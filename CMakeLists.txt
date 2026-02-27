  cmake_minimum_required(VERSION 3.16)

if(APPLE)
    if(DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
        if(CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS "10.13")
            set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS deployment target" FORCE)
        endif()
    else()
        set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS deployment target")
    endif()
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" _dicomsdl_version_raw)
string(STRIP "${_dicomsdl_version_raw}" DICOMPROJECT_VERSION)

set(DICOMSDL_VERSION ${DICOMPROJECT_VERSION})

set(_dicom_version_file "${CMAKE_CURRENT_SOURCE_DIR}/misc/dictionary/_dicom_version.txt")
if(EXISTS "${_dicom_version_file}")
    file(READ "${_dicom_version_file}" _dicom_standard_version_raw)
    string(STRIP "${_dicom_standard_version_raw}" DICOM_STANDARD_VERSION)
else()
    set(DICOM_STANDARD_VERSION "unknown")
endif()

set(DICOM_VERSION_SOURCE "${_dicom_version_file}")
set(DICOMSDL_VERSION_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")

project(DicomProject VERSION ${DICOMPROJECT_VERSION} LANGUAGES C CXX)

set(GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/version.h.in"
    "${GENERATED_INCLUDE_DIR}/version.h"
    @ONLY
)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(_dicom_release_flags /Ox /Ob2 /Oi /Ot /GL /DNDEBUG)
    foreach(flag IN LISTS _dicom_release_flags)
        add_compile_options($<$<CONFIG:Release>:${flag}>)
        add_compile_options($<$<CONFIG:RelWithDebInfo>:${flag}>)
    endforeach()
    add_link_options(
        $<$<CONFIG:Release>:/LTCG>
        $<$<CONFIG:RelWithDebInfo>:/LTCG>
    )
elseif(MSVC)
    set(_dicom_release_flags /Ox /Ob2 /Oi /Ot /DNDEBUG)
    foreach(flag IN LISTS _dicom_release_flags)
        add_compile_options($<$<CONFIG:Release>:${flag}>)
        add_compile_options($<$<CONFIG:RelWithDebInfo>:${flag}>)
    endforeach()
else()
    set(_dicom_release_flags -O3 -fstrict-aliasing -funroll-loops)
    foreach(flag IN LISTS _dicom_release_flags)
        add_compile_options($<$<CONFIG:Release>:${flag}>)
        add_compile_options($<$<CONFIG:RelWithDebInfo>:${flag}>)
    endforeach()
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(-fconstexpr-steps=16777216)
    endif()
    add_link_options(
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:RelWithDebInfo>:-O3>
    )
endif()

set(DICOM_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

include(CTest)
include(ExternalProject)
include(FetchContent)

set(DICOMSDL_FMT_GIT_REPOSITORY "https://github.com/fmtlib/fmt.git"
	CACHE STRING "fmt upstream git repository used by FetchContent.")
# Release: fmt 12.1.0 (GitHub latest release, checked 2026-02-27)
set(DICOMSDL_FMT_GIT_TAG "12.1.0"
	CACHE STRING "fmt git ref used by FetchContent (use branch/tag/SHA).")

set(DICOMSDL_OPENJPEG_GIT_REPOSITORY "https://github.com/uclouvain/openjpeg.git"
	CACHE STRING "openjpeg upstream git repository used by FetchContent.")
# Release: openjpeg v2.5.4 (GitHub latest release, checked 2026-02-27)
set(DICOMSDL_OPENJPEG_GIT_TAG "v2.5.4"
	CACHE STRING "openjpeg git ref used by FetchContent (use branch/tag/SHA).")

set(DICOMSDL_LIBJPEG_TURBO_GIT_REPOSITORY "https://github.com/libjpeg-turbo/libjpeg-turbo.git"
	CACHE STRING "libjpeg-turbo upstream git repository used by FetchContent.")
# Release: libjpeg-turbo 3.1.3 (GitHub latest release, checked 2026-02-27)
set(DICOMSDL_LIBJPEG_TURBO_GIT_TAG "3.1.3"
	CACHE STRING "libjpeg-turbo git ref used by FetchContent (use branch/tag/SHA).")

set(DICOMSDL_LIBDEFLATE_GIT_REPOSITORY "https://github.com/ebiggers/libdeflate.git"
	CACHE STRING "libdeflate upstream git repository used by FetchContent.")
# Release: libdeflate v1.25 (GitHub latest release, checked 2026-02-27)
set(DICOMSDL_LIBDEFLATE_GIT_TAG "v1.25"
	CACHE STRING "libdeflate git ref used by FetchContent (use branch/tag/SHA).")

set(DICOMSDL_CHARLS_GIT_REPOSITORY "https://github.com/team-charls/charls.git"
	CACHE STRING "CharLS upstream git repository used by FetchContent.")
# Release: CharLS 2.4.2 (GitHub latest release, checked 2026-02-27)
set(DICOMSDL_CHARLS_GIT_TAG "2.4.2"
	CACHE STRING "CharLS git ref used by FetchContent (use branch/tag/SHA).")

set(DICOMSDL_OPENJPH_GIT_REPOSITORY "https://github.com/aous72/OpenJPH.git"
	CACHE STRING "OpenJPH upstream git repository used by FetchContent.")
# Release: OpenJPH 0.26.3 (GitHub latest release, checked 2026-02-27)
set(DICOMSDL_OPENJPH_GIT_TAG "0.26.3"
	CACHE STRING "OpenJPH git ref used by FetchContent (use branch/tag/SHA).")

set(DICOMSDL_LIBJXL_GIT_REPOSITORY "https://github.com/libjxl/libjxl.git"
	CACHE STRING "libjxl upstream git repository used by FetchContent.")
# Release: libjxl v0.11.2 (GitHub latest release, checked 2026-02-27)
set(DICOMSDL_LIBJXL_GIT_TAG "v0.11.2"
	CACHE STRING "libjxl git ref used by FetchContent (use branch/tag/SHA).")

set(DICOMSDL_NANOBIND_GIT_REPOSITORY "https://github.com/wjakob/nanobind.git"
	CACHE STRING "nanobind upstream git repository used by FetchContent.")
# Release note: nanobind has no GitHub Releases page; using latest version tag v2.12.0 (checked 2026-02-27)
set(DICOMSDL_NANOBIND_GIT_TAG "v2.12.0"
	CACHE STRING "nanobind git ref used by FetchContent (use branch/tag/SHA).")

set(_dicomsdl_any_codec_shared_from_cache OFF)
foreach(_dicomsdl_codec IN ITEMS JPEG JPEGLS JPEG2K HTJ2K JPEGXL)
	if(DEFINED DICOMSDL_CODEC_${_dicomsdl_codec}_SHARED AND DICOMSDL_CODEC_${_dicomsdl_codec}_SHARED)
		set(_dicomsdl_any_codec_shared_from_cache ON)
		break()
	endif()
endforeach()
option(DICOMSDL_BUILD_SHARED_CORE
	"Build dicomsdl core as a shared library. This should be ON when any codec plugin mode is shared."
	${_dicomsdl_any_codec_shared_from_cache})
if(_dicomsdl_any_codec_shared_from_cache AND NOT DICOMSDL_BUILD_SHARED_CORE)
	message(STATUS "Enabling DICOMSDL_BUILD_SHARED_CORE because shared codec plugin mode is active.")
	set(DICOMSDL_BUILD_SHARED_CORE ON CACHE BOOL
		"Build dicomsdl core as a shared library. This should be ON when any codec plugin mode is shared."
		FORCE)
endif()
unset(_dicomsdl_any_codec_shared_from_cache)
unset(_dicomsdl_codec)

if(DICOMSDL_BUILD_SHARED_CORE)
	set(_dicomsdl_core_library_type SHARED)
else()
	set(_dicomsdl_core_library_type STATIC)
endif()
message(STATUS "DICOMSDL core linkage: ${_dicomsdl_core_library_type}")

add_library(dicomsdl ${_dicomsdl_core_library_type}
	src/dataset.cpp
	src/diagnostics.cpp
	src/dataset_deflate_codec.cpp
	src/dataset_endian_converter.cpp
	src/dumpdataset.cpp
	src/dicomfile.cpp
	src/dicom_write.cpp
	src/uid_utils.cpp
	src/sequence.cpp
	src/dataelement.cpp
	src/instream.cpp
	src/pixelsequence.cpp
	src/pixel/registry/codec_registry.cpp
	src/pixel/bridge/codec_dispatch_bridge.cpp
	src/pixel/bridge/codec_option_bridge.cpp
	src/pixel/plugin_abi/external/plugin_external_bridge.cpp
	src/pixel/plugin_abi/builtin/jpeg2k_builtin_plugin.cpp
	src/pixel/plugin_abi/builtin/htj2k_builtin_plugin.cpp
	src/pixel/plugin_abi/builtin/jpegls_builtin_plugin.cpp
	src/pixel/plugin_abi/builtin/jpegxl_builtin_plugin.cpp
	src/pixel/plugin_abi/external/plugin_loader.cpp
	src/pixel/plugin_abi/builtin/jpeg_builtin_plugin.cpp
	src/pixel/plugin_abi/builtin/rle_builtin_plugin.cpp
	src/pixel/entrypoint/decode_entrypoint.cpp
	src/pixel/decode/core/decode_entrypoint_runner.cpp
	src/pixel/decode/core/decode_modality_transform.cpp
	src/pixel/decode/core/decode_frame_dispatch.cpp
	src/pixel/decode/core/decode_frame_source_resolver.cpp
	src/pixel/decode/core/decode_plugin_dispatch.cpp
	src/pixel/decode/core/decode_frame_source_loader.cpp
	src/pixel/decode/core/decode_planar_copy.cpp
	src/pixel/decode/codecs/raw_decode.cpp
	src/pixel/decode/codecs/encapsulated_uncompressed_decode.cpp
	src/pixel/entrypoint/encode_entrypoint.cpp
	src/pixel/encode/core/encode_context_resolver.cpp
	src/pixel/encode/core/encode_set_pixel_data_resolver.cpp
	src/pixel/encode/core/encode_binding_resolver.cpp
	src/pixel/encode/core/encode_set_pixel_data_runner.cpp
	src/pixel/encode/core/encode_source_layout_resolver.cpp
	src/pixel/encode/core/encode_target_resolver.cpp
	src/pixel/encode/core/native_pixel_copy.cpp
	src/pixel/encode/core/multicomponent_option_resolver.cpp
	src/pixel/encode/core/encode_metadata_updater.cpp
	src/pixel/encode/codecs/encapsulated_encode.cpp
	src/pixel/encode/codecs/jpeg2k_encode.cpp
	src/pixel/encode/codecs/htj2k_encode.cpp
	src/pixel/encode/codecs/jpegls_encode.cpp
	src/pixel/encode/codecs/jpeg_encode.cpp
	src/pixel/encode/codecs/jpegxl_encode.cpp
	src/pixel/decode/codecs/jpeg2k_decode.cpp
	src/pixel/decode/codecs/htj2k_decode.cpp
	src/pixel/decode/codecs/jpegls_decode.cpp
	src/pixel/decode/codecs/jpegxl_decode.cpp
	src/pixel/decode/codecs/jpeg_decode.cpp
	)
set_target_properties(dicomsdl PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(dicomsdl PROPERTIES
	ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
	LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
if(WIN32 AND DICOMSDL_BUILD_SHARED_CORE)
	# dicom.h does not annotate all public C++ symbols with __declspec(dllexport),
	# so enable broad export for Windows shared-core builds.
	set_target_properties(dicomsdl PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()
target_include_directories(dicomsdl
	PUBLIC
		${DICOM_INCLUDE_DIR}
		${GENERATED_INCLUDE_DIR}
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/src
)
FetchContent_Declare(dicomsdl_fmt
	GIT_REPOSITORY "${DICOMSDL_FMT_GIT_REPOSITORY}"
	GIT_TAG "${DICOMSDL_FMT_GIT_TAG}"
	GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(dicomsdl_fmt)
target_link_libraries(dicomsdl PUBLIC fmt::fmt-header-only)

option(DICOMSDL_ENABLE_OPENJPH "Enable OpenJPH backend for HTJ2K decoding" ON)

option(DICOMSDL_ENABLE_JPEGXL "Enable JPEG XL backend dependencies (libjxl) for codec development" OFF)

option(DICOMSDL_CODEC_JPEG_BUILTIN
	"Enable builtin JPEG codec dispatch in CodecRegistry." ON)
option(DICOMSDL_CODEC_JPEG_SHARED
	"Build external shared JPEG codec plugin bridge target." OFF)
option(DICOMSDL_CODEC_JPEGLS_BUILTIN
	"Enable builtin JPEG-LS codec dispatch in CodecRegistry." ON)
option(DICOMSDL_CODEC_JPEGLS_SHARED
	"Build external shared JPEG-LS codec plugin bridge target." OFF)
option(DICOMSDL_CODEC_JPEG2K_BUILTIN
	"Enable builtin JPEG2000 codec dispatch in CodecRegistry." ON)
option(DICOMSDL_CODEC_JPEG2K_SHARED
	"Build external shared JPEG2000 codec plugin bridge target." OFF)
option(DICOMSDL_CODEC_HTJ2K_BUILTIN
	"Enable builtin HTJ2K codec dispatch in CodecRegistry." ON)
option(DICOMSDL_CODEC_HTJ2K_SHARED
	"Build external shared HTJ2K codec plugin bridge target." OFF)
option(DICOMSDL_CODEC_JPEGXL_BUILTIN
	"Enable builtin JPEG XL codec dispatch in CodecRegistry." ON)
option(DICOMSDL_CODEC_JPEGXL_SHARED
	"Build external shared JPEG XL codec plugin bridge target." OFF)

macro(dicomsdl_resolve_codec_mode CODEC_UPPER CODEC_LABEL)
	if(DICOMSDL_CODEC_${CODEC_UPPER}_BUILTIN AND DICOMSDL_CODEC_${CODEC_UPPER}_SHARED)
		message(FATAL_ERROR
			"DICOMSDL_CODEC_${CODEC_UPPER}_BUILTIN and "
			"DICOMSDL_CODEC_${CODEC_UPPER}_SHARED cannot both be ON.")
	endif()
	if(DICOMSDL_CODEC_${CODEC_UPPER}_BUILTIN)
		set(_dicomsdl_codec_mode "builtin")
	elseif(DICOMSDL_CODEC_${CODEC_UPPER}_SHARED)
		set(_dicomsdl_codec_mode "shared")
	else()
		set(_dicomsdl_codec_mode "none")
	endif()
	set(DICOMSDL_CODEC_${CODEC_UPPER}_MODE "${_dicomsdl_codec_mode}" CACHE INTERNAL
		"Resolved ${CODEC_LABEL} codec mode" FORCE)
	message(STATUS "DICOMSDL codec ${CODEC_LABEL} mode: ${_dicomsdl_codec_mode}")
	unset(_dicomsdl_codec_mode)
endmacro()

dicomsdl_resolve_codec_mode(JPEG "jpeg")
dicomsdl_resolve_codec_mode(JPEGLS "jpegls")
dicomsdl_resolve_codec_mode(JPEG2K "jpeg2k")
dicomsdl_resolve_codec_mode(HTJ2K "htj2k")
dicomsdl_resolve_codec_mode(JPEGXL "jpegxl")

if(DEFINED BUILD_SHARED_LIBS)
	set(_dicomsdl_build_shared_libs_defined TRUE)
	set(_dicomsdl_build_shared_libs "${BUILD_SHARED_LIBS}")
else()
	set(_dicomsdl_build_shared_libs_defined FALSE)
endif()

if(WIN32)
	# Avoid runtime DLL dependency for wheel tests/import on Windows.
	# OpenJPEG builds a shared openjp2 by default, which is not bundled by
	# cibuildwheel automatically, causing _dicomsdl import failures.
	set(BUILD_SHARED_LIBS OFF)
endif()

set(_dicomsdl_build_testing "${BUILD_TESTING}")
# Keep OpenJPEG tests disabled in this superbuild. Otherwise CTest registers
# hundreds of OpenJPEG cases while EXCLUDE_FROM_ALL prevents their binaries
# from being built, which causes mass "Not Run" failures.
set(BUILD_TESTING OFF)
FetchContent_Declare(dicomsdl_openjpeg
	GIT_REPOSITORY "${DICOMSDL_OPENJPEG_GIT_REPOSITORY}"
	GIT_TAG "${DICOMSDL_OPENJPEG_GIT_TAG}"
	GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(dicomsdl_openjpeg)
set(BUILD_TESTING "${_dicomsdl_build_testing}")
unset(_dicomsdl_build_testing)

if(_dicomsdl_build_shared_libs_defined)
	set(BUILD_SHARED_LIBS "${_dicomsdl_build_shared_libs}")
else()
	unset(BUILD_SHARED_LIBS)
endif()
unset(_dicomsdl_build_shared_libs)
unset(_dicomsdl_build_shared_libs_defined)

if(DEFINED BUILD_SHARED_LIBS)
	set(_dicomsdl_charls_build_shared_libs_defined TRUE)
	set(_dicomsdl_charls_build_shared_libs "${BUILD_SHARED_LIBS}")
else()
	set(_dicomsdl_charls_build_shared_libs_defined FALSE)
endif()

set(BUILD_SHARED_LIBS OFF)
FetchContent_Declare(dicomsdl_charls
	GIT_REPOSITORY "${DICOMSDL_CHARLS_GIT_REPOSITORY}"
	GIT_TAG "${DICOMSDL_CHARLS_GIT_TAG}"
	GIT_SHALLOW TRUE
	GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(dicomsdl_charls)

if(_dicomsdl_charls_build_shared_libs_defined)
	set(BUILD_SHARED_LIBS "${_dicomsdl_charls_build_shared_libs}")
else()
	unset(BUILD_SHARED_LIBS)
endif()
unset(_dicomsdl_charls_build_shared_libs)
unset(_dicomsdl_charls_build_shared_libs_defined)

set(LIBDEFLATE_BUILD_STATIC_LIB ON CACHE BOOL "Build libdeflate static library" FORCE)
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "Disable libdeflate shared library" FORCE)
set(LIBDEFLATE_BUILD_GZIP OFF CACHE BOOL "Disable libdeflate-gzip executable" FORCE)
set(LIBDEFLATE_BUILD_TESTS OFF CACHE BOOL "Disable libdeflate tests" FORCE)
set(LIBDEFLATE_INSTALL OFF CACHE BOOL "Disable libdeflate install targets" FORCE)
set(LIBDEFLATE_COMPRESSION_SUPPORT ON CACHE BOOL "Enable libdeflate compression codepaths" FORCE)
set(LIBDEFLATE_DECOMPRESSION_SUPPORT ON CACHE BOOL "Enable libdeflate decompression codepaths" FORCE)
FetchContent_Declare(dicomsdl_libdeflate
	GIT_REPOSITORY "${DICOMSDL_LIBDEFLATE_GIT_REPOSITORY}"
	GIT_TAG "${DICOMSDL_LIBDEFLATE_GIT_TAG}"
	GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(dicomsdl_libdeflate)

if(DICOMSDL_ENABLE_OPENJPH)
	if(DEFINED BUILD_SHARED_LIBS)
		set(_dicomsdl_openjph_build_shared_libs_defined TRUE)
		set(_dicomsdl_openjph_build_shared_libs "${BUILD_SHARED_LIBS}")
	else()
		set(_dicomsdl_openjph_build_shared_libs_defined FALSE)
	endif()

	if(DEFINED CACHE{BUILD_SHARED_LIBS})
		set(_dicomsdl_openjph_build_shared_libs_cache_defined TRUE)
		get_property(
			_dicomsdl_openjph_build_shared_libs_cache
			CACHE BUILD_SHARED_LIBS
			PROPERTY VALUE
		)
	else()
		set(_dicomsdl_openjph_build_shared_libs_cache_defined FALSE)
	endif()

	set(BUILD_SHARED_LIBS OFF)
	set(BUILD_SHARED_LIBS OFF CACHE BOOL "Force static OpenJPH build in dicomsdl superbuild" FORCE)
		set(OJPH_ENABLE_TIFF_SUPPORT OFF CACHE BOOL "Disable OpenJPH TIFF support" FORCE)
		set(OJPH_BUILD_TESTS OFF CACHE BOOL "Disable OpenJPH tests" FORCE)
		set(OJPH_BUILD_EXECUTABLES OFF CACHE BOOL "Disable OpenJPH executables" FORCE)
		set(OJPH_BUILD_STREAM_EXPAND OFF CACHE BOOL "Disable OpenJPH stream expand executable" FORCE)
		set(OJPH_BUILD_FUZZER OFF CACHE BOOL "Disable OpenJPH fuzzer target" FORCE)
		if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
			# TODO: Temporary workaround for clang-cl builds.
			# OpenJPH's SSSE3 translation unit currently fails because the required SSSE3 target
			# feature flags are not applied in this toolchain path. Re-enable when OpenJPH
			# provides proper clang-cl SIMD flag handling (or when we carry a local upstream fix).
			set(OJPH_DISABLE_SSSE3 ON CACHE BOOL "Disable OpenJPH SSSE3 path for clang-cl compatibility" FORCE)
		endif()
		FetchContent_Declare(dicomsdl_openjph
			GIT_REPOSITORY "${DICOMSDL_OPENJPH_GIT_REPOSITORY}"
		GIT_TAG "${DICOMSDL_OPENJPH_GIT_TAG}"
		GIT_PROGRESS TRUE
	)
	FetchContent_MakeAvailable(dicomsdl_openjph)

	if(_dicomsdl_openjph_build_shared_libs_defined)
		set(BUILD_SHARED_LIBS "${_dicomsdl_openjph_build_shared_libs}")
	else()
		unset(BUILD_SHARED_LIBS)
	endif()

	if(_dicomsdl_openjph_build_shared_libs_cache_defined)
		set(BUILD_SHARED_LIBS "${_dicomsdl_openjph_build_shared_libs_cache}" CACHE BOOL "" FORCE)
	else()
		unset(BUILD_SHARED_LIBS CACHE)
	endif()
	unset(_dicomsdl_openjph_build_shared_libs)
	unset(_dicomsdl_openjph_build_shared_libs_defined)
	unset(_dicomsdl_openjph_build_shared_libs_cache)
	unset(_dicomsdl_openjph_build_shared_libs_cache_defined)
endif()

if(DICOMSDL_ENABLE_JPEGXL)
	if(DEFINED BUILD_SHARED_LIBS)
		set(_dicomsdl_jpegxl_build_shared_libs_defined TRUE)
		set(_dicomsdl_jpegxl_build_shared_libs "${BUILD_SHARED_LIBS}")
	else()
		set(_dicomsdl_jpegxl_build_shared_libs_defined FALSE)
	endif()
	if(DEFINED CACHE{BUILD_SHARED_LIBS})
		set(_dicomsdl_jpegxl_build_shared_libs_cache_defined TRUE)
		get_property(
			_dicomsdl_jpegxl_build_shared_libs_cache
			CACHE BUILD_SHARED_LIBS
			PROPERTY VALUE
		)
	else()
		set(_dicomsdl_jpegxl_build_shared_libs_cache_defined FALSE)
	endif()

	if(DEFINED BUILD_TESTING)
		set(_dicomsdl_jpegxl_build_testing_defined TRUE)
		set(_dicomsdl_jpegxl_build_testing "${BUILD_TESTING}")
	else()
		set(_dicomsdl_jpegxl_build_testing_defined FALSE)
	endif()
	if(DEFINED CACHE{BUILD_TESTING})
		set(_dicomsdl_jpegxl_build_testing_cache_defined TRUE)
		get_property(
			_dicomsdl_jpegxl_build_testing_cache
			CACHE BUILD_TESTING
			PROPERTY VALUE
		)
	else()
		set(_dicomsdl_jpegxl_build_testing_cache_defined FALSE)
	endif()

	set(BUILD_SHARED_LIBS OFF)
	set(BUILD_SHARED_LIBS OFF CACHE BOOL "Force static libjxl build in dicomsdl superbuild" FORCE)
	set(BUILD_TESTING OFF)
	set(BUILD_TESTING OFF CACHE BOOL "Disable libjxl tests in dicomsdl superbuild" FORCE)

	set(JPEGXL_ENABLE_FUZZERS OFF CACHE BOOL "Disable libjxl fuzzers in dicomsdl superbuild" FORCE)
	set(JPEGXL_ENABLE_DEVTOOLS OFF CACHE BOOL "Disable libjxl dev tools in dicomsdl superbuild" FORCE)
	set(JPEGXL_ENABLE_TOOLS OFF CACHE BOOL "Disable libjxl CLI tools in dicomsdl superbuild" FORCE)
	set(JPEGXL_ENABLE_BENCHMARK OFF CACHE BOOL "Disable libjxl benchmarks in dicomsdl superbuild" FORCE)
	set(JPEGXL_ENABLE_EXAMPLES OFF CACHE BOOL "Disable libjxl examples in dicomsdl superbuild" FORCE)
	set(JPEGXL_ENABLE_DOXYGEN OFF CACHE BOOL "Disable libjxl doxygen in dicomsdl superbuild" FORCE)
	set(JPEGXL_ENABLE_MANPAGES OFF CACHE BOOL "Disable libjxl manpages in dicomsdl superbuild" FORCE)
	set(JPEGXL_ENABLE_JNI OFF CACHE BOOL "Disable libjxl JNI wrapper in dicomsdl superbuild" FORCE)
	set(JPEGXL_ENABLE_OPENEXR OFF CACHE BOOL "Disable libjxl OpenEXR integration in dicomsdl superbuild" FORCE)
	set(JPEGXL_ENABLE_VIEWERS OFF CACHE BOOL "Disable libjxl viewers in dicomsdl superbuild" FORCE)
	set(JPEGXL_ENABLE_PLUGINS OFF CACHE BOOL "Disable libjxl plugins in dicomsdl superbuild" FORCE)
	set(JPEGXL_ENABLE_SJPEG OFF CACHE BOOL "Disable libjxl sjpeg path in dicomsdl superbuild" FORCE)
	set(JPEGXL_ENABLE_JPEGLI OFF CACHE BOOL "Disable libjxl jpegli path in dicomsdl superbuild" FORCE)
	set(JPEGXL_ENABLE_TCMALLOC OFF CACHE BOOL "Disable libjxl tcmalloc in dicomsdl superbuild" FORCE)
	set(JPEGXL_BUNDLE_LIBPNG OFF CACHE BOOL "Disable libjxl bundled libpng in dicomsdl superbuild" FORCE)
	set(JPEGXL_ENABLE_SKCMS ON CACHE BOOL "Enable skcms for libjxl in dicomsdl superbuild" FORCE)

	FetchContent_Declare(dicomsdl_libjxl
		GIT_REPOSITORY "${DICOMSDL_LIBJXL_GIT_REPOSITORY}"
		GIT_TAG "${DICOMSDL_LIBJXL_GIT_TAG}"
		GIT_PROGRESS TRUE
		GIT_SUBMODULES "third_party/highway;third_party/brotli;third_party/skcms"
		GIT_SUBMODULES_RECURSE FALSE
	)
	FetchContent_MakeAvailable(dicomsdl_libjxl)

	if(_dicomsdl_jpegxl_build_shared_libs_defined)
		set(BUILD_SHARED_LIBS "${_dicomsdl_jpegxl_build_shared_libs}")
	else()
		unset(BUILD_SHARED_LIBS)
	endif()
	if(_dicomsdl_jpegxl_build_shared_libs_cache_defined)
		set(BUILD_SHARED_LIBS "${_dicomsdl_jpegxl_build_shared_libs_cache}" CACHE BOOL "" FORCE)
	else()
		unset(BUILD_SHARED_LIBS CACHE)
	endif()
	if(_dicomsdl_jpegxl_build_testing_defined)
		set(BUILD_TESTING "${_dicomsdl_jpegxl_build_testing}")
	else()
		unset(BUILD_TESTING)
	endif()
	if(_dicomsdl_jpegxl_build_testing_cache_defined)
		set(BUILD_TESTING "${_dicomsdl_jpegxl_build_testing_cache}" CACHE BOOL "" FORCE)
	else()
		unset(BUILD_TESTING CACHE)
	endif()

	unset(_dicomsdl_jpegxl_build_shared_libs_defined)
	unset(_dicomsdl_jpegxl_build_shared_libs)
	unset(_dicomsdl_jpegxl_build_shared_libs_cache_defined)
	unset(_dicomsdl_jpegxl_build_shared_libs_cache)
	unset(_dicomsdl_jpegxl_build_testing_defined)
	unset(_dicomsdl_jpegxl_build_testing)
	unset(_dicomsdl_jpegxl_build_testing_cache_defined)
	unset(_dicomsdl_jpegxl_build_testing_cache)
endif()

FetchContent_Declare(dicomsdl_libjpeg_turbo
	GIT_REPOSITORY "${DICOMSDL_LIBJPEG_TURBO_GIT_REPOSITORY}"
	GIT_TAG "${DICOMSDL_LIBJPEG_TURBO_GIT_TAG}"
	GIT_PROGRESS TRUE
)
if(POLICY CMP0169)
	cmake_policy(PUSH)
	cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_GetProperties(dicomsdl_libjpeg_turbo)
if(NOT dicomsdl_libjpeg_turbo_POPULATED)
	FetchContent_Populate(dicomsdl_libjpeg_turbo)
endif()
if(POLICY CMP0169)
	cmake_policy(POP)
endif()

set(_dicomsdl_libturbojpeg_prefix "${CMAKE_CURRENT_BINARY_DIR}/extern/libjpeg-turbo")
set(_dicomsdl_libturbojpeg_build_dir "${_dicomsdl_libturbojpeg_prefix}/build")
set(_dicomsdl_libturbojpeg_install_dir "${_dicomsdl_libturbojpeg_prefix}/install")
set(_dicomsdl_libturbojpeg_cmake_args
	-DCMAKE_INSTALL_PREFIX=${_dicomsdl_libturbojpeg_install_dir}
	-DCMAKE_INSTALL_LIBDIR=lib
	-DCMAKE_INSTALL_INCLUDEDIR=include
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON
	-DENABLE_SHARED=OFF
	-DENABLE_STATIC=ON
	-DWITH_TURBOJPEG=ON
	-DWITH_JAVA=OFF
	-DWITH_TOOLS=OFF
	-DWITH_TESTS=OFF
	-DWITH_FUZZ=OFF
)
if(NOT CMAKE_CONFIGURATION_TYPES AND CMAKE_BUILD_TYPE)
	list(APPEND _dicomsdl_libturbojpeg_cmake_args
		-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
	)
endif()
if(CMAKE_TOOLCHAIN_FILE)
	list(APPEND _dicomsdl_libturbojpeg_cmake_args
		-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
	)
endif()
if(APPLE)
	if(CMAKE_OSX_ARCHITECTURES)
		list(APPEND _dicomsdl_libturbojpeg_cmake_args
			-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
		)
	endif()
	if(DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND NOT CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
		list(APPEND _dicomsdl_libturbojpeg_cmake_args
			-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
		)
	endif()
endif()

if(MSVC OR CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
	set(_dicomsdl_libturbojpeg_filename "turbojpeg-static${CMAKE_STATIC_LIBRARY_SUFFIX}")
else()
	set(_dicomsdl_libturbojpeg_filename "${CMAKE_STATIC_LIBRARY_PREFIX}turbojpeg${CMAKE_STATIC_LIBRARY_SUFFIX}")
endif()
set(_dicomsdl_libturbojpeg_archive
	"${_dicomsdl_libturbojpeg_install_dir}/lib/${_dicomsdl_libturbojpeg_filename}")

ExternalProject_Add(dicomsdl_libturbojpeg
	SOURCE_DIR "${dicomsdl_libjpeg_turbo_SOURCE_DIR}"
	BINARY_DIR "${_dicomsdl_libturbojpeg_build_dir}"
	PREFIX "${_dicomsdl_libturbojpeg_prefix}"
	CMAKE_ARGS ${_dicomsdl_libturbojpeg_cmake_args}
	BUILD_BYPRODUCTS "${_dicomsdl_libturbojpeg_archive}"
)

add_library(dicomsdl_turbojpeg STATIC IMPORTED GLOBAL)
set_target_properties(dicomsdl_turbojpeg PROPERTIES
	IMPORTED_LOCATION "${_dicomsdl_libturbojpeg_archive}"
)
add_dependencies(dicomsdl_turbojpeg dicomsdl_libturbojpeg)
add_dependencies(dicomsdl dicomsdl_libturbojpeg)

unset(EXECUTABLE_OUTPUT_PATH)
unset(EXECUTABLE_OUTPUT_PATH CACHE)
unset(LIBRARY_OUTPUT_PATH)
unset(LIBRARY_OUTPUT_PATH CACHE)

target_include_directories(dicomsdl PRIVATE
	${dicomsdl_openjpeg_SOURCE_DIR}/src/lib/openjp2
	${dicomsdl_openjpeg_BINARY_DIR}/src/lib/openjp2
	${dicomsdl_libjpeg_turbo_SOURCE_DIR}/src
	${dicomsdl_libjpeg_turbo_BINARY_DIR}
)
if(TARGET openjp2_static)
	target_link_libraries(dicomsdl PRIVATE openjp2_static)
else()
	target_link_libraries(dicomsdl PRIVATE openjp2)
endif()
if(DICOMSDL_ENABLE_OPENJPH)
	target_link_libraries(dicomsdl PRIVATE openjph)
	target_compile_definitions(dicomsdl PRIVATE DICOMSDL_HAS_OPENJPH=1)
else()
	target_compile_definitions(dicomsdl PRIVATE DICOMSDL_HAS_OPENJPH=0)
endif()
if(DICOMSDL_ENABLE_JPEGXL)
	if(NOT TARGET jxl OR NOT TARGET jxl_threads OR NOT TARGET jxl_cms)
		message(FATAL_ERROR "libjxl targets (jxl/jxl_threads/jxl_cms) were not generated")
	endif()
	target_link_libraries(dicomsdl PRIVATE jxl jxl_threads jxl_cms)
	target_compile_definitions(dicomsdl PRIVATE DICOMSDL_HAS_JPEGXL=1)
else()
	target_compile_definitions(dicomsdl PRIVATE DICOMSDL_HAS_JPEGXL=0)
endif()

function(dicomsdl_apply_codec_builtin_defines TARGET_NAME)
	foreach(CODEC_UPPER IN ITEMS JPEG JPEGLS JPEG2K HTJ2K JPEGXL)
		if(DICOMSDL_CODEC_${CODEC_UPPER}_BUILTIN)
			target_compile_definitions(${TARGET_NAME}
				PRIVATE DICOMSDL_CODEC_${CODEC_UPPER}_BUILTIN=1)
		else()
			target_compile_definitions(${TARGET_NAME}
				PRIVATE DICOMSDL_CODEC_${CODEC_UPPER}_BUILTIN=0)
		endif()
	endforeach()
	if(DICOMSDL_ENABLE_OPENJPH)
		target_compile_definitions(${TARGET_NAME} PRIVATE DICOMSDL_HAS_OPENJPH=1)
	else()
		target_compile_definitions(${TARGET_NAME} PRIVATE DICOMSDL_HAS_OPENJPH=0)
	endif()
	if(DICOMSDL_ENABLE_JPEGXL)
		target_compile_definitions(${TARGET_NAME} PRIVATE DICOMSDL_HAS_JPEGXL=1)
	else()
		target_compile_definitions(${TARGET_NAME} PRIVATE DICOMSDL_HAS_JPEGXL=0)
	endif()
endfunction()

dicomsdl_apply_codec_builtin_defines(dicomsdl)

target_link_libraries(dicomsdl PRIVATE charls)
target_link_libraries(dicomsdl PRIVATE dicomsdl_turbojpeg)
if(TARGET libdeflate::libdeflate_static)
	target_link_libraries(dicomsdl PRIVATE libdeflate::libdeflate_static)
elseif(TARGET libdeflate_static)
	target_link_libraries(dicomsdl PRIVATE libdeflate_static)
else()
	message(FATAL_ERROR "libdeflate static target was not generated")
endif()
target_compile_definitions(dicomsdl PRIVATE DICOMSDL_HAS_LIBDEFLATE=1)
option(DICOMSDL_ENABLE_INFO_LOGS "Compile in INFO-level logging (otherwise info logs are compiled out)" OFF)
if(DICOMSDL_ENABLE_INFO_LOGS)
	target_compile_definitions(dicomsdl PUBLIC DICOMSDL_ENABLE_INFO_LOGS=1)
else()
	target_compile_definitions(dicomsdl PUBLIC DICOMSDL_ENABLE_INFO_LOGS=0)
endif()
target_compile_features(dicomsdl PUBLIC cxx_std_20)

function(dicomsdl_add_shared_codec_plugin TARGET_SUFFIX WRAPPER_SOURCE)
	set(TARGET_NAME "dicomsdl_codec_plugin_${TARGET_SUFFIX}")
	add_library(${TARGET_NAME} SHARED
		${WRAPPER_SOURCE}
	)
	target_include_directories(${TARGET_NAME}
		PRIVATE
			${DICOM_INCLUDE_DIR}
			${GENERATED_INCLUDE_DIR}
			${CMAKE_CURRENT_SOURCE_DIR}/src
	)
	target_link_libraries(${TARGET_NAME} PRIVATE dicomsdl)
	target_compile_features(${TARGET_NAME} PRIVATE cxx_std_20)
	set_target_properties(${TARGET_NAME} PROPERTIES
		LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		OUTPUT_NAME "dicomsdl_codec_${TARGET_SUFFIX}_plugin"
	)
endfunction()

if(DICOMSDL_CODEC_JPEG_SHARED)
	dicomsdl_add_shared_codec_plugin(jpeg
		src/pixel/plugin_abi/shared/jpeg_plugin_shared.cpp
	)
endif()
if(DICOMSDL_CODEC_JPEGLS_SHARED)
	dicomsdl_add_shared_codec_plugin(jpegls
		src/pixel/plugin_abi/shared/jpegls_plugin_shared.cpp
	)
endif()
if(DICOMSDL_CODEC_JPEG2K_SHARED)
	dicomsdl_add_shared_codec_plugin(jpeg2k
		src/pixel/plugin_abi/shared/jpeg2k_plugin_shared.cpp
	)
endif()
if(DICOMSDL_CODEC_HTJ2K_SHARED)
	dicomsdl_add_shared_codec_plugin(htj2k
		src/pixel/plugin_abi/shared/htj2k_plugin_shared.cpp
	)
endif()
if(DICOMSDL_CODEC_JPEGXL_SHARED)
	dicomsdl_add_shared_codec_plugin(jpegxl
		src/pixel/plugin_abi/shared/jpegxl_plugin_shared.cpp
	)
endif()

option(DICOM_BUILD_EXAMPLES "Build example executables for DicomProject" ON)

if(DICOM_BUILD_EXAMPLES)
	add_executable(keyword_lookup_example
		examples/keyword_lookup_example.cpp
	)
	target_link_libraries(keyword_lookup_example PRIVATE dicomsdl)

	add_executable(tag_lookup_example
		examples/tag_lookup_example.cpp
	)
	target_link_libraries(tag_lookup_example PRIVATE dicomsdl)

	add_executable(uid_lookup_example
		examples/uid_lookup_example.cpp
	)
	target_link_libraries(uid_lookup_example PRIVATE dicomsdl)

	add_executable(dump_dataset_example
		examples/dump_dataset_example.cpp
	)
	target_link_libraries(dump_dataset_example PRIVATE dicomsdl)

	add_executable(simple_read
		examples/simple_read.cpp
	)
	target_link_libraries(simple_read PRIVATE dicomsdl)

	add_executable(dicomdump
		examples/dicomdump.cpp
	)
	target_link_libraries(dicomdump PRIVATE dicomsdl)

	add_executable(dicomconv
		examples/dicomconv.cpp
	)
	target_link_libraries(dicomconv PRIVATE dicomsdl)
	target_include_directories(dicomconv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

	add_executable(batch_assign_with_error_check
		examples/batch_assign_with_error_check.cpp
	)
	target_link_libraries(batch_assign_with_error_check PRIVATE dicomsdl)
endif()

option(DICOM_BUILD_PYTHON "Build Python bindings with nanobind" ON)
option(DICOM_PYTHON_STABLE_ABI "Build Python bindings with nanobind STABLE_ABI (abi3, CPython>=3.12)" OFF)

if(DICOM_BUILD_PYTHON)
	find_package(Python REQUIRED COMPONENTS Interpreter Development.Module OPTIONAL_COMPONENTS Development.SABIModule)
	FetchContent_Declare(dicomsdl_nanobind
		GIT_REPOSITORY "${DICOMSDL_NANOBIND_GIT_REPOSITORY}"
		GIT_TAG "${DICOMSDL_NANOBIND_GIT_TAG}"
		GIT_PROGRESS TRUE
		GIT_SUBMODULES "ext/robin_map"
		GIT_SUBMODULES_RECURSE FALSE
	)
	FetchContent_MakeAvailable(dicomsdl_nanobind)
	if(DICOM_PYTHON_STABLE_ABI)
		nanobind_add_module(_dicomsdl STABLE_ABI NOMINSIZE
			bindings/python/dicom_module.cpp
		)
	else()
		nanobind_add_module(_dicomsdl NOMINSIZE
			bindings/python/dicom_module.cpp
		)
	endif()
	target_link_libraries(_dicomsdl PRIVATE dicomsdl fmt::fmt-header-only)
endif()

if(BUILD_TESTING)
	add_executable(dicomsdl_dictionary_consistency
		tests/dictionary_consistency.cpp
	)
	target_link_libraries(dicomsdl_dictionary_consistency PRIVATE dicomsdl)
	add_test(NAME dictionary_consistency COMMAND dicomsdl_dictionary_consistency)
	set_tests_properties(dictionary_consistency PROPERTIES LABELS "dicomsdl")

	add_executable(dicomsdl_basic_smoke
		tests/basic_smoke.cpp
	)
	target_link_libraries(dicomsdl_basic_smoke PRIVATE dicomsdl)
	add_test(NAME basic_smoke COMMAND dicomsdl_basic_smoke)
	set_tests_properties(basic_smoke PROPERTIES LABELS "dicomsdl")

	add_executable(dicomsdl_numeric_values
		tests/numeric_values.cpp
	)
	target_link_libraries(dicomsdl_numeric_values PRIVATE dicomsdl)
	add_test(NAME numeric_values COMMAND dicomsdl_numeric_values)
	set_tests_properties(numeric_values PROPERTIES
		LABELS "dicomsdl"
		WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
	)

	add_executable(dicomsdl_uid_consistency
		tests/uid_consistency.cpp
	)
	target_link_libraries(dicomsdl_uid_consistency PRIVATE dicomsdl)
	add_test(NAME uid_consistency COMMAND dicomsdl_uid_consistency)
	set_tests_properties(uid_consistency PROPERTIES LABELS "dicomsdl")

	add_executable(dicomsdl_separation_regression
		tests/separation_regression.cpp
	)
	target_link_libraries(dicomsdl_separation_regression PRIVATE dicomsdl)
	add_test(NAME separation_regression COMMAND dicomsdl_separation_regression)
	set_tests_properties(separation_regression PROPERTIES LABELS "dicomsdl")

	add_executable(dicomsdl_dataset_ordering_regression
		tests/dataset_ordering_regression.cpp
	)
	target_link_libraries(dicomsdl_dataset_ordering_regression PRIVATE dicomsdl)
	add_test(NAME dataset_ordering_regression COMMAND dicomsdl_dataset_ordering_regression)
	set_tests_properties(dataset_ordering_regression PROPERTIES LABELS "dicomsdl")

	add_executable(dicomsdl_codec_cycle_roundtrip
		tests/codec_cycle_roundtrip.cpp
	)
	target_link_libraries(dicomsdl_codec_cycle_roundtrip PRIVATE dicomsdl)
	add_test(NAME codec_cycle_roundtrip COMMAND dicomsdl_codec_cycle_roundtrip)
	set_tests_properties(codec_cycle_roundtrip PROPERTIES LABELS "dicomsdl")

	add_executable(dicomsdl_codec_error_boundary
		tests/codec_error_boundary.cpp
	)
	target_link_libraries(dicomsdl_codec_error_boundary PRIVATE dicomsdl)
	add_test(NAME codec_error_boundary COMMAND dicomsdl_codec_error_boundary)
	set_tests_properties(codec_error_boundary PROPERTIES LABELS "dicomsdl")

	add_executable(dicomsdl_codec_cycle_realdata
		tests/codec_cycle_realdata.cpp
	)
	target_link_libraries(dicomsdl_codec_cycle_realdata PRIVATE dicomsdl)

	add_executable(dicomsdl_codec_plugin_abi_adapter
		tests/codec_plugin_abi_adapter.cpp
	)
	target_link_libraries(dicomsdl_codec_plugin_abi_adapter PRIVATE dicomsdl)
	add_test(NAME codec_plugin_abi_adapter COMMAND dicomsdl_codec_plugin_abi_adapter)
	set_tests_properties(codec_plugin_abi_adapter PROPERTIES LABELS "dicomsdl")

	add_executable(dicomsdl_codec_plugin_loader
		tests/codec_plugin_loader.cpp
	)
	target_link_libraries(dicomsdl_codec_plugin_loader PRIVATE dicomsdl)
	add_test(NAME codec_plugin_loader COMMAND dicomsdl_codec_plugin_loader)
	set_tests_properties(codec_plugin_loader PROPERTIES LABELS "dicomsdl")

	add_library(dicomsdl_test_codec_plugin SHARED
		tests/codec_plugin_dynamic_fixture.cpp
	)
	target_include_directories(dicomsdl_test_codec_plugin
		PRIVATE
			${DICOM_INCLUDE_DIR}
			${GENERATED_INCLUDE_DIR}
	)

	add_executable(dicomsdl_codec_plugin_dynamic_load
		tests/codec_plugin_dynamic_load.cpp
	)
	target_link_libraries(dicomsdl_codec_plugin_dynamic_load PRIVATE dicomsdl)
	add_dependencies(dicomsdl_codec_plugin_dynamic_load dicomsdl_test_codec_plugin)
	add_test(NAME codec_plugin_dynamic_load
		COMMAND dicomsdl_codec_plugin_dynamic_load
		$<TARGET_FILE:dicomsdl_test_codec_plugin>)
	set_tests_properties(codec_plugin_dynamic_load PROPERTIES LABELS "dicomsdl")

	add_executable(dicomsdl_codec_plugin_registration
		tests/codec_plugin_registration.cpp
	)
	target_link_libraries(dicomsdl_codec_plugin_registration PRIVATE dicomsdl)
	add_test(NAME codec_plugin_registration COMMAND dicomsdl_codec_plugin_registration)
	set_tests_properties(codec_plugin_registration PROPERTIES LABELS "dicomsdl")

	foreach(_dicomsdl_test_target IN ITEMS
		dicomsdl_dictionary_consistency
		dicomsdl_basic_smoke
		dicomsdl_numeric_values
		dicomsdl_uid_consistency
		dicomsdl_separation_regression
		dicomsdl_dataset_ordering_regression
		dicomsdl_codec_cycle_roundtrip
		dicomsdl_codec_error_boundary
		dicomsdl_codec_cycle_realdata
		dicomsdl_codec_plugin_abi_adapter
		dicomsdl_codec_plugin_loader
		dicomsdl_test_codec_plugin
		dicomsdl_codec_plugin_dynamic_load
		dicomsdl_codec_plugin_registration
	)
		if(TARGET ${_dicomsdl_test_target})
			dicomsdl_apply_codec_builtin_defines(${_dicomsdl_test_target})
		endif()
	endforeach()
	unset(_dicomsdl_test_target)
endif()
