#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <dicom.h>

namespace {
using namespace dicom::literals;

struct RunSummary {
	std::size_t file_count{0};
	std::size_t file_fail_count{0};
	std::size_t codec_pass_count{0};
	std::size_t codec_fail_count{0};
	std::size_t codec_skip_count{0};
};

struct FrameDigest {
	std::size_t size{0};
	std::uint64_t hash{0};
};

[[noreturn]] void fail(const std::string& message) {
	std::cerr << message << std::endl;
	std::exit(1);
}

[[nodiscard]] std::uint64_t fnv1a64(std::span<const std::uint8_t> bytes) {
	constexpr std::uint64_t kOffsetBasis = 1469598103934665603ULL;
	constexpr std::uint64_t kPrime = 1099511628211ULL;
	std::uint64_t hash = kOffsetBasis;
	for (std::uint8_t value : bytes) {
		hash ^= static_cast<std::uint64_t>(value);
		hash *= kPrime;
	}
	return hash;
}

[[nodiscard]] std::vector<FrameDigest> build_frame_digests(const dicom::DicomFile& file) {
	const auto& info = file.pixeldata_info(true);
	if (!info.has_pixel_data || info.frames <= 0) {
		throw std::runtime_error("No decodable pixel data");
	}

	const std::size_t frame_count = static_cast<std::size_t>(info.frames);
	std::vector<FrameDigest> digests;
	digests.reserve(frame_count);
	for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
		const auto decoded = file.pixel_data(frame_index);
		digests.push_back(FrameDigest{
		    .size = decoded.size(),
		    .hash = fnv1a64(std::span<const std::uint8_t>(decoded)),
		});
	}
	return digests;
}

void assert_file_matches_digests(const dicom::DicomFile& file,
    const std::vector<FrameDigest>& expected, std::string_view context) {
	for (std::size_t frame_index = 0; frame_index < expected.size(); ++frame_index) {
		const auto decoded = file.pixel_data(frame_index);
		const FrameDigest actual{
		    .size = decoded.size(),
		    .hash = fnv1a64(std::span<const std::uint8_t>(decoded)),
		};
		const auto& exp = expected[frame_index];
		if (actual.size != exp.size || actual.hash != exp.hash) {
			throw std::runtime_error(std::string(context) + " frame#" +
			    std::to_string(frame_index) + " mismatch (size/hash changed)");
		}
	}
}

void run_three_cycles_for_codec(const std::string& input_path, dicom::uid::WellKnown codec_ts,
    std::string_view codec_name) {
	auto file = dicom::read_file(input_path);
	if (!file) {
		throw std::runtime_error("Failed to read file: " + input_path);
	}

	// Normalize once so each cycle starts from native source bytes.
	file->set_transfer_syntax("ExplicitVRLittleEndian"_uid);
	const auto baseline = build_frame_digests(*file);

	for (int cycle = 1; cycle <= 3; ++cycle) {
		file->set_transfer_syntax(codec_ts);
		assert_file_matches_digests(*file, baseline,
		    input_path + " " + std::string(codec_name) + " cycle#" + std::to_string(cycle) +
		    " encoded-decode");

		file->set_transfer_syntax("ExplicitVRLittleEndian"_uid);
		assert_file_matches_digests(*file, baseline,
		    input_path + " " + std::string(codec_name) + " cycle#" + std::to_string(cycle) +
		    " back-to-native");
	}
}

[[nodiscard]] bool is_known_unsupported_case(std::string_view message) {
	return message.find("unsupported PhotometricInterpretation") != std::string_view::npos ||
	    message.find("transfer syntax is not supported yet in set_pixel_data") != std::string_view::npos;
}

void run_for_file(const std::string& input_path, RunSummary& summary) {
	++summary.file_count;
	std::cout << "[RUN] " << input_path << '\n';
	bool file_failed = false;
	const std::array<std::pair<dicom::uid::WellKnown, const char*>, 4> codecs{{
	    {"EncapsulatedUncompressedExplicitVRLittleEndian"_uid,
	        "EncapsulatedUncompressedExplicitVRLittleEndian"},
	    {"HTJ2KLossless"_uid, "HTJ2KLossless"},
	    {"JPEGLSLossless"_uid, "JPEGLSLossless"},
	    {"JPEGLosslessSV1"_uid, "JPEGLosslessSV1"},
	}};

	for (const auto& [codec_ts, codec_name] : codecs) {
		try {
			run_three_cycles_for_codec(input_path, codec_ts, codec_name);
			std::cout << "  - " << codec_name << ": PASS\n";
			++summary.codec_pass_count;
		} catch (const std::exception& e) {
			const std::string message = e.what();
			if (is_known_unsupported_case(message)) {
				std::cout << "  - " << codec_name << ": SKIP (" << message << ")\n";
				++summary.codec_skip_count;
				continue;
			}
			std::cout << "  - " << codec_name << ": FAIL (" << message << ")\n";
			++summary.codec_fail_count;
			file_failed = true;
		}
	}

	if (file_failed) {
		++summary.file_fail_count;
	}
}

} // namespace

int main(int argc, char** argv) {
	if (argc < 2) {
		std::cerr << "Usage: " << argv[0] << " <dicom-file> [<dicom-file>...]\n";
		return 2;
	}

	try {
		RunSummary summary{};
		for (int i = 1; i < argc; ++i) {
			run_for_file(argv[i], summary);
		}

		std::cout << "SUMMARY files=" << summary.file_count
		          << " file_fail=" << summary.file_fail_count
		          << " codec_pass=" << summary.codec_pass_count
		          << " codec_skip=" << summary.codec_skip_count
		          << " codec_fail=" << summary.codec_fail_count << '\n';
		if (summary.codec_fail_count != 0) {
			return 1;
		}
	} catch (const std::exception& e) {
		fail(std::string("Exception: ") + e.what());
	}

	std::cout << "ALL PASS\n";
	return 0;
}
