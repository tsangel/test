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
# Release: CharLS 2.4.3 (GitHub latest release, checked 2026-03-04)
set(DICOMSDL_CHARLS_GIT_TAG "2.4.3"
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

add_library(dicomsdl STATIC
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
	src/pixel/external_plugin.cpp
	src/pixel/entrypoint/decode_entrypoint.cpp
	src/pixel/decode/core/decode_entrypoint_runner.cpp
	src/pixel/decode/core/decode_modality_transform.cpp
	src/pixel/decode/core/decode_frame_dispatch.cpp
	src/pixel/decode/core/decode_frame_source_loader.cpp
	src/pixel/decode/core/decode_planar_copy.cpp
	src/pixel/entrypoint/encode_entrypoint.cpp
	src/pixel/encode/codecs/encapsulated_encode.cpp
	src/pixel/encode/codecs/jpeg2k_encode.cpp
	src/pixel/encode/codecs/htj2k_encode.cpp
	src/pixel/encode/codecs/jpegls_encode.cpp
	src/pixel/encode/codecs/jpeg_encode.cpp
	src/pixel/encode/codecs/jpegxl_encode.cpp
	src/pixel/encode/core/encode_set_pixel_data_runner.cpp
	src/pixel/encode/core/encode_source_layout_resolver.cpp
	src/pixel/encode/core/encode_target_resolver.cpp
	src/pixel/encode/core/native_pixel_copy.cpp
	src/pixel/encode/core/multicomponent_option_resolver.cpp
	src/pixel/encode/core/encode_metadata_updater.cpp
	)
set_target_properties(dicomsdl PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(dicomsdl PROPERTIES
	ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
	LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
	RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
target_include_directories(dicomsdl
	PUBLIC
		${DICOM_INCLUDE_DIR}
		${GENERATED_INCLUDE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/abi
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
	"Enable static runtime JPEG plugin." ON)
option(DICOMSDL_CODEC_JPEG_SHARED
	"Build runtime JPEG shared plugin target." OFF)
option(DICOMSDL_CODEC_JPEGLS_BUILTIN
	"Enable static runtime JPEG-LS plugin." ON)
option(DICOMSDL_CODEC_JPEGLS_SHARED
	"Build runtime JPEG-LS shared plugin target." OFF)
option(DICOMSDL_CODEC_JPEG2K_BUILTIN
	"Enable static runtime JPEG2000 plugin." ON)
option(DICOMSDL_CODEC_JPEG2K_SHARED
	"Build runtime JPEG2000 shared plugin target." OFF)
option(DICOMSDL_CODEC_HTJ2K_BUILTIN
	"Enable static runtime HTJ2K plugin." ON)
option(DICOMSDL_CODEC_HTJ2K_SHARED
	"Build runtime HTJ2K shared plugin target." OFF)
option(DICOMSDL_CODEC_JPEGXL_BUILTIN
	"Enable static runtime JPEG XL plugin." ON)
option(DICOMSDL_CODEC_JPEGXL_SHARED
	"Build runtime JPEG XL shared plugin target." OFF)
option(DICOMSDL_PIXEL_HTJ2K_PLUGIN
	"Build src/pixel_ HTJ2K shared plugin target." OFF)
option(DICOMSDL_PIXEL_JPEGLS_PLUGIN
	"Build src/pixel_ JPEG-LS shared plugin target." OFF)
option(DICOMSDL_PIXEL_OPENJPEG_PLUGIN
	"Build src/pixel_ OpenJPEG shared plugin target." OFF)
option(DICOMSDL_PIXEL_OPENJPEG_STATIC_PLUGIN
	"Build src/pixel_ OpenJPEG static plugin target." ON)
option(DICOMSDL_PIXEL_JPEGXL_PLUGIN
	"Build src/pixel_ JPEG-XL shared plugin target." OFF)
option(DICOMSDL_PIXEL_JPEGXL_STATIC_PLUGIN
	"Build src/pixel_ JPEG-XL static plugin target." ${DICOMSDL_ENABLE_JPEGXL})
option(DICOMSDL_PIXEL_JPEG_PLUGIN
	"Build src/pixel_ JPEG shared plugin target." OFF)
option(DICOMSDL_PIXEL_JPEG_STATIC_PLUGIN
	"Build src/pixel_ JPEG static plugin target." ON)
option(DICOMSDL_PIXEL_JPEGLS_STATIC_PLUGIN
	"Build src/pixel_ JPEG-LS static plugin target." ON)
option(DICOMSDL_PIXEL_HTJ2K_STATIC_PLUGIN
	"Build src/pixel_ HTJ2K static plugin target." ${DICOMSDL_ENABLE_OPENJPH})
option(DICOMSDL_PIXEL_CORE
	"Build src/pixel_ core uncompressed helpers." ON)
option(DICOMSDL_PIXEL_RLE_STATIC_PLUGIN
	"Build src/pixel_ static RLE plugin target." ON)
option(DICOMSDL_PIXEL_RUNTIME
	"Build src/pixel_ runtime registry target." ON)

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

foreach(_dicomsdl_codec IN ITEMS JPEG JPEGLS JPEG2K HTJ2K JPEGXL)
	if(DICOMSDL_CODEC_${_dicomsdl_codec}_SHARED)
		message(WARNING
			"DICOMSDL_CODEC_${_dicomsdl_codec}_SHARED uses a removed plugin ABI path and is ignored. "
			"Use DICOMSDL_PIXEL_*_PLUGIN options instead.")
	endif()
endforeach()
unset(_dicomsdl_codec)

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
if(DEFINED BUILD_CODEC)
	set(_dicomsdl_openjpeg_build_codec_defined TRUE)
	set(_dicomsdl_openjpeg_build_codec "${BUILD_CODEC}")
else()
	set(_dicomsdl_openjpeg_build_codec_defined FALSE)
endif()
if(DEFINED CACHE{BUILD_CODEC})
	get_property(_dicomsdl_openjpeg_build_codec_cache_type
		CACHE BUILD_CODEC PROPERTY TYPE)
	set(_dicomsdl_openjpeg_build_codec_cache_defined TRUE)
	set(_dicomsdl_openjpeg_build_codec_cache "${BUILD_CODEC}")
else()
	set(_dicomsdl_openjpeg_build_codec_cache_defined FALSE)
endif()
# Keep OpenJPEG tests disabled in this superbuild. Otherwise CTest registers
# hundreds of OpenJPEG cases while EXCLUDE_FROM_ALL prevents their binaries
# from being built, which causes mass "Not Run" failures.
set(BUILD_TESTING OFF)
# Disable OpenJPEG CLI tools (opj_compress/opj_decompress/opj_dump) to avoid
# optional external image stack linkage (png/tiff/lcms2) in default dicomsdl builds.
set(BUILD_CODEC OFF CACHE BOOL
	"Disable OpenJPEG codec CLI tools in dicomsdl superbuild" FORCE)
FetchContent_Declare(dicomsdl_openjpeg
	GIT_REPOSITORY "${DICOMSDL_OPENJPEG_GIT_REPOSITORY}"
	GIT_TAG "${DICOMSDL_OPENJPEG_GIT_TAG}"
	GIT_PROGRESS TRUE
)
FetchContent_MakeAvailable(dicomsdl_openjpeg)
set(BUILD_TESTING "${_dicomsdl_build_testing}")
unset(_dicomsdl_build_testing)
if(_dicomsdl_openjpeg_build_codec_defined)
	set(BUILD_CODEC "${_dicomsdl_openjpeg_build_codec}")
else()
	unset(BUILD_CODEC)
endif()
unset(_dicomsdl_openjpeg_build_codec)
unset(_dicomsdl_openjpeg_build_codec_defined)
if(_dicomsdl_openjpeg_build_codec_cache_defined)
	set(BUILD_CODEC "${_dicomsdl_openjpeg_build_codec_cache}"
		CACHE ${_dicomsdl_openjpeg_build_codec_cache_type} "" FORCE)
else()
	unset(BUILD_CODEC CACHE)
endif()
unset(_dicomsdl_openjpeg_build_codec_cache)
unset(_dicomsdl_openjpeg_build_codec_cache_defined)
unset(_dicomsdl_openjpeg_build_codec_cache_type)

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

if(DICOMSDL_ENABLE_OPENJPH OR DICOMSDL_PIXEL_HTJ2K_PLUGIN)
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

if(DICOMSDL_ENABLE_JPEGXL OR DICOMSDL_PIXEL_JPEGXL_PLUGIN)
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
	set(_dicomsdl_libturbojpeg_osx_architectures "${CMAKE_OSX_ARCHITECTURES}")
	if(NOT _dicomsdl_libturbojpeg_osx_architectures)
		string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _dicomsdl_libturbojpeg_system_processor_lc)
		if(_dicomsdl_libturbojpeg_system_processor_lc STREQUAL "aarch64")
			set(_dicomsdl_libturbojpeg_osx_architectures "arm64")
		elseif(_dicomsdl_libturbojpeg_system_processor_lc STREQUAL "amd64")
			set(_dicomsdl_libturbojpeg_osx_architectures "x86_64")
		elseif(_dicomsdl_libturbojpeg_system_processor_lc STREQUAL "arm64" OR
			_dicomsdl_libturbojpeg_system_processor_lc STREQUAL "x86_64" OR
			_dicomsdl_libturbojpeg_system_processor_lc STREQUAL "i386")
			set(_dicomsdl_libturbojpeg_osx_architectures "${_dicomsdl_libturbojpeg_system_processor_lc}")
		endif()
	endif()
	if(_dicomsdl_libturbojpeg_osx_architectures)
		list(APPEND _dicomsdl_libturbojpeg_cmake_args
			-DCMAKE_OSX_ARCHITECTURES=${_dicomsdl_libturbojpeg_osx_architectures}
		)
	endif()
	if(DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND NOT CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
		list(APPEND _dicomsdl_libturbojpeg_cmake_args
			-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
		)
	endif()
	unset(_dicomsdl_libturbojpeg_system_processor_lc)
	unset(_dicomsdl_libturbojpeg_osx_architectures)
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

if(DICOMSDL_PIXEL_CORE)
	add_library(dicomsdl_pixel_core STATIC
		src/pixel_/core/common.cpp
		src/pixel_/core/decode_uncompressed.cpp
		src/pixel_/core/encode_uncompressed.cpp
	)
	target_include_directories(dicomsdl_pixel_core
		PUBLIC
			${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/abi
			${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/core
	)
	target_compile_features(dicomsdl_pixel_core PRIVATE cxx_std_20)
	set_target_properties(dicomsdl_pixel_core PROPERTIES
		ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		OUTPUT_NAME "dicomsdl_pixel_core"
	)
endif()

if(DICOMSDL_PIXEL_RLE_STATIC_PLUGIN)
	add_library(dicomsdl_pixel_rle_plugin_static STATIC
		src/pixel_/plugins/static/rle_v2/common.cpp
		src/pixel_/plugins/static/rle_v2/decode.cpp
		src/pixel_/plugins/static/rle_v2/encode.cpp
		src/pixel_/plugins/static/rle_v2/entry.cpp
	)
	target_include_directories(dicomsdl_pixel_rle_plugin_static
		PUBLIC
			${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/abi
			${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/plugins/static/rle_v2
	)
	target_compile_features(dicomsdl_pixel_rle_plugin_static PRIVATE cxx_std_20)
	set_target_properties(dicomsdl_pixel_rle_plugin_static PROPERTIES
		ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		OUTPUT_NAME "dicomsdl_pixel_rle_plugin_static"
	)
endif()

if(DICOMSDL_PIXEL_OPENJPEG_STATIC_PLUGIN)
	add_library(dicomsdl_pixel_openjpeg_plugin_static STATIC
		src/pixel_/plugins/shared/openjpeg_v2/error.cpp
		src/pixel_/plugins/shared/openjpeg_v2/value_utils.cpp
		src/pixel_/plugins/shared/openjpeg_v2/openjpeg_helpers.cpp
		src/pixel_/plugins/shared/openjpeg_v2/context_api.cpp
		src/pixel_/plugins/shared/openjpeg_v2/decode.cpp
		src/pixel_/plugins/shared/openjpeg_v2/encode.cpp
		src/pixel_/plugins/shared/openjpeg_v2/static_api.cpp
	)
	target_include_directories(dicomsdl_pixel_openjpeg_plugin_static
		PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/abi
			${dicomsdl_openjpeg_SOURCE_DIR}/src/lib/openjp2
			${dicomsdl_openjpeg_BINARY_DIR}/src/lib/openjp2
	)
	if(TARGET openjp2_static)
		target_link_libraries(dicomsdl_pixel_openjpeg_plugin_static PRIVATE openjp2_static)
	else()
		target_link_libraries(dicomsdl_pixel_openjpeg_plugin_static PRIVATE openjp2)
	endif()
	target_compile_features(dicomsdl_pixel_openjpeg_plugin_static PRIVATE cxx_std_20)
	set_target_properties(dicomsdl_pixel_openjpeg_plugin_static PROPERTIES
		ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		OUTPUT_NAME "dicomsdl_pixel_openjpeg_plugin_static"
	)
endif()

if(DICOMSDL_PIXEL_JPEG_STATIC_PLUGIN)
	add_library(dicomsdl_pixel_jpeg_plugin_static STATIC
		src/pixel_/plugins/shared/jpeg_v2/common.cpp
		src/pixel_/plugins/shared/jpeg_v2/decode.cpp
		src/pixel_/plugins/shared/jpeg_v2/encode.cpp
		src/pixel_/plugins/shared/jpeg_v2/static_api.cpp
	)
	target_include_directories(dicomsdl_pixel_jpeg_plugin_static
		PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/abi
			${dicomsdl_libjpeg_turbo_SOURCE_DIR}/src
			${dicomsdl_libjpeg_turbo_BINARY_DIR}
	)
	target_link_libraries(dicomsdl_pixel_jpeg_plugin_static PRIVATE dicomsdl_turbojpeg)
	target_compile_features(dicomsdl_pixel_jpeg_plugin_static PRIVATE cxx_std_20)
	set_target_properties(dicomsdl_pixel_jpeg_plugin_static PROPERTIES
		ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		OUTPUT_NAME "dicomsdl_pixel_jpeg_plugin_static"
	)
endif()

if(DICOMSDL_PIXEL_JPEGLS_STATIC_PLUGIN)
	add_library(dicomsdl_pixel_jpegls_plugin_static STATIC
		src/pixel_/plugins/shared/jpegls_v2/common.cpp
		src/pixel_/plugins/shared/jpegls_v2/decode.cpp
		src/pixel_/plugins/shared/jpegls_v2/encode.cpp
		src/pixel_/plugins/shared/jpegls_v2/static_api.cpp
	)
	target_include_directories(dicomsdl_pixel_jpegls_plugin_static
		PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/abi
	)
	target_link_libraries(dicomsdl_pixel_jpegls_plugin_static PRIVATE charls)
	target_compile_features(dicomsdl_pixel_jpegls_plugin_static PRIVATE cxx_std_20)
	set_target_properties(dicomsdl_pixel_jpegls_plugin_static PROPERTIES
		ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		OUTPUT_NAME "dicomsdl_pixel_jpegls_plugin_static"
	)
endif()

if(DICOMSDL_PIXEL_HTJ2K_STATIC_PLUGIN)
	if(NOT TARGET openjph)
		message(FATAL_ERROR "DICOMSDL_PIXEL_HTJ2K_STATIC_PLUGIN requires OpenJPH (target openjph).")
	endif()
	add_library(dicomsdl_pixel_htj2k_plugin_static STATIC
		src/pixel_/plugins/shared/htj2k_v2/common.cpp
		src/pixel_/plugins/shared/htj2k_v2/decode.cpp
		src/pixel_/plugins/shared/htj2k_v2/encode.cpp
		src/pixel_/plugins/shared/htj2k_v2/static_api.cpp
	)
	target_include_directories(dicomsdl_pixel_htj2k_plugin_static
		PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/abi
	)
	target_link_libraries(dicomsdl_pixel_htj2k_plugin_static PRIVATE openjph)
	target_compile_features(dicomsdl_pixel_htj2k_plugin_static PRIVATE cxx_std_20)
	set_target_properties(dicomsdl_pixel_htj2k_plugin_static PROPERTIES
		ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		OUTPUT_NAME "dicomsdl_pixel_htj2k_plugin_static"
	)
endif()

if(DICOMSDL_PIXEL_JPEGXL_STATIC_PLUGIN)
	if(NOT TARGET jxl OR NOT TARGET jxl_threads OR NOT TARGET jxl_cms)
		message(FATAL_ERROR "DICOMSDL_PIXEL_JPEGXL_STATIC_PLUGIN requires libjxl targets (jxl/jxl_threads/jxl_cms).")
	endif()
	add_library(dicomsdl_pixel_jpegxl_plugin_static STATIC
		src/pixel_/plugins/shared/jpegxl_v2/common.cpp
		src/pixel_/plugins/shared/jpegxl_v2/decode.cpp
		src/pixel_/plugins/shared/jpegxl_v2/encode.cpp
		src/pixel_/plugins/shared/jpegxl_v2/static_api.cpp
	)
	target_include_directories(dicomsdl_pixel_jpegxl_plugin_static
		PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/abi
	)
	target_link_libraries(dicomsdl_pixel_jpegxl_plugin_static PRIVATE jxl jxl_threads jxl_cms)
	target_compile_features(dicomsdl_pixel_jpegxl_plugin_static PRIVATE cxx_std_20)
	set_target_properties(dicomsdl_pixel_jpegxl_plugin_static PROPERTIES
		ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		OUTPUT_NAME "dicomsdl_pixel_jpegxl_plugin_static"
	)
endif()

if(DICOMSDL_PIXEL_RUNTIME)
	if(NOT TARGET dicomsdl_pixel_core)
		message(FATAL_ERROR "DICOMSDL_PIXEL_RUNTIME requires DICOMSDL_PIXEL_CORE.")
	endif()
	add_library(dicomsdl_pixel_runtime STATIC
		src/pixel_/runtime/plugin_registry_v2.cpp
		src/pixel_/runtime/shared_plugin_loader_v2.cpp
		src/pixel_/runtime/registry_bootstrap_v2.cpp
		src/pixel_/runtime/host_adapter_v2.cpp
		src/pixel_/runtime/runtime_registry_v2.cpp
	)
	target_include_directories(dicomsdl_pixel_runtime
		PUBLIC
			${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/abi
			${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/runtime
		PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/core
			${CMAKE_CURRENT_SOURCE_DIR}/include
			${CMAKE_CURRENT_BINARY_DIR}/generated/include
	)
	target_link_libraries(dicomsdl_pixel_runtime PRIVATE
		dicomsdl_pixel_core
		${CMAKE_DL_LIBS}
	)
	if(TARGET dicomsdl_pixel_rle_plugin_static)
		target_link_libraries(dicomsdl_pixel_runtime PRIVATE
			dicomsdl_pixel_rle_plugin_static
		)
		target_compile_definitions(dicomsdl_pixel_runtime PRIVATE
			DICOMSDL_PIXEL_RUNTIME_WITH_RLE_STATIC=1
		)
	endif()
	if(TARGET dicomsdl_pixel_openjpeg_plugin_static)
		target_link_libraries(dicomsdl_pixel_runtime PRIVATE
			dicomsdl_pixel_openjpeg_plugin_static
		)
		target_compile_definitions(dicomsdl_pixel_runtime PRIVATE
			DICOMSDL_PIXEL_RUNTIME_WITH_OPENJPEG_STATIC=1
		)
	endif()
	if(TARGET dicomsdl_pixel_jpeg_plugin_static)
		target_link_libraries(dicomsdl_pixel_runtime PRIVATE
			dicomsdl_pixel_jpeg_plugin_static
		)
		target_compile_definitions(dicomsdl_pixel_runtime PRIVATE
			DICOMSDL_PIXEL_RUNTIME_WITH_JPEG_STATIC=1
		)
	endif()
	if(TARGET dicomsdl_pixel_jpegls_plugin_static)
		target_link_libraries(dicomsdl_pixel_runtime PRIVATE
			dicomsdl_pixel_jpegls_plugin_static
		)
		target_compile_definitions(dicomsdl_pixel_runtime PRIVATE
			DICOMSDL_PIXEL_RUNTIME_WITH_JPEGLS_STATIC=1
		)
	endif()
	if(TARGET dicomsdl_pixel_htj2k_plugin_static)
		target_link_libraries(dicomsdl_pixel_runtime PRIVATE
			dicomsdl_pixel_htj2k_plugin_static
		)
		target_compile_definitions(dicomsdl_pixel_runtime PRIVATE
			DICOMSDL_PIXEL_RUNTIME_WITH_HTJ2K_STATIC=1
		)
	endif()
	if(TARGET dicomsdl_pixel_jpegxl_plugin_static)
		target_link_libraries(dicomsdl_pixel_runtime PRIVATE
			dicomsdl_pixel_jpegxl_plugin_static
		)
		target_compile_definitions(dicomsdl_pixel_runtime PRIVATE
			DICOMSDL_PIXEL_RUNTIME_WITH_JPEGXL_STATIC=1
		)
	endif()
	target_compile_features(dicomsdl_pixel_runtime PRIVATE cxx_std_20)
	set_target_properties(dicomsdl_pixel_runtime PROPERTIES
		ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		OUTPUT_NAME "dicomsdl_pixel_runtime"
	)
	target_link_libraries(dicomsdl PRIVATE dicomsdl_pixel_runtime)
	target_compile_definitions(dicomsdl PRIVATE
		DICOMSDL_PIXEL_RUNTIME_ENABLED=1
	)
endif()

if(DICOMSDL_PIXEL_OPENJPEG_PLUGIN)
	add_library(dicomsdl_pixel_openjpeg_plugin SHARED
		src/pixel_/plugins/shared/openjpeg_v2/error.cpp
		src/pixel_/plugins/shared/openjpeg_v2/value_utils.cpp
		src/pixel_/plugins/shared/openjpeg_v2/openjpeg_helpers.cpp
		src/pixel_/plugins/shared/openjpeg_v2/context_api.cpp
		src/pixel_/plugins/shared/openjpeg_v2/decode.cpp
		src/pixel_/plugins/shared/openjpeg_v2/encode.cpp
		src/pixel_/plugins/shared/openjpeg_v2/shared_entry.cpp
	)
	target_include_directories(dicomsdl_pixel_openjpeg_plugin
		PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/abi
			${dicomsdl_openjpeg_SOURCE_DIR}/src/lib/openjp2
			${dicomsdl_openjpeg_BINARY_DIR}/src/lib/openjp2
	)
	if(TARGET openjp2_static)
		target_link_libraries(dicomsdl_pixel_openjpeg_plugin PRIVATE openjp2_static)
	else()
		target_link_libraries(dicomsdl_pixel_openjpeg_plugin PRIVATE openjp2)
	endif()
	target_compile_features(dicomsdl_pixel_openjpeg_plugin PRIVATE cxx_std_20)
	set_target_properties(dicomsdl_pixel_openjpeg_plugin PROPERTIES
		LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		OUTPUT_NAME "dicomsdl_pixel_openjpeg_plugin"
	)
	if(APPLE)
		set_target_properties(dicomsdl_pixel_openjpeg_plugin PROPERTIES
			BUILD_RPATH "@loader_path"
			INSTALL_RPATH "@loader_path"
		)
	elseif(UNIX)
		set_target_properties(dicomsdl_pixel_openjpeg_plugin PROPERTIES
			BUILD_RPATH "\$ORIGIN"
			INSTALL_RPATH "\$ORIGIN"
		)
	endif()
endif()

if(DICOMSDL_PIXEL_JPEG_PLUGIN)
	add_library(dicomsdl_pixel_jpeg_plugin SHARED
		src/pixel_/plugins/shared/jpeg_v2/common.cpp
		src/pixel_/plugins/shared/jpeg_v2/decode.cpp
		src/pixel_/plugins/shared/jpeg_v2/encode.cpp
		src/pixel_/plugins/shared/jpeg_v2/shared_entry.cpp
	)
	target_include_directories(dicomsdl_pixel_jpeg_plugin
		PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/abi
			${dicomsdl_libjpeg_turbo_SOURCE_DIR}/src
			${dicomsdl_libjpeg_turbo_BINARY_DIR}
	)
	target_link_libraries(dicomsdl_pixel_jpeg_plugin PRIVATE dicomsdl_turbojpeg)
	target_compile_features(dicomsdl_pixel_jpeg_plugin PRIVATE cxx_std_20)
	set_target_properties(dicomsdl_pixel_jpeg_plugin PROPERTIES
		LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		OUTPUT_NAME "dicomsdl_pixel_jpeg_plugin"
	)
	if(APPLE)
		set_target_properties(dicomsdl_pixel_jpeg_plugin PROPERTIES
			BUILD_RPATH "@loader_path"
			INSTALL_RPATH "@loader_path"
		)
	elseif(UNIX)
		set_target_properties(dicomsdl_pixel_jpeg_plugin PROPERTIES
			BUILD_RPATH "\$ORIGIN"
			INSTALL_RPATH "\$ORIGIN"
		)
	endif()
endif()

if(DICOMSDL_PIXEL_JPEGLS_PLUGIN)
	add_library(dicomsdl_pixel_jpegls_plugin SHARED
		src/pixel_/plugins/shared/jpegls_v2/common.cpp
		src/pixel_/plugins/shared/jpegls_v2/decode.cpp
		src/pixel_/plugins/shared/jpegls_v2/encode.cpp
		src/pixel_/plugins/shared/jpegls_v2/shared_entry.cpp
	)
	target_include_directories(dicomsdl_pixel_jpegls_plugin
		PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/abi
	)
	target_link_libraries(dicomsdl_pixel_jpegls_plugin PRIVATE charls)
	target_compile_features(dicomsdl_pixel_jpegls_plugin PRIVATE cxx_std_20)
	set_target_properties(dicomsdl_pixel_jpegls_plugin PROPERTIES
		LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		OUTPUT_NAME "dicomsdl_pixel_jpegls_plugin"
	)
	if(APPLE)
		set_target_properties(dicomsdl_pixel_jpegls_plugin PROPERTIES
			BUILD_RPATH "@loader_path"
			INSTALL_RPATH "@loader_path"
		)
	elseif(UNIX)
		set_target_properties(dicomsdl_pixel_jpegls_plugin PROPERTIES
			BUILD_RPATH "\$ORIGIN"
			INSTALL_RPATH "\$ORIGIN"
		)
		endif()
endif()

if(DICOMSDL_PIXEL_HTJ2K_PLUGIN)
	if(NOT TARGET openjph)
		message(FATAL_ERROR "DICOMSDL_PIXEL_HTJ2K_PLUGIN requires OpenJPH (target openjph).")
	endif()
	add_library(dicomsdl_pixel_htj2k_plugin SHARED
		src/pixel_/plugins/shared/htj2k_v2/common.cpp
		src/pixel_/plugins/shared/htj2k_v2/decode.cpp
		src/pixel_/plugins/shared/htj2k_v2/encode.cpp
		src/pixel_/plugins/shared/htj2k_v2/shared_entry.cpp
	)
	target_include_directories(dicomsdl_pixel_htj2k_plugin
		PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/abi
	)
	target_link_libraries(dicomsdl_pixel_htj2k_plugin PRIVATE openjph)
	target_compile_features(dicomsdl_pixel_htj2k_plugin PRIVATE cxx_std_20)
	set_target_properties(dicomsdl_pixel_htj2k_plugin PROPERTIES
		LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		OUTPUT_NAME "dicomsdl_pixel_htj2k_plugin"
	)
	if(APPLE)
		set_target_properties(dicomsdl_pixel_htj2k_plugin PROPERTIES
			BUILD_RPATH "@loader_path"
			INSTALL_RPATH "@loader_path"
		)
	elseif(UNIX)
		set_target_properties(dicomsdl_pixel_htj2k_plugin PROPERTIES
			BUILD_RPATH "\$ORIGIN"
			INSTALL_RPATH "\$ORIGIN"
		)
	endif()
endif()

if(DICOMSDL_PIXEL_JPEGXL_PLUGIN)
	if(NOT TARGET jxl OR NOT TARGET jxl_threads OR NOT TARGET jxl_cms)
		message(FATAL_ERROR "DICOMSDL_PIXEL_JPEGXL_PLUGIN requires libjxl targets (jxl/jxl_threads/jxl_cms).")
	endif()
	add_library(dicomsdl_pixel_jpegxl_plugin SHARED
		src/pixel_/plugins/shared/jpegxl_v2/common.cpp
		src/pixel_/plugins/shared/jpegxl_v2/decode.cpp
		src/pixel_/plugins/shared/jpegxl_v2/encode.cpp
		src/pixel_/plugins/shared/jpegxl_v2/shared_entry.cpp
	)
	target_include_directories(dicomsdl_pixel_jpegxl_plugin
		PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/abi
	)
	target_link_libraries(dicomsdl_pixel_jpegxl_plugin PRIVATE jxl jxl_threads jxl_cms)
	target_compile_features(dicomsdl_pixel_jpegxl_plugin PRIVATE cxx_std_20)
	set_target_properties(dicomsdl_pixel_jpegxl_plugin PROPERTIES
		LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
		OUTPUT_NAME "dicomsdl_pixel_jpegxl_plugin"
	)
	if(APPLE)
		set_target_properties(dicomsdl_pixel_jpegxl_plugin PROPERTIES
			BUILD_RPATH "@loader_path"
			INSTALL_RPATH "@loader_path"
		)
	elseif(UNIX)
		set_target_properties(dicomsdl_pixel_jpegxl_plugin PROPERTIES
			BUILD_RPATH "\$ORIGIN"
			INSTALL_RPATH "\$ORIGIN"
		)
	endif()
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

				if(TARGET dicomsdl_pixel_core)
						add_executable(dicomsdl_pixel_core_uncompressed_smoke
							tests/pixel_core_uncompressed_smoke.cpp
						)
					target_include_directories(dicomsdl_pixel_core_uncompressed_smoke
					PRIVATE
						${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/abi
						${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/core
					)
					target_link_libraries(dicomsdl_pixel_core_uncompressed_smoke PRIVATE
						dicomsdl_pixel_core
					)
					target_compile_features(dicomsdl_pixel_core_uncompressed_smoke PRIVATE cxx_std_20)
					add_test(NAME pixel_core_uncompressed_smoke
						COMMAND dicomsdl_pixel_core_uncompressed_smoke)
					set_tests_properties(pixel_core_uncompressed_smoke PROPERTIES LABELS "dicomsdl")
				endif()

				if(TARGET dicomsdl_pixel_rle_plugin_static)
						add_executable(dicomsdl_pixel_rle_static_plugin_smoke
							tests/pixel_rle_static_plugin_smoke.cpp
						)
				target_include_directories(dicomsdl_pixel_rle_static_plugin_smoke
					PRIVATE
						${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/abi
						${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/plugins/static/rle_v2
					)
					target_link_libraries(dicomsdl_pixel_rle_static_plugin_smoke PRIVATE
						dicomsdl_pixel_rle_plugin_static
					)
					target_compile_features(dicomsdl_pixel_rle_static_plugin_smoke PRIVATE cxx_std_20)
					add_test(NAME pixel_rle_static_plugin_smoke
						COMMAND dicomsdl_pixel_rle_static_plugin_smoke)
					set_tests_properties(pixel_rle_static_plugin_smoke PROPERTIES LABELS "dicomsdl")
				endif()

			if(TARGET dicomsdl_pixel_jpeg_plugin)
					add_executable(dicomsdl_pixel_jpeg_plugin_smoke
						tests/pixel_jpeg_plugin_smoke.cpp
					)
			target_compile_features(dicomsdl_pixel_jpeg_plugin_smoke PRIVATE cxx_std_20)
			target_include_directories(dicomsdl_pixel_jpeg_plugin_smoke
				PRIVATE
					${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/abi
			)
			if(CMAKE_DL_LIBS)
				target_link_libraries(dicomsdl_pixel_jpeg_plugin_smoke PRIVATE ${CMAKE_DL_LIBS})
			endif()
			add_dependencies(dicomsdl_pixel_jpeg_plugin_smoke
				dicomsdl_pixel_jpeg_plugin
			)
			add_test(NAME pixel_jpeg_plugin_smoke
				COMMAND dicomsdl_pixel_jpeg_plugin_smoke
				$<TARGET_FILE:dicomsdl_pixel_jpeg_plugin>)
			set_tests_properties(pixel_jpeg_plugin_smoke PROPERTIES LABELS "dicomsdl")
		endif()

			if(TARGET dicomsdl_pixel_htj2k_plugin)
					add_executable(dicomsdl_pixel_htj2k_plugin_smoke
						tests/pixel_htj2k_plugin_smoke.cpp
					)
		target_compile_features(dicomsdl_pixel_htj2k_plugin_smoke PRIVATE cxx_std_20)
		target_include_directories(dicomsdl_pixel_htj2k_plugin_smoke
			PRIVATE
				${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/abi
		)
		if(CMAKE_DL_LIBS)
			target_link_libraries(dicomsdl_pixel_htj2k_plugin_smoke PRIVATE ${CMAKE_DL_LIBS})
		endif()
		add_dependencies(dicomsdl_pixel_htj2k_plugin_smoke
			dicomsdl_pixel_htj2k_plugin
		)
		add_test(NAME pixel_htj2k_plugin_smoke
			COMMAND dicomsdl_pixel_htj2k_plugin_smoke
			$<TARGET_FILE:dicomsdl_pixel_htj2k_plugin>)
		set_tests_properties(pixel_htj2k_plugin_smoke PROPERTIES LABELS "dicomsdl")
	endif()

			if(TARGET dicomsdl_pixel_runtime AND TARGET dicomsdl_pixel_jpeg_plugin)
					add_executable(dicomsdl_pixel_runtime_registry_bootstrap_smoke
						tests/pixel_runtime_registry_bootstrap_smoke.cpp
					)
			target_link_libraries(dicomsdl_pixel_runtime_registry_bootstrap_smoke PRIVATE
				dicomsdl_pixel_runtime
			)
			target_compile_features(dicomsdl_pixel_runtime_registry_bootstrap_smoke PRIVATE cxx_std_20)
			add_dependencies(dicomsdl_pixel_runtime_registry_bootstrap_smoke
				dicomsdl_pixel_jpeg_plugin
			)
				add_test(NAME pixel_runtime_registry_bootstrap_smoke
					COMMAND dicomsdl_pixel_runtime_registry_bootstrap_smoke
					$<TARGET_FILE:dicomsdl_pixel_jpeg_plugin>)
				set_tests_properties(pixel_runtime_registry_bootstrap_smoke PROPERTIES LABELS "dicomsdl")
			endif()

			if(TARGET dicomsdl_pixel_runtime)
					add_executable(dicomsdl_pixel_host_adapter_smoke
						tests/pixel_host_adapter_smoke.cpp
					)
				target_include_directories(dicomsdl_pixel_host_adapter_smoke
					PRIVATE
						${CMAKE_CURRENT_SOURCE_DIR}/include
						${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/abi
						${CMAKE_CURRENT_SOURCE_DIR}/src/pixel_/runtime
						${CMAKE_CURRENT_BINARY_DIR}/generated/include
					)
					target_link_libraries(dicomsdl_pixel_host_adapter_smoke PRIVATE
						dicomsdl_pixel_runtime
					)
				target_compile_features(dicomsdl_pixel_host_adapter_smoke PRIVATE cxx_std_20)
				add_test(NAME pixel_host_adapter_smoke
					COMMAND dicomsdl_pixel_host_adapter_smoke)
				set_tests_properties(pixel_host_adapter_smoke PROPERTIES LABELS "dicomsdl")
			endif()

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
		dicomsdl_pixel_core_uncompressed_smoke
		dicomsdl_pixel_rle_static_plugin_smoke
		dicomsdl_pixel_jpeg_plugin_smoke
		dicomsdl_pixel_htj2k_plugin_smoke
		dicomsdl_pixel_runtime_registry_bootstrap_smoke
		dicomsdl_pixel_host_adapter_smoke
	)
		if(TARGET ${_dicomsdl_test_target})
			dicomsdl_apply_codec_builtin_defines(${_dicomsdl_test_target})
		endif()
	endforeach()
	unset(_dicomsdl_test_target)
endif()
