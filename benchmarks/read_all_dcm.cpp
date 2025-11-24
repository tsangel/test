// Benchmark: read all DICOM files under a directory using dicom::read_file (new build).
// Usage:
//   ./read_all_dcm_cpp [root] [repeat] [source]
//   root   : directory to scan recursively (default: /Users/tsangel/Documents/workspace.dev/sample/ncc/3121/pt)
//   repeat : number of times to iterate the whole set (default: 10)
//   source : file | memory (default: file) — memory preloads each file into RAM before parsing

#include "dicom.h"

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <fstream>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
	const fs::path default_root{"/Users/tsangel/Documents/workspace.dev/sample/ncc/3121/pt"};
	const fs::path root = (argc >= 2) ? fs::path(argv[1]) : default_root;
	const int repeat = (argc >= 3) ? std::max(1, std::atoi(argv[2])) : 10;
	const std::string source = (argc >= 4) ? argv[3] : "file";

	std::cout << "impl=new (C++) source=" << source << " repeat=" << repeat << " root=" << root << "\n";

	if (!fs::exists(root)) {
		std::cerr << "root does not exist: " << root << "\n";
		return 1;
	}

	struct FileEntry {
		fs::path path;
		std::uintmax_t size;
		std::vector<std::uint8_t> buffer;  // empty when reading from file
	};

	std::vector<FileEntry> files;
	for (auto const& entry : fs::recursive_directory_iterator(root)) {
		if (!entry.is_regular_file()) continue;
		if (entry.path().extension().string() == ".dcm" || entry.path().extension().string() == ".DCM") {
			files.push_back(FileEntry{entry.path(), entry.file_size(), {}});
		}
	}

	if (files.empty()) {
		std::cout << "no .dcm files found under: " << root << "\n";
		return 0;
	}

	if (source == "memory") {
		for (auto& f : files) {
			std::ifstream is(f.path, std::ios::binary);
			f.buffer.resize(static_cast<std::size_t>(f.size));
			is.read(reinterpret_cast<char*>(f.buffer.data()), static_cast<std::streamsize>(f.buffer.size()));
		}
	}

	const std::size_t bytes_per_run = [&] {
		std::size_t sum = 0;
		for (auto const& f : files) sum += static_cast<std::size_t>(f.size);
		return sum;
	}();

	double total_seconds = 0.0;
	for (int r = 0; r < repeat; ++r) {
		const auto t0 = std::chrono::steady_clock::now();
		for (auto const& f : files) {
			std::unique_ptr<dicom::DataSet> dataset;
			if (source == "memory") {
				dicom::ReadOptions opts;
				dataset = dicom::read_bytes(f.path.string(), f.buffer.data(), f.buffer.size(), opts);
			} else {
				dataset = dicom::read_file(f.path.string());
			}
			(void)dataset;
		}
		const auto t1 = std::chrono::steady_clock::now();
		const double seconds = std::chrono::duration<double>(t1 - t0).count();
		total_seconds += seconds;

		const double avg_ms = (seconds / files.size()) * 1000.0;
		const double mbps = (bytes_per_run / (1024.0 * 1024.0)) / (seconds > 0 ? seconds : 1e-9);
		std::cout << "run " << (r + 1) << ": files=" << files.size()
		          << " bytes=" << bytes_per_run
		          << " time=" << std::fixed << std::setprecision(3) << seconds << "s "
		          << "avg=" << std::setprecision(2) << avg_ms << "ms/file "
		          << "throughput=" << std::setprecision(2) << mbps << " MiB/s\n";
	}

	const std::size_t total_files = files.size() * static_cast<std::size_t>(repeat);
	const std::size_t total_bytes = bytes_per_run * static_cast<std::size_t>(repeat);
	const double avg_ms = (total_seconds / total_files) * 1000.0;
	const double mbps = (total_bytes / (1024.0 * 1024.0)) / (total_seconds > 0 ? total_seconds : 1e-9);
	std::cout << "runs=" << repeat
	          << " total_files=" << total_files
	          << " total_bytes=" << total_bytes
	          << " time=" << std::fixed << std::setprecision(3) << total_seconds << "s "
	          << "avg=" << std::setprecision(2) << avg_ms << "ms/file "
	          << "throughput=" << std::setprecision(2) << mbps << " MiB/s\n";

	return 0;
}
