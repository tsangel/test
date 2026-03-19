#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include <dicom.h>

#include "pixel/host/decode/decode_thread_policy.hpp"

namespace {

[[noreturn]] void fail(const std::string& message) {
	std::cerr << message << '\n';
	std::exit(1);
}

template <typename T>
void expect_eq(const T& actual, const T& expected, std::string_view label) {
	if (!(actual == expected)) {
		fail(std::string(label) + " mismatch");
	}
}

} // namespace

int main() {
	using namespace dicom::literals;

	constexpr std::size_t kHardwareThreads = 32;

	{
		dicom::pixel::DecodeOptions options{};
		const auto settings = dicom::pixel::detail::resolve_decode_frame_thread_settings(
		    "JPEG2000Lossless"_uid, options, kHardwareThreads);
		expect_eq(settings.worker_count, std::size_t{1},
		    "single-frame jpeg2000 auto worker_count");
		expect_eq(settings.codec_threads, 32,
		    "single-frame jpeg2000 auto codec_threads");
	}

	{
		dicom::pixel::DecodeOptions options{};
		const auto settings =
		    dicom::pixel::detail::resolve_decode_all_frames_thread_settings(
		        "JPEG2000Lossless"_uid, options, std::size_t{1}, kHardwareThreads);
		expect_eq(settings.worker_count, std::size_t{1},
		    "single-frame batch jpeg2000 auto worker_count");
		expect_eq(settings.codec_threads, 32,
		    "single-frame batch jpeg2000 auto codec_threads");
	}

	{
		dicom::pixel::DecodeOptions options{};
		const auto settings =
		    dicom::pixel::detail::resolve_decode_all_frames_thread_settings(
		        "JPEG2000Lossless"_uid, options, std::size_t{2}, kHardwareThreads);
		expect_eq(settings.worker_count, std::size_t{2},
		    "two-frame jpeg2000 auto worker_count");
		expect_eq(settings.codec_threads, 16,
		    "two-frame jpeg2000 auto codec_threads");
	}

	{
		dicom::pixel::DecodeOptions options{};
		const auto settings =
		    dicom::pixel::detail::resolve_decode_all_frames_thread_settings(
		        "JPEG2000Lossless"_uid, options, std::size_t{8}, kHardwareThreads);
		expect_eq(settings.worker_count, std::size_t{8},
		    "many-frame jpeg2000 auto worker_count");
		expect_eq(settings.codec_threads, 1,
		    "many-frame jpeg2000 auto codec_threads");
	}

	{
		dicom::pixel::DecodeOptions options{};
		options.worker_threads = 2;
		options.codec_threads = 6;
		const auto settings =
		    dicom::pixel::detail::resolve_decode_all_frames_thread_settings(
		        "JPEG2000Lossless"_uid, options, std::size_t{8}, kHardwareThreads);
		expect_eq(settings.worker_count, std::size_t{2},
		    "explicit worker_threads preserved");
		expect_eq(settings.codec_threads, 6,
		    "explicit codec_threads preserved");
	}

	{
		dicom::pixel::DecodeOptions options{};
		const auto settings = dicom::pixel::detail::resolve_decode_frame_thread_settings(
		    "ExplicitVRLittleEndian"_uid, options, kHardwareThreads);
		expect_eq(settings.codec_threads, 1,
		    "native decode auto codec_threads");
	}

	return 0;
}
