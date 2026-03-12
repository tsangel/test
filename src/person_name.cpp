#include "dicom.h"

#include <algorithm>

namespace dicom {

namespace {

template <typename AssignFn>
bool split_preserve_empty(std::string_view text, char delimiter, std::size_t max_parts,
    AssignFn&& assign_part) {
	std::size_t start = 0;
	std::size_t index = 0;
	while (true) {
		if (index >= max_parts) {
			return false;
		}
		const auto pos = text.find(delimiter, start);
		const auto part = pos == std::string_view::npos
		                      ? text.substr(start)
		                      : text.substr(start, pos - start);
		assign_part(index, part);
		++index;
		if (pos == std::string_view::npos) {
			return true;
		}
		start = pos + 1;
	}
}

std::optional<PersonNameGroup> parse_person_name_group(std::string_view raw_group) {
	if (raw_group.empty()) {
		return std::nullopt;
	}
	PersonNameGroup group;
	std::size_t component_count = 0;
	if (!split_preserve_empty(raw_group, '^', group.components.size(),
	        [&](std::size_t index, std::string_view part) {
		        group.components[index].assign(part.data(), part.size());
		        component_count = index + 1;
	        })) {
		return std::nullopt;
	}
	group.explicit_component_count_ = static_cast<std::uint8_t>(component_count);
	return group;
}

void append_person_name_group_to_string(const PersonNameGroup& group, std::string& out) {
	std::size_t last_non_empty = group.components.size();
	while (last_non_empty > 0 && group.components[last_non_empty - 1].empty()) {
		--last_non_empty;
	}
	for (std::size_t i = 0; i < last_non_empty; ++i) {
		if (i != 0) {
			out.push_back('^');
		}
		out += group.components[i];
	}
}

}  // namespace

bool PersonNameGroup::empty() const noexcept {
	return std::all_of(components.begin(), components.end(),
	    [](const std::string& component) { return component.empty(); });
}

std::string PersonNameGroup::to_dicom_string() const {
	std::size_t last_non_empty = components.size();
	while (last_non_empty > 0 && components[last_non_empty - 1].empty()) {
		--last_non_empty;
	}
	const auto component_count =
	    std::max<std::size_t>(last_non_empty, explicit_component_count_);
	if (component_count == 0) {
		return {};
	}
	std::string out;
	for (std::size_t i = 0; i < component_count; ++i) {
		if (i != 0) {
			out.push_back('^');
		}
		out += components[i];
	}
	return out;
}

bool PersonName::empty() const noexcept {
	const auto group_empty = [](const std::optional<PersonNameGroup>& group) {
		return !group || group->empty();
	};
	return group_empty(alphabetic) && group_empty(ideographic) && group_empty(phonetic);
}

std::string PersonName::to_dicom_string() const {
	const std::array<const std::optional<PersonNameGroup>*, 3> groups{
	    &alphabetic, &ideographic, &phonetic};
	std::size_t last_non_empty = groups.size();
	while (last_non_empty > 0 &&
	       (!*groups[last_non_empty - 1] || (*groups[last_non_empty - 1])->empty())) {
		--last_non_empty;
	}
	const auto group_count = std::max<std::size_t>(last_non_empty, explicit_group_count_);
	if (group_count == 0) {
		return {};
	}
	std::string out;
	for (std::size_t i = 0; i < group_count; ++i) {
		if (i != 0) {
			out.push_back('=');
		}
		if (*groups[i]) {
			out += (*groups[i])->to_dicom_string();
		}
	}
	return out;
}

std::optional<PersonName> PersonName::parse(std::string_view utf8_value) {
	PersonName parsed;
	bool invalid = false;
	if (!split_preserve_empty(utf8_value, '=', 3,
	        [&](std::size_t index, std::string_view raw_group) {
		        parsed.explicit_group_count_ =
		            static_cast<std::uint8_t>(std::max<std::size_t>(parsed.explicit_group_count_, index + 1));
		        if (raw_group.empty()) {
			        return;
		        }
		        auto group = parse_person_name_group(raw_group);
		        if (!group) {
			        invalid = true;
			        return;
		        }
		        switch (index) {
		        case 0:
			        parsed.alphabetic = std::move(*group);
			        break;
		        case 1:
			        parsed.ideographic = std::move(*group);
			        break;
		        case 2:
			        parsed.phonetic = std::move(*group);
			        break;
		        default:
			        break;
		        }
	        }) || invalid) {
		return std::nullopt;
	}
	return parsed;
}

std::optional<std::vector<PersonName>> PersonName::parse_many(
    std::span<const std::string> utf8_values) {
	std::vector<PersonName> out;
	out.reserve(utf8_values.size());
	for (const auto& value : utf8_values) {
		auto parsed = parse(value);
		if (!parsed) {
			return std::nullopt;
		}
		out.push_back(std::move(*parsed));
	}
	return out;
}

std::optional<std::vector<PersonName>> PersonName::parse_many(
    std::span<const std::string_view> utf8_values) {
	std::vector<PersonName> out;
	out.reserve(utf8_values.size());
	for (const auto value : utf8_values) {
		auto parsed = parse(value);
		if (!parsed) {
			return std::nullopt;
		}
		out.push_back(std::move(*parsed));
	}
	return out;
}

}  // namespace dicom
