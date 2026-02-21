#include <cerrno>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include <dicom.h>

namespace {

struct CliOptions {
	std::size_t max_print_chars{80};
	bool include_offset{true};
	bool with_filename{false};
	std::vector<std::string> paths{};
};

void print_usage(const char* prog) {
	std::cerr
	    << "Usage: " << prog << " [--max-print-chars N] [--no-offset] [--with-filename] <file> [file...]\n"
	    << "\n"
	    << "Options:\n"
	    << "  --max-print-chars N  Max printable width per line before VALUE truncation (default: 80)\n"
	    << "  --no-offset          Hide OFFSET column in dump output\n"
	    << "  --with-filename      Prefix each output line with 'filename:'\n"
	    << "  -h, --help           Show this help\n";
}

bool parse_max_print_chars(const std::string& text, std::size_t& out_value) {
	if (text.empty()) {
		return false;
	}
	char* end = nullptr;
	errno = 0;
	const auto parsed = std::strtoull(text.c_str(), &end, 10);
	if (errno != 0 || end == nullptr || *end != '\0') {
		return false;
	}
	if (parsed > std::numeric_limits<std::size_t>::max()) {
		return false;
	}
	out_value = static_cast<std::size_t>(parsed);
	return true;
}

bool parse_args(int argc, char** argv, CliOptions& opts) {
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "-h" || arg == "--help") {
			print_usage(argv[0]);
			return false;
		}
		if (arg == "--no-offset") {
			opts.include_offset = false;
			continue;
		}
		if (arg == "--with-filename") {
			opts.with_filename = true;
			continue;
		}
		if (arg == "--max-print-chars") {
			if (i + 1 >= argc) {
				std::cerr << "Missing value for --max-print-chars\n";
				print_usage(argv[0]);
				return false;
			}
			std::size_t value = 0;
			if (!parse_max_print_chars(argv[i + 1], value)) {
				std::cerr << "Invalid --max-print-chars value: " << argv[i + 1] << '\n';
				print_usage(argv[0]);
				return false;
			}
			opts.max_print_chars = value;
			++i;
			continue;
		}
		if (!arg.empty() && arg[0] == '-') {
			std::cerr << "Unknown option: " << arg << '\n';
			print_usage(argv[0]);
			return false;
		}
		opts.paths.push_back(arg);
	}

	if (opts.paths.empty()) {
		print_usage(argv[0]);
		return false;
	}
	return true;
}

void write_prefixed_lines(const std::string& path, const std::string& text) {
	std::size_t start = 0;
	while (start < text.size()) {
		const auto end = text.find('\n', start);
		if (end == std::string::npos) {
			std::cout << path << ':' << text.substr(start) << '\n';
			return;
		}
		std::cout << path << ':' << text.substr(start, end - start + 1);
		start = end + 1;
	}
}

void write_dump(const std::string& path, const std::string& text, bool with_filename) {
	if (with_filename) {
		write_prefixed_lines(path, text);
		return;
	}

	std::cout << text;
	if (!text.empty() && text.back() != '\n') {
		std::cout << '\n';
	}
}

}  // namespace

int main(int argc, char** argv) {
	CliOptions opts{};
	if (!parse_args(argc, argv, opts)) {
		return 1;
	}

	const bool with_filename = opts.with_filename || opts.paths.size() > 1;
	int exit_code = 0;
	for (const auto& path : opts.paths) {
		try {
			auto file = dicom::read_file(path);
			if (!file) {
				std::cerr << "dicomdump: " << path << ": failed to open file\n";
				exit_code = 1;
				continue;
			}
			const auto text = file->dump(opts.max_print_chars, opts.include_offset);
			write_dump(path, text, with_filename);
		} catch (const std::exception& ex) {
			std::cerr << "dicomdump: " << path << ": " << ex.what() << '\n';
			exit_code = 1;
		}
	}
	return exit_code;
}
