#include <dicom.h>

#include <exception>
#include <iostream>
#include <limits>
#include <string>

namespace {

bool parse_max_print_chars(const std::string& text, std::size_t& out_value) {
	if (text.empty()) {
		return false;
	}
	unsigned long long parsed = 0;
	try {
		parsed = std::stoull(text);
	} catch (...) {
		return false;
	}
	if (parsed > std::numeric_limits<std::size_t>::max()) {
		return false;
	}
	out_value = static_cast<std::size_t>(parsed);
	return true;
}

}  // namespace

int main(int argc, char** argv) {
	if (argc != 2 && argc != 3) {
		std::cerr << "Usage: " << argv[0] << " <dicom-file> [max-print-chars]\n";
		return 1;
	}

	std::size_t max_print_chars = 80;
	if (argc == 3 && !parse_max_print_chars(argv[2], max_print_chars)) {
		std::cerr << "Invalid max-print-chars: " << argv[2] << '\n';
		return 1;
	}

	try {
		auto file = dicom::read_file(argv[1]);
		std::cout << file->dump(max_print_chars);
	} catch (const std::exception& ex) {
		std::cerr << "Failed to read DICOM file: " << ex.what() << std::endl;
		return 1;
	}

	return 0;
}
