#include <dicom.h>

#include <exception>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t kMaxLineLength = 160;

template <typename T>
std::string format_vector(const std::vector<T>& values) {
	if (values.empty()) {
		return "[]";
	}
	std::ostringstream oss;
	oss << '[';
	for (std::size_t i = 0; i < values.size(); ++i) {
		if (i) {
			oss << ',';
		}
		oss << values[i];
	}
	oss << ']';
	return oss.str();
}

std::string format_tag_vector(const std::vector<dicom::Tag>& values) {
	if (values.empty()) {
		return "[]";
	}
	std::ostringstream oss;
	oss << '[';
	for (std::size_t i = 0; i < values.size(); ++i) {
		if (i) {
			oss << ',';
		}
		oss << values[i].to_string();
	}
	oss << ']';
	return oss.str();
}

std::optional<std::string> describe_ui(const dicom::DataElement& element) {
	auto text = element.to_uid_string();
	if (!text) return std::nullopt;
	if (auto wk = dicom::uid::from_value(*text)) {
		std::ostringstream oss;
		oss << "UID(value='" << wk->value() << "', keyword='"
		    << (wk->keyword().empty() ? "-" : std::string(wk->keyword()))
		    << "', name='" << wk->name() << "', type='" << wk->type() << "')";
		return oss.str();
	}
	return std::string("UID(value='") + *text + "')";
}

std::optional<std::string> describe_numeric(const dicom::DataElement& element) {
	const auto vr = element.vr();
	const int vm = element.vm();
	switch (static_cast<std::uint16_t>(vr)) {
	case dicom::VR::SS_val:
	case dicom::VR::US_val:
	case dicom::VR::SL_val:
	case dicom::VR::UL_val: {
		if (vm > 1) {
			auto vec = element.to_long_vector();
			if (!vec) return std::nullopt;
			return format_vector(*vec);
		}
		auto value = element.to_long();
		return value ? std::optional<std::string>(std::to_string(*value)) : std::nullopt;
	}
	case dicom::VR::SV_val:
	case dicom::VR::UV_val: {
		if (vm > 1) {
			auto vec = element.to_longlong_vector();
			if (!vec) return std::nullopt;
			return format_vector(*vec);
		}
		auto value = element.to_longlong();
		return value ? std::optional<std::string>(std::to_string(*value)) : std::nullopt;
	}
	case dicom::VR::FL_val:
	case dicom::VR::FD_val:
	case dicom::VR::DS_val:
	case dicom::VR::IS_val: {
		if (vm > 1) {
			auto vec = element.to_double_vector();
			if (!vec) return std::nullopt;
			return format_vector(*vec);
		}
		auto value = element.to_double();
		return value ? std::optional<std::string>(std::to_string(*value)) : std::nullopt;
	}
	case dicom::VR::UI_val:
		return describe_ui(element);
	case dicom::VR::AT_val: {
		if (vm > 1) {
			auto vec = element.to_tag_vector();
			if (!vec) return std::nullopt;
			return format_tag_vector(*vec);
		}
		auto tag = element.to_tag();
		return tag ? std::optional<std::string>(tag->to_string()) : std::nullopt;
	}
	default:
		return std::nullopt;
	}
}

std::string describe_element(const dicom::DataElement& element) {
	const auto tag = element.tag();
	const auto keyword = dicom::lookup::tag_to_keyword(tag.value());
	std::ostringstream oss;
	oss << tag.to_string()
	    << " VR=" << element.vr().str()
	    << " len=" << element.length()
	    << " off=" << element.offset()
	    << " vm=" << element.vm()
	    << " keyword=" << (keyword.empty() ? "-" : std::string(keyword));
	if (auto value = describe_numeric(element)) {
		oss << " value=" << *value;
	} else if (element.vr().is_string()) {
		if (auto strings = element.to_string_views()) {
			oss << " value=";
			if (strings->size() == 1) {
				oss << '"' << std::string(strings->front()) << '"';
			} else {
				oss << '[';
				for (std::size_t i = 0; i < strings->size(); ++i) {
					if (i) oss << ',';
					oss << '"' << std::string((*strings)[i]) << '"';
				}
				oss << ']';
			}
		}
	}
	std::string line = oss.str();
	if (line.size() > kMaxLineLength) {
		line.resize(kMaxLineLength - 3);
		line.append("...");
	}
	return line;
}

void dump_dataset(const dicom::DataSet& dataset, std::size_t indent = 0);
void dump_pixel_sequence(const dicom::PixelSequence& pixseq, std::size_t indent);

void dump_sequence(const dicom::Sequence& sequence, std::size_t indent) {
	const std::string indent_str(indent, ' ');
	for (int idx = 0; idx < sequence.size(); ++idx) {
		const dicom::DataSet* item = sequence.get_dataset(static_cast<std::size_t>(idx));
		if (!item) {
			continue;
		}
		std::cout << indent_str << "Item[" << idx << "] {\n";
		dump_dataset(*item, indent + 2);
		std::cout << indent_str << "}\n";
	}
}

void dump_pixel_sequence(const dicom::PixelSequence& pixseq, std::size_t indent) {
	const std::string indent_str(indent, ' ');
	const auto bot_offset = pixseq.basic_offset_table_offset();
	const auto bot_count = pixseq.basic_offset_table_count();

	if (bot_count == 0) {
		std::cout << indent_str << "BasicOffsetTable: none\n";
	} else {
		std::cout << indent_str << "BasicOffsetTable: offset=0x"
		          << std::hex << bot_offset << std::dec
		          << " count=" << bot_count << '\n';
	}

	const auto frame_count = pixseq.number_of_frames();
	for (std::size_t i = 0; i < frame_count; ++i) {
		const auto* frame = pixseq.frame(i);
		if (!frame) continue;
		const auto& frags = frame->fragments();
		std::cout << indent_str << "Frame[" << i << "] fragments=" << frags.size() << '\n';
		for (std::size_t j = 0; j < frags.size(); ++j) {
			const auto& frag = frags[j];
			std::cout << indent_str << "  frag[" << j << "] offset=0x"
			          << std::hex << frag.offset << std::dec
			          << " length=" << frag.length << '\n';
		}
	}
}

void dump_dataset(const dicom::DataSet& dataset, std::size_t indent) {
	const std::string indent_str(indent, ' ');
	for (const auto& element : dataset) {
		std::cout << indent_str << describe_element(element) << '\n';
		if (const dicom::Sequence* seq = element.sequence()) {
			dump_sequence(*seq, indent + 2);
		} else if (const dicom::PixelSequence* pixseq = element.pixel_sequence()) {
			dump_pixel_sequence(*pixseq, indent + 2);
		}
	}
}

}  // namespace

int main(int argc, char** argv) {
	if (argc != 2) {
		std::cerr << "Usage: " << argv[0] << " <dicom-file>" << std::endl;
		return 1;
	}

	try {
		auto dataset = dicom::read_file(argv[1]);
		dump_dataset(*dataset);
	} catch (const std::exception& ex) {
		std::cerr << "Failed to read DICOM file: " << ex.what() << std::endl;
		return 1;
	}

	return 0;
}
