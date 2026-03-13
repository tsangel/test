#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include <dicom.h>

#include "codec_builtin_flags.hpp"
#include "pixel/runtime/plugin_registry_v2.hpp"

namespace {

void expect_auto_backend_binding() {
	pixel::runtime_v2::BindingRegistryV2 registry{};
	pixel::runtime_v2::init_builtin_registry_v2(&registry);
	const auto* binding =
	    registry.find_decoder_binding(PIXEL_CODEC_PROFILE_HTJ2K_LOSSLESS_V2);

	if (dicom::test::kHtj2kBuiltin && dicom::test::kJpeg2kBuiltin &&
	    dicom::test::kHasOpenJphBackend) {
		if (binding == nullptr || binding->display_name == nullptr ||
		    std::string_view(binding->display_name).find("HTJ2K") == std::string_view::npos) {
			std::cerr << "auto HTJ2K backend should prefer OpenJPH when both backends are built\n";
			std::exit(1);
		}
		return;
	}

	if (dicom::test::kHtj2kBuiltin) {
		if (binding == nullptr || binding->display_name == nullptr ||
		    std::string_view(binding->display_name).find("HTJ2K") == std::string_view::npos) {
			std::cerr << "auto HTJ2K backend should resolve to builtin HTJ2K decoder\n";
			std::exit(1);
		}
		return;
	}

	if (dicom::test::kJpeg2kBuiltin) {
		if (binding == nullptr || binding->display_name == nullptr ||
		    std::string_view(binding->display_name).find("OpenJPEG") == std::string_view::npos) {
			std::cerr << "auto HTJ2K backend should fall back to OpenJPEG decoder\n";
			std::exit(1);
		}
		return;
	}

	if (binding != nullptr) {
		std::cerr << "auto HTJ2K backend should be absent when no backend is built\n";
		std::exit(1);
	}
}

}  // namespace

int main() {
	auto fail = [](const std::string& message) {
		std::cerr << message << '\n';
		std::exit(1);
	};

	expect_auto_backend_binding();

	std::string error{};
	if (dicom::test::kHasOpenJphBackend && dicom::test::kHtj2kBuiltin) {
		if (!dicom::pixel::use_openjph_for_htj2k_decoding(&error)) {
			fail("use_openjph_for_htj2k_decoding should succeed before runtime initialization: " +
			    error);
		}
		if (dicom::pixel::get_htj2k_decoder_backend() !=
		    dicom::pixel::Htj2kDecoderBackend::openjph) {
			fail("get_htj2k_decoder_backend should report openjph after early configuration");
		}
	} else {
		if (dicom::pixel::use_openjph_for_htj2k_decoding(&error)) {
			fail("use_openjph_for_htj2k_decoding should fail when OpenJPH HTJ2K backend is unavailable");
		}
		if (error.find("not available") == std::string::npos) {
			fail("missing OpenJPH HTJ2K backend should report availability error");
		}
	}

	error.clear();
	if (dicom::test::kJpeg2kBuiltin) {
		if (!dicom::pixel::use_openjpeg_for_htj2k_decoding(&error)) {
			fail("use_openjpeg_for_htj2k_decoding should succeed before runtime initialization: " +
			    error);
		}
		if (dicom::pixel::get_htj2k_decoder_backend() !=
		    dicom::pixel::Htj2kDecoderBackend::openjpeg) {
			fail("get_htj2k_decoder_backend should report openjpeg after early configuration");
		}
	} else {
		if (dicom::pixel::use_openjpeg_for_htj2k_decoding(&error)) {
			fail("use_openjpeg_for_htj2k_decoding should fail when OpenJPEG HTJ2K backend is unavailable");
		}
		if (error.find("not available") == std::string::npos) {
			fail("missing OpenJPEG HTJ2K backend should report availability error");
		}
	}

	error.clear();
	if (dicom::pixel::register_external_codec_plugin_from_library(
	        "dicomsdl_missing_codec_plugin_for_test", &error)) {
		fail("register_external_codec_plugin_from_library should fail for a missing test plugin");
	}
	if (error.empty()) {
		fail("failed plugin registration should provide an error message");
	}

	std::string late_error{};
	if (dicom::pixel::set_htj2k_decoder_backend(
	        dicom::pixel::Htj2kDecoderBackend::auto_select, &late_error)) {
		fail("set_htj2k_decoder_backend should fail after runtime initialization");
	}
	if (late_error.find("before first pixel decode/encode or external plugin registration") ==
	    std::string::npos) {
		fail("late HTJ2K backend configuration should explain initialization ordering");
	}

	return 0;
}
