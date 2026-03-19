#pragma once

#include "dicom.h"

#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
#include "pixel/host/adapter/host_adapter.hpp"
#include "pixel/runtime/runtime_registry.hpp"
#endif

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <thread>

namespace dicom::pixel::detail {

struct EffectiveExecutionThreadSettings {
	std::size_t worker_count{1};
	int codec_threads{1};
};

[[nodiscard]] inline std::size_t resolve_hardware_thread_count() noexcept {
	const unsigned int hw_threads = std::thread::hardware_concurrency();
	return hw_threads == 0u ? std::size_t{1}
	                        : static_cast<std::size_t>(hw_threads);
}

[[nodiscard]] inline int resolve_auto_codec_thread_cap(
    uid::WellKnown transfer_syntax, std::size_t hardware_threads) noexcept {
	const auto capped_hardware_threads =
	    hardware_threads > static_cast<std::size_t>(std::numeric_limits<int>::max())
	        ? std::numeric_limits<int>::max()
	        : static_cast<int>(hardware_threads);

	if (transfer_syntax.is_jpeg2000()) {
		// Preserve the historical "use available CPU budget" behavior for JPEG2000
		// auto threading while still letting outer worker scheduling bound it.
		return capped_hardware_threads;
	}
	if (transfer_syntax.is_jpegxl()) {
		return 4;
	}
	if (!transfer_syntax.is_htj2k()) {
		return 1;
	}

#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
	const auto* registry = ::pixel::runtime::current_registry();
	if (registry != nullptr) {
		uint32_t codec_profile_code = PIXEL_CODEC_PROFILE_UNKNOWN;
		if (::pixel::runtime::codec_profile_code_from_transfer_syntax(
		        transfer_syntax, &codec_profile_code)) {
			const auto* binding = registry->find_decoder_binding(codec_profile_code);
			if (binding != nullptr && binding->display_name != nullptr) {
				const std::string_view display_name{binding->display_name};
				if (display_name.find("OpenJPEG") != std::string_view::npos) {
					return 8;
				}
				if (display_name.find("HTJ2K") != std::string_view::npos) {
					return 1;
				}
			}
		}
	}
#endif

	return get_htj2k_decoder_backend() == Htj2kDecoderBackend::openjpeg ? 8 : 1;
}

[[nodiscard]] inline int resolve_auto_codec_threads(
    uid::WellKnown transfer_syntax, std::size_t worker_count,
    std::size_t hardware_threads) noexcept {
	if (worker_count >= std::size_t{4}) {
		return 1;
	}

	std::size_t budget = hardware_threads == 0 ? std::size_t{1} : hardware_threads;
	if (worker_count > std::size_t{1}) {
		budget /= worker_count;
	}
	if (budget == std::size_t{0}) {
		budget = std::size_t{1};
	}

	const auto codec_cap = resolve_auto_codec_thread_cap(transfer_syntax, hardware_threads);
	const auto capped_codec_budget =
	    codec_cap <= 0 ? std::size_t{1} : static_cast<std::size_t>(codec_cap);
	if (budget > capped_codec_budget) {
		budget = capped_codec_budget;
	}
	return static_cast<int>(budget);
}

[[nodiscard]] inline EffectiveExecutionThreadSettings resolve_decode_frame_thread_settings(
    uid::WellKnown transfer_syntax, const DecodeOptions& options,
    std::size_t hardware_threads = resolve_hardware_thread_count()) noexcept {
	const int codec_threads =
	    options.codec_threads == -1
	        ? resolve_auto_codec_threads(transfer_syntax, std::size_t{1}, hardware_threads)
	        : options.codec_threads;
	return EffectiveExecutionThreadSettings{
	    .worker_count = std::size_t{1},
	    .codec_threads = codec_threads,
	};
}

[[nodiscard]] inline EffectiveExecutionThreadSettings
resolve_decode_all_frames_thread_settings(
    uid::WellKnown transfer_syntax, const DecodeOptions& options, std::size_t frames,
    std::size_t hardware_threads = resolve_hardware_thread_count()) noexcept {
	std::size_t worker_count = std::size_t{1};
	if (frames > std::size_t{1}) {
		if (options.worker_threads == -1) {
			worker_count = (frames < hardware_threads) ? frames : hardware_threads;
		} else if (options.worker_threads > 1) {
			worker_count = static_cast<std::size_t>(options.worker_threads);
			if (worker_count > frames) {
				worker_count = frames;
			}
		}
	}

	const int codec_threads =
	    options.codec_threads == -1
	        ? resolve_auto_codec_threads(transfer_syntax, worker_count, hardware_threads)
	        : options.codec_threads;

	return EffectiveExecutionThreadSettings{
	    .worker_count = worker_count,
	    .codec_threads = codec_threads,
	};
}

} // namespace dicom::pixel::detail
