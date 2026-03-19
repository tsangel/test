#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <dicom.h>

namespace {
using namespace dicom::literals;

[[noreturn]] void fail(const std::string& message) {
	std::cerr << message << std::endl;
	std::exit(1);
}

void expect_contains(std::string_view haystack, std::string_view needle,
    std::string_view label) {
	if (haystack.find(needle) == std::string_view::npos) {
		fail(std::string(label) + " missing token: " + std::string(needle));
	}
}

struct ProgressState {
	std::atomic<std::size_t> calls{0};
	std::atomic<std::size_t> last_completed{0};
	std::atomic<std::size_t> last_total{0};
	std::size_t cancel_after{std::size_t(-1)};
};

void on_progress(
    std::size_t completed, std::size_t total, void* user_data) noexcept {
	auto* state = static_cast<ProgressState*>(user_data);
	state->calls.fetch_add(1, std::memory_order_relaxed);
	state->last_completed.store(completed, std::memory_order_release);
	state->last_total.store(total, std::memory_order_release);
}

bool should_cancel(void* user_data) noexcept {
	auto* state = static_cast<ProgressState*>(user_data);
	return state->last_completed.load(std::memory_order_acquire) >=
	    state->cancel_after;
}

void configure_native_multiframe_file(dicom::DicomFile& df,
    const std::vector<std::uint16_t>& pixels, int frames, int rows, int cols) {
	// Describe the source volume with the normalized layout/span pair used by
	// the production encode path.
	const dicom::pixel::ConstPixelSpan source{
	    .layout = dicom::pixel::PixelLayout{
	        .data_type = dicom::pixel::DataType::u16,
	        .photometric = dicom::pixel::Photometric::monochrome2,
	        .planar = dicom::pixel::Planar::interleaved,
	        .reserved = 0,
	        .rows = static_cast<std::uint32_t>(rows),
	        .cols = static_cast<std::uint32_t>(cols),
	        .frames = static_cast<std::uint32_t>(frames),
	        .samples_per_pixel = 1,
	        .bits_stored = 16,
	        .row_stride = static_cast<std::size_t>(cols) * sizeof(std::uint16_t),
	        .frame_stride = static_cast<std::size_t>(rows * cols) * sizeof(std::uint16_t),
	    },
	    .bytes = std::span<const std::uint8_t>(
	        reinterpret_cast<const std::uint8_t*>(pixels.data()),
	        pixels.size() * sizeof(std::uint16_t)),
	};
	df.set_pixel_data("ExplicitVRLittleEndian"_uid, source);
}

} // namespace

int main() {
	using namespace dicom::literals;

	constexpr int kFrames = 4;
	constexpr int kRows = 2;
	constexpr int kCols = 3;
	const std::vector<std::uint16_t> pixels{
	    1, 2, 3, 4, 5, 6,
	    11, 12, 13, 14, 15, 16,
	    21, 22, 23, 24, 25, 26,
	    31, 32, 33, 34, 35, 36,
	};
	const auto expected_bytes = std::span<const std::uint8_t>(
	    reinterpret_cast<const std::uint8_t*>(pixels.data()),
	    pixels.size() * sizeof(std::uint16_t));

	{
		dicom::DicomFile df{};
		configure_native_multiframe_file(df, pixels, kFrames, kRows, kCols);
		dicom::pixel::DecodeOptions options{};
		options.worker_threads = 2;
		const auto plan = df.create_decode_plan(options);
		std::vector<std::uint8_t> out(plan.output_layout.frame_stride * kFrames, 0);
		ProgressState progress{};
		const dicom::pixel::ExecutionObserver observer{
		    .on_progress = &on_progress,
		    .should_cancel = nullptr,
		    .user_data = &progress,
		    .notify_every = 2,
		};

		df.decode_all_frames_into(std::span<std::uint8_t>(out.data(), out.size()), plan,
		    &observer);

		if (progress.calls.load(std::memory_order_acquire) != 2) {
			fail("progress callback should fire twice for 4 frames with notify_every=2");
		}
		if (progress.last_completed.load(std::memory_order_acquire) != kFrames) {
			fail("progress callback should report final completed frame count");
		}
		if (progress.last_total.load(std::memory_order_acquire) != kFrames) {
			fail("progress callback should report total frame count");
		}
		if (!std::equal(out.begin(), out.end(), expected_bytes.begin(), expected_bytes.end())) {
			fail("decode_all_frames_into with observer should preserve pixel payload");
		}
	}

	{
		dicom::DicomFile df{};
		configure_native_multiframe_file(df, pixels, kFrames, kRows, kCols);
		dicom::pixel::DecodeOptions options{};
		options.worker_threads = 1;
		const auto plan = df.create_decode_plan(options);
		std::vector<std::uint8_t> out(plan.output_layout.frame_stride * kFrames, 0);
		ProgressState progress{};
		progress.cancel_after = 2;
		const dicom::pixel::ExecutionObserver observer{
		    .on_progress = &on_progress,
		    .should_cancel = &should_cancel,
		    .user_data = &progress,
		    .notify_every = 1,
		};

		try {
			df.decode_all_frames_into(
			    std::span<std::uint8_t>(out.data(), out.size()), plan, &observer);
			fail("decode_all_frames_into cancellation should throw");
		} catch (const std::exception& e) {
			const std::string what = e.what();
			expect_contains(
			    what, "pixel::decode_all_frames_into", "observer cancel throw");
			expect_contains(what, "status=cancelled", "observer cancel throw");
			expect_contains(what, "stage=cancel", "observer cancel throw");
		}

		if (progress.last_completed.load(std::memory_order_acquire) != 2) {
			fail("cancellation should stop after the requested completed frame count");
		}
	}

	return 0;
}
