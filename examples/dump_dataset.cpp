#include <iostream>
#include <string>
#include <sstream>
#include <vector>
#include <fmt/format.h>

#include <dicom.h>

using namespace dicom::literals;

template <typename T>
void print_vector(std::ostringstream& oss, const std::vector<T>& v) {
	if (v.empty()) {
		oss << "[]";
		return;
	}
	oss << '[';
	for (size_t i = 0; i < v.size(); ++i) {
		if (i) oss << ',';
		oss << v[i];
	}
	oss << ']';
}

int main(int argc, char** argv) {
	if (argc < 2) {
		std::cerr << "Usage: " << argv[0] << " <dicom-file>\n";
		return 1;
	}

	const std::string path = argv[1];
	auto ds = dicom::read_file(path);
	if (!ds) {
		std::cerr << "Failed to read file: " << path << "\n";
		return 1;
	}

	constexpr std::size_t kMaxLine = 160;

	for (auto& elem : *ds) {
		std::ostringstream oss;
		const auto tag = elem.tag();
		const auto vr = elem.vr();
		const auto keyword = dicom::lookup::tag_to_keyword(tag.value());
		oss << fmt::format("({:04X},{:04X}) VR={} len={} off={} vm={} keyword={}",
		                   tag.group(), tag.element(), vr.str(), elem.length(),
		                   elem.offset(), elem.vm(), keyword.empty() ? "-" : keyword);

		oss << " value=";
		bool printed = false;

		switch (static_cast<std::uint16_t>(vr)) {
		case dicom::VR::SS_val:
		case dicom::VR::US_val:
		case dicom::VR::SL_val:
		case dicom::VR::UL_val: {
				if (elem.vm() > 1) {
					if (auto v = elem.to_long_vector()) { print_vector(oss, *v); printed = true; }
				} else if (auto v = elem.to_long()) { oss << *v; printed = true; }
				break;
			}
			case dicom::VR::SV_val:
			case dicom::VR::UV_val: {
				if (elem.vm() > 1) {
					if (auto v = elem.to_longlong_vector()) { print_vector(oss, *v); printed = true; }
				} else if (auto v = elem.to_longlong()) { oss << *v; printed = true; }
				break;
			}
			case dicom::VR::FL_val:
		case dicom::VR::FD_val:
		case dicom::VR::DS_val:
		case dicom::VR::IS_val: {
			if (elem.vm() > 1) {
				if (auto v = elem.to_double_vector()) { print_vector(oss, *v); printed = true; }
			} else if (auto v = elem.to_double()) { oss << *v; printed = true; }
			break;
		}
		case dicom::VR::AT_val: {
			if (elem.vm() > 1) {
				if (auto v = elem.to_tag_vector()) {
					oss << '[';
					for (size_t i = 0; i < v->size(); ++i) {
						if (i) oss << ',';
						oss << fmt::format("({:04X},{:04X})", (*v)[i].group(), (*v)[i].element());
					}
					oss << ']';
					printed = true;
				}
			} else if (auto v = elem.to_tag()) {
				oss << fmt::format("({:04X},{:04X})", v->group(), v->element());
				printed = true;
			}
			break;
		}
		default:
			break;
		}

		if (!printed) oss << "[TODO]";

		auto line = oss.str();
		if (line.size() > kMaxLine) {
			line = line.substr(0, kMaxLine - 3) + "...";
		}
		std::cout << line << "\n";
	}

	return 0;
}
