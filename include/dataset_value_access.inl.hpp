#pragma once

// This file is included from dicom.h inside namespace dicom.

namespace detail {

inline Tag tag_key_from_text_cpp(std::string_view key, const char* api_name) {
	if (key.find('.') != std::string_view::npos) {
		throw std::invalid_argument(std::string(api_name) +
		    " does not support nested tag-path strings");
	}
	return Tag(key);
}

inline std::optional<std::string> dataelement_get_owned_string_cpp(
    const DataElement& element) {
	if (!element.is_present()) {
		return std::nullopt;
	}
	if (element.vr().uses_specific_character_set()) {
		return element.to_utf8_string();
	}
	if (auto value = element.to_string_view()) {
		return std::string(*value);
	}
	return std::nullopt;
}

inline std::optional<std::vector<std::string>> dataelement_get_owned_strings_cpp(
    const DataElement& element) {
	if (!element.is_present()) {
		return std::nullopt;
	}
	if (element.vr().uses_specific_character_set()) {
		return element.to_utf8_strings();
	}
	auto views = element.to_string_views();
	if (!views) {
		return std::nullopt;
	}
	std::vector<std::string> values;
	values.reserve(views->size());
	for (auto view : *views) {
		values.emplace_back(view);
	}
	return values;
}

inline bool dataelement_set_string_values_cpp(
    DataElement& element, std::span<const std::string_view> values) {
	return element.vr().uses_specific_character_set() ? element.from_utf8_views(values)
	                                                  : element.from_string_views(values);
}

template <typename T>
std::optional<T> dataelement_get_value_cpp(const DataElement& element) {
	if constexpr (std::is_same_v<T, int>) {
		return element.to_int();
	} else if constexpr (std::is_same_v<T, long>) {
		return element.to_long();
	} else if constexpr (std::is_same_v<T, long long>) {
		return element.to_longlong();
	} else if constexpr (std::is_same_v<T, double>) {
		return element.to_double();
	} else if constexpr (std::is_same_v<T, Tag>) {
		return element.to_tag();
	} else if constexpr (std::is_same_v<T, std::string_view>) {
		return element.to_string_view();
	} else if constexpr (std::is_same_v<T, std::string>) {
		return dataelement_get_owned_string_cpp(element);
	} else if constexpr (std::is_same_v<T, PersonName>) {
		return element.to_person_name();
	} else if constexpr (std::is_same_v<T, std::vector<int>>) {
		return element.to_int_vector();
	} else if constexpr (std::is_same_v<T, std::vector<long>>) {
		return element.to_long_vector();
	} else if constexpr (std::is_same_v<T, std::vector<long long>>) {
		return element.to_longlong_vector();
	} else if constexpr (std::is_same_v<T, std::vector<double>>) {
		return element.to_double_vector();
	} else if constexpr (std::is_same_v<T, std::vector<Tag>>) {
		return element.to_tag_vector();
	} else if constexpr (std::is_same_v<T, std::vector<std::string_view>>) {
		return element.to_string_views();
	} else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
		return dataelement_get_owned_strings_cpp(element);
	} else if constexpr (std::is_same_v<T, std::vector<PersonName>>) {
		return element.to_person_names();
	} else {
		static_assert(dependent_false_v<T>, "Unsupported dicom::DataSet::get_value<T>() type");
	}
}

}  // namespace detail

template <typename T>
std::optional<T> DataSet::get_value(Tag tag) const {
	return detail::dataelement_get_value_cpp<T>(get_dataelement(tag));
}

template <typename T>
std::optional<T> DataSet::get_value(std::string_view tag_path) const {
	return detail::dataelement_get_value_cpp<T>(get_dataelement(tag_path));
}

template <typename T>
T DataSet::get_value(Tag tag, T default_value) const {
	return get_value<T>(tag).value_or(std::move(default_value));
}

template <typename T>
T DataSet::get_value(std::string_view tag_path, T default_value) const {
	return get_value<T>(tag_path).value_or(std::move(default_value));
}

template <typename AssignFn>
bool DataSet::set_value_impl(Tag tag, VR vr, AssignFn&& assign_fn) {
	auto& existing = get_dataelement(tag);
	if (existing.is_present()) {
		if (vr == VR::None || existing.vr() == vr) {
			return assign_fn(existing);
		}
		if (existing.vr().is_sequence() || existing.vr().is_pixel_sequence() ||
		    vr.is_sequence() || vr.is_pixel_sequence()) {
			return false;
		}
		DataElement& target = add_dataelement(tag, vr);
		return assign_fn(target);
	}

	if (vr != VR::None) {
		if (vr.is_sequence() || vr.is_pixel_sequence()) {
			return false;
		}
	}

	DataElement& target = add_dataelement(tag, vr);
	return assign_fn(target);
}

inline bool DataSet::set_value(Tag tag, std::string_view value) {
	return set_value_impl(tag, VR::None,
	    [&](DataElement& element) {
		    return element.vr().uses_specific_character_set() ? element.from_utf8_view(value)
		                                                      : element.from_string_view(value);
	    });
}

inline bool DataSet::set_value(Tag tag, const PersonName& value) {
	return set_value_impl(
	    tag, VR::None, [&](DataElement& element) { return element.from_person_name(value); });
}

inline bool DataSet::set_value(Tag tag, std::span<const int> values) {
	return set_value_impl(
	    tag, VR::None, [&](DataElement& element) { return element.from_int_vector(values); });
}

inline bool DataSet::set_value(Tag tag, std::span<const long> values) {
	return set_value_impl(
	    tag, VR::None, [&](DataElement& element) { return element.from_long_vector(values); });
}

inline bool DataSet::set_value(Tag tag, std::span<const long long> values) {
	return set_value_impl(tag, VR::None,
	    [&](DataElement& element) { return element.from_longlong_vector(values); });
}

inline bool DataSet::set_value(Tag tag, std::span<const double> values) {
	return set_value_impl(
	    tag, VR::None, [&](DataElement& element) { return element.from_double_vector(values); });
}

inline bool DataSet::set_value(Tag tag, std::span<const Tag> values) {
	return set_value_impl(
	    tag, VR::None, [&](DataElement& element) { return element.from_tag_vector(values); });
}

inline bool DataSet::set_value(Tag tag, std::span<const std::string_view> values) {
	return set_value_impl(tag, VR::None,
	    [&](DataElement& element) {
		    return detail::dataelement_set_string_values_cpp(element, values);
	    });
}

inline bool DataSet::set_value(Tag tag, std::span<const PersonName> values) {
	return set_value_impl(
	    tag, VR::None, [&](DataElement& element) { return element.from_person_names(values); });
}

inline bool DataSet::set_value(Tag tag, VR vr, std::string_view value) {
	return set_value_impl(tag, vr,
	    [&](DataElement& element) {
		    return element.vr().uses_specific_character_set() ? element.from_utf8_view(value)
		                                                      : element.from_string_view(value);
	    });
}

inline bool DataSet::set_value(Tag tag, VR vr, const PersonName& value) {
	return set_value_impl(
	    tag, vr, [&](DataElement& element) { return element.from_person_name(value); });
}

inline bool DataSet::set_value(Tag tag, VR vr, std::span<const int> values) {
	return set_value_impl(
	    tag, vr, [&](DataElement& element) { return element.from_int_vector(values); });
}

inline bool DataSet::set_value(Tag tag, VR vr, std::span<const long> values) {
	return set_value_impl(
	    tag, vr, [&](DataElement& element) { return element.from_long_vector(values); });
}

inline bool DataSet::set_value(Tag tag, VR vr, std::span<const long long> values) {
	return set_value_impl(
	    tag, vr, [&](DataElement& element) { return element.from_longlong_vector(values); });
}

inline bool DataSet::set_value(Tag tag, VR vr, std::span<const double> values) {
	return set_value_impl(
	    tag, vr, [&](DataElement& element) { return element.from_double_vector(values); });
}

inline bool DataSet::set_value(Tag tag, VR vr, std::span<const Tag> values) {
	return set_value_impl(
	    tag, vr, [&](DataElement& element) { return element.from_tag_vector(values); });
}

inline bool DataSet::set_value(Tag tag, VR vr, std::span<const std::string_view> values) {
	return set_value_impl(tag, vr,
	    [&](DataElement& element) {
		    return detail::dataelement_set_string_values_cpp(element, values);
	    });
}

inline bool DataSet::set_value(Tag tag, VR vr, std::span<const PersonName> values) {
	return set_value_impl(
	    tag, vr, [&](DataElement& element) { return element.from_person_names(values); });
}

#define DICOMSDL_DEFINE_DATASET_SET_VALUE_SCALAR(cpp_type, method_name)                             \
	inline bool DataSet::set_value(Tag tag, cpp_type value) {                                         \
		return set_value_impl(                                                                         \
		    tag, VR::None, [&](DataElement& element) { return element.method_name(value); });          \
	}                                                                                                  \
	inline bool DataSet::set_value(std::string_view key, cpp_type value) {                            \
		return set_value(detail::tag_key_from_text_cpp(key, "DataSet::set_value"), value);            \
	}                                                                                                  \
	inline bool DataSet::set_value(Tag tag, VR vr, cpp_type value) {                                  \
		return set_value_impl(                                                                         \
		    tag, vr, [&](DataElement& element) { return element.method_name(value); });                \
	}                                                                                                  \
	inline bool DataSet::set_value(std::string_view key, VR vr, cpp_type value) {                     \
		return set_value(detail::tag_key_from_text_cpp(key, "DataSet::set_value"), vr, value);        \
	}

#define DICOMSDL_DEFINE_DATASET_SET_VALUE_SPAN(cpp_type)                                             \
	inline bool DataSet::set_value(std::string_view key, std::span<const cpp_type> values) {         \
		return set_value(detail::tag_key_from_text_cpp(key, "DataSet::set_value"), values);          \
	}                                                                                                  \
	inline bool DataSet::set_value(                                                                   \
	    std::string_view key, VR vr, std::span<const cpp_type> values) {                              \
		return set_value(detail::tag_key_from_text_cpp(key, "DataSet::set_value"), vr, values);      \
	}

DICOMSDL_DEFINE_DATASET_SET_VALUE_SCALAR(int, from_int)
DICOMSDL_DEFINE_DATASET_SET_VALUE_SCALAR(long, from_long)
DICOMSDL_DEFINE_DATASET_SET_VALUE_SCALAR(long long, from_longlong)
DICOMSDL_DEFINE_DATASET_SET_VALUE_SCALAR(double, from_double)
DICOMSDL_DEFINE_DATASET_SET_VALUE_SCALAR(Tag, from_tag)

DICOMSDL_DEFINE_DATASET_SET_VALUE_SPAN(int)
DICOMSDL_DEFINE_DATASET_SET_VALUE_SPAN(long)
DICOMSDL_DEFINE_DATASET_SET_VALUE_SPAN(long long)
DICOMSDL_DEFINE_DATASET_SET_VALUE_SPAN(double)
DICOMSDL_DEFINE_DATASET_SET_VALUE_SPAN(Tag)
DICOMSDL_DEFINE_DATASET_SET_VALUE_SPAN(std::string_view)
DICOMSDL_DEFINE_DATASET_SET_VALUE_SPAN(PersonName)

inline bool DataSet::set_value(std::string_view key, std::string_view value) {
	return set_value(detail::tag_key_from_text_cpp(key, "DataSet::set_value"), value);
}

inline bool DataSet::set_value(std::string_view key, const PersonName& value) {
	return set_value(detail::tag_key_from_text_cpp(key, "DataSet::set_value"), value);
}

inline bool DataSet::set_value(
    std::string_view key, VR vr, std::string_view value) {
	return set_value(detail::tag_key_from_text_cpp(key, "DataSet::set_value"), vr, value);
}

inline bool DataSet::set_value(
    std::string_view key, VR vr, const PersonName& value) {
	return set_value(detail::tag_key_from_text_cpp(key, "DataSet::set_value"), vr, value);
}

template <typename T>
std::optional<T> DicomFile::get_value(Tag tag) const {
	return dataset().template get_value<T>(tag);
}

template <typename T>
std::optional<T> DicomFile::get_value(std::string_view tag_path) const {
	return dataset().template get_value<T>(tag_path);
}

template <typename T>
T DicomFile::get_value(Tag tag, T default_value) const {
	return dataset().template get_value<T>(tag, std::move(default_value));
}

template <typename T>
T DicomFile::get_value(std::string_view tag_path, T default_value) const {
	return dataset().template get_value<T>(tag_path, std::move(default_value));
}

#define DICOMSDL_DEFINE_DICOMFILE_SET_VALUE_SCALAR(cpp_type)                                         \
	inline bool DicomFile::set_value(Tag tag, cpp_type value) {                                       \
		return root_dataset_.set_value(tag, value);                                                    \
	}                                                                                                  \
	inline bool DicomFile::set_value(std::string_view key, cpp_type value) {                          \
		return root_dataset_.set_value(key, value);                                                    \
	}                                                                                                  \
	inline bool DicomFile::set_value(Tag tag, VR vr, cpp_type value) {                                \
		return root_dataset_.set_value(tag, vr, value);                                                \
	}                                                                                                  \
	inline bool DicomFile::set_value(std::string_view key, VR vr, cpp_type value) {                   \
		return root_dataset_.set_value(key, vr, value);                                                \
	}

#define DICOMSDL_DEFINE_DICOMFILE_SET_VALUE_SPAN(cpp_type)                                           \
	inline bool DicomFile::set_value(Tag tag, std::span<const cpp_type> values) {                     \
		return root_dataset_.set_value(tag, values);                                                   \
	}                                                                                                  \
	inline bool DicomFile::set_value(std::string_view key, std::span<const cpp_type> values) {       \
		return root_dataset_.set_value(key, values);                                                   \
	}                                                                                                  \
	inline bool DicomFile::set_value(Tag tag, VR vr, std::span<const cpp_type> values) {              \
		return root_dataset_.set_value(tag, vr, values);                                               \
	}                                                                                                  \
	inline bool DicomFile::set_value(                                                                  \
	    std::string_view key, VR vr, std::span<const cpp_type> values) {                              \
		return root_dataset_.set_value(key, vr, values);                                               \
	}

DICOMSDL_DEFINE_DICOMFILE_SET_VALUE_SCALAR(int)
DICOMSDL_DEFINE_DICOMFILE_SET_VALUE_SCALAR(long)
DICOMSDL_DEFINE_DICOMFILE_SET_VALUE_SCALAR(long long)
DICOMSDL_DEFINE_DICOMFILE_SET_VALUE_SCALAR(double)
DICOMSDL_DEFINE_DICOMFILE_SET_VALUE_SCALAR(Tag)
DICOMSDL_DEFINE_DICOMFILE_SET_VALUE_SCALAR(std::string_view)
DICOMSDL_DEFINE_DICOMFILE_SET_VALUE_SCALAR(const PersonName&)

DICOMSDL_DEFINE_DICOMFILE_SET_VALUE_SPAN(int)
DICOMSDL_DEFINE_DICOMFILE_SET_VALUE_SPAN(long)
DICOMSDL_DEFINE_DICOMFILE_SET_VALUE_SPAN(long long)
DICOMSDL_DEFINE_DICOMFILE_SET_VALUE_SPAN(double)
DICOMSDL_DEFINE_DICOMFILE_SET_VALUE_SPAN(Tag)
DICOMSDL_DEFINE_DICOMFILE_SET_VALUE_SPAN(std::string_view)
DICOMSDL_DEFINE_DICOMFILE_SET_VALUE_SPAN(PersonName)

#undef DICOMSDL_DEFINE_DICOMFILE_SET_VALUE_SPAN
#undef DICOMSDL_DEFINE_DICOMFILE_SET_VALUE_SCALAR
#undef DICOMSDL_DEFINE_DATASET_SET_VALUE_SPAN
#undef DICOMSDL_DEFINE_DATASET_SET_VALUE_SCALAR
