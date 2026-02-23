#include <exception>
#include <iostream>
#include <string>

#include <dicom.h>

namespace {

struct CliOptions {
	std::string input_path{};
	std::string output_path{};
	std::string transfer_syntax_text{};
};

void print_transfer_syntax_list() {
	std::cerr << "\nAvailable Transfer Syntax UIDs (keyword = UID):\n";
	for (const auto& entry : dicom::kUidRegistry) {
		if (entry.uid_type != dicom::UidType::TransferSyntax) {
			continue;
		}
		if (entry.keyword.empty()) {
			std::cerr << "  " << entry.value << '\n';
			continue;
		}
		std::cerr << "  " << entry.keyword << " = " << entry.value << '\n';
	}
}

void print_usage(const char* prog, bool include_transfer_syntax_list) {
	std::cerr
	    << "Usage: " << prog << " <input.dcm> <output.dcm> <transfer-syntax>\n"
	    << "\n"
	    << "Examples:\n"
	    << "  " << prog << " input.dcm output.dcm ExplicitVRLittleEndian\n"
	    << "  " << prog << " input.dcm output.dcm 1.2.840.10008.1.2\n"
	    << "\n"
	    << "The transfer-syntax argument accepts a DICOM UID keyword or dotted UID value.\n";
	if (include_transfer_syntax_list) {
		print_transfer_syntax_list();
	}
}

bool parse_args(int argc, char** argv, CliOptions& opts, bool& help_shown) {
	help_shown = false;
	if (argc == 2) {
		const std::string arg = argv[1];
		if (arg == "-h" || arg == "--help") {
			print_usage(argv[0], true);
			help_shown = true;
			return false;
		}
	}

	if (argc != 4) {
		print_usage(argv[0], false);
		return false;
	}

	opts.input_path = argv[1];
	opts.output_path = argv[2];
	opts.transfer_syntax_text = argv[3];
	return true;
}

dicom::uid::WellKnown resolve_transfer_syntax(const std::string& text) {
	const auto uid = dicom::uid::lookup(text);
	if (!uid) {
		throw std::invalid_argument("Unknown UID: " + text);
	}
	if (uid->uid_type() != dicom::UidType::TransferSyntax) {
		throw std::invalid_argument("UID is not a Transfer Syntax: " + text);
	}
	return *uid;
}

} // namespace

int main(int argc, char** argv) {
	CliOptions opts{};
	bool help_shown = false;
	if (!parse_args(argc, argv, opts, help_shown)) {
		return help_shown ? 0 : 1;
	}

	try {
		auto file = dicom::read_file(opts.input_path);
		if (!file) {
			std::cerr << "dicomconv: failed to read input file: " << opts.input_path << '\n';
			return 1;
		}

		const auto transfer_syntax = resolve_transfer_syntax(opts.transfer_syntax_text);
		file->set_transfer_syntax(transfer_syntax);
		file->write_file(opts.output_path);
		return 0;
	} catch (const std::exception& ex) {
		std::cerr << "dicomconv: " << ex.what() << '\n';
		return 1;
	}
}
