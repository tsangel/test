cmake_minimum_required(VERSION 3.16)

if(APPLE)
    set(_dicomsdl_macos_min_target "10.13")
    set(_dicomsdl_macos_architectures "")

    if(DEFINED CMAKE_OSX_ARCHITECTURES AND NOT CMAKE_OSX_ARCHITECTURES STREQUAL "")
        set(_dicomsdl_macos_architectures "${CMAKE_OSX_ARCHITECTURES}")
    elseif(DEFINED ENV{CMAKE_OSX_ARCHITECTURES} AND NOT "$ENV{CMAKE_OSX_ARCHITECTURES}" STREQUAL "")
        set(_dicomsdl_macos_architectures "$ENV{CMAKE_OSX_ARCHITECTURES}")
    elseif(CMAKE_HOST_SYSTEM_PROCESSOR)
        set(_dicomsdl_macos_architectures "${CMAKE_HOST_SYSTEM_PROCESSOR}")
    endif()

    string(REPLACE ";" " " _dicomsdl_macos_architecture_text "${_dicomsdl_macos_architectures}")
    string(TOLOWER "${_dicomsdl_macos_architecture_text}" _dicomsdl_macos_architecture_text)
    if(_dicomsdl_macos_architecture_text MATCHES "(^|[ ,])arm64($|[ ,])" OR
       _dicomsdl_macos_architecture_text MATCHES "(^|[ ,])aarch64($|[ ,])")
        set(_dicomsdl_macos_min_target "11.0")
    endif()

    set(_dicomsdl_requested_macos_target "")
    if(DEFINED CMAKE_OSX_DEPLOYMENT_TARGET AND NOT CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL "")
        set(_dicomsdl_requested_macos_target "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    elseif(DEFINED ENV{MACOSX_DEPLOYMENT_TARGET} AND NOT "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
        set(_dicomsdl_requested_macos_target "$ENV{MACOSX_DEPLOYMENT_TARGET}")
    endif()

    if(_dicomsdl_requested_macos_target STREQUAL "" OR
       _dicomsdl_requested_macos_target VERSION_LESS _dicomsdl_macos_min_target)
        set(_dicomsdl_effective_macos_target "${_dicomsdl_macos_min_target}")
    else()
        set(_dicomsdl_effective_macos_target "${_dicomsdl_requested_macos_target}")
    endif()

    set(CMAKE_OSX_DEPLOYMENT_TARGET "${_dicomsdl_effective_macos_target}" CACHE STRING
        "Minimum macOS deployment target" FORCE)

    unset(_dicomsdl_effective_macos_target)
    unset(_dicomsdl_requested_macos_target)
    unset(_dicomsdl_macos_architecture_text)
    unset(_dicomsdl_macos_architectures)
    unset(_dicomsdl_macos_min_target)
endif()

set(_version_header_file "${CMAKE_CURRENT_SOURCE_DIR}/include/dicom_const.h")
set(_version_parser_script "${CMAKE_CURRENT_SOURCE_DIR}/scripts/extract_version_from_const_header.py")

set(_default_temp_version_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/version")
set(DICOMSDL_TEMP_VERSION_FILE "${_default_temp_version_dir}/VERSION" CACHE FILEPATH
    "Path to generated temporary DICOMSDL VERSION file")
set(DICOM_STANDARD_TEMP_VERSION_FILE "${_default_temp_version_dir}/DICOM_STANDARD_VERSION" CACHE FILEPATH
    "Path to generated temporary DICOM standard version file")

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_version_header_file}"
    "${_version_parser_script}"
)

find_program(_dicomsdl_python_executable NAMES python3 python REQUIRED)

get_filename_component(_dicomsdl_temp_version_dir "${DICOMSDL_TEMP_VERSION_FILE}" DIRECTORY)
if(_dicomsdl_temp_version_dir)
    file(MAKE_DIRECTORY "${_dicomsdl_temp_version_dir}")
endif()
get_filename_component(_dicom_standard_temp_version_dir "${DICOM_STANDARD_TEMP_VERSION_FILE}" DIRECTORY)
if(_dicom_standard_temp_version_dir)
    file(MAKE_DIRECTORY "${_dicom_standard_temp_version_dir}")
endif()

execute_process(
    COMMAND
        "${_dicomsdl_python_executable}" "${_version_parser_script}"
        --header "${_version_header_file}"
        --version-out "${DICOMSDL_TEMP_VERSION_FILE}"
        --dicom-standard-out "${DICOM_STANDARD_TEMP_VERSION_FILE}"
    RESULT_VARIABLE _version_extract_result
    OUTPUT_VARIABLE _version_extract_stdout
    ERROR_VARIABLE _version_extract_stderr
)
if(NOT _version_extract_result EQUAL 0)
    message(FATAL_ERROR
        "Failed to extract version values from include/dicom_const.h.\n"
        "stdout:\n${_version_extract_stdout}\n"
        "stderr:\n${_version_extract_stderr}")
endif()

file(READ "${DICOMSDL_TEMP_VERSION_FILE}" _dicomsdl_version_raw)
string(STRIP "${_dicomsdl_version_raw}" DICOMPROJECT_VERSION)
set(DICOMSDL_VERSION "${DICOMPROJECT_VERSION}")

file(READ "${DICOM_STANDARD_TEMP_VERSION_FILE}" _dicom_standard_version_raw)
string(STRIP "${_dicom_standard_version_raw}" DICOM_STANDARD_VERSION)

set(DICOM_VERSION_SOURCE "${DICOM_STANDARD_TEMP_VERSION_FILE}")
set(DICOMSDL_VERSION_SOURCE "${DICOMSDL_TEMP_VERSION_FILE}")

project(DicomProject VERSION ${DICOMPROJECT_VERSION} LANGUAGES C CXX)

set(DICOM_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

include(CTest)
include(ExternalProject)
include(FetchContent)
# Ensure third-party option() calls honor normal variables we set in helper
# functions (without cache push/pop compatibility shims).
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(DicomsdlCompilerSetup)
include(DicomsdlDependencies)

add_library(dicomsdl STATIC
    src/dataset.cpp
    src/diagnostics.cpp
    src/dataset_deflate_codec.cpp
    src/dataset_endian_converter.cpp
    src/charset/charset_mutation.cpp
    src/charset/charset_transcode.cpp
    src/charset/charset_sbcs.cpp
    src/charset/charset_gb.cpp
    src/charset/charset_iso2022.cpp
    src/charset/charset_iso2022_decode.cpp
    src/charset/charset_codec.cpp
    src/dumpdataset.cpp
    src/dicomfile.cpp
    src/dicomfile_pixel_encode.cpp
    src/dicom_write.cpp
    src/person_name.cpp
    src/uid_utils.cpp
    src/sequence.cpp
    src/dataelement.cpp
    src/instream.cpp
    src/pixelsequence.cpp
    src/pixel/host/error/codec_error.cpp
    src/pixel/host/entrypoint/runtime_entrypoint.cpp
    src/pixel/host/entrypoint/decode_entrypoint.cpp
    src/pixel/host/decode/decode_modality_value_transform.cpp
    src/pixel/host/decode/decode_frame_dispatch.cpp
    src/pixel/host/support/dicom_pixel_support.cpp
    src/pixel/host/entrypoint/encode_entrypoint.cpp
    src/pixel/host/encode/encode_options_policy.cpp
    src/pixel/host/encode/encode_context_validation.cpp
    src/pixel/host/encode/encode_set_pixel_data_runner.cpp
    src/pixel/host/encode/encode_target_policy.cpp
    src/pixel/host/encode/multicomponent_transform_policy.cpp
    src/pixel/host/encode/encode_metadata_updater.cpp
)
set_target_properties(dicomsdl PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)

target_include_directories(dicomsdl
    PUBLIC
        ${DICOM_INCLUDE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/abi
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

option(DICOMSDL_ENABLE_OPENJPH "Enable OpenJPH backend for HTJ2K decoding" ON)
option(DICOMSDL_ENABLE_JPEGXL "Enable JPEG XL backend dependencies (libjxl) for codec development" OFF)

option(DICOMSDL_PIXEL_HTJ2K_PLUGIN "Build src/pixel HTJ2K shared plugin target." OFF)
option(DICOMSDL_PIXEL_JPEGLS_PLUGIN "Build src/pixel JPEG-LS shared plugin target." OFF)
option(DICOMSDL_PIXEL_OPENJPEG_PLUGIN "Build src/pixel OpenJPEG shared plugin target." OFF)
option(DICOMSDL_PIXEL_OPENJPEG_STATIC_PLUGIN "Build src/pixel OpenJPEG static plugin target." ON)
option(DICOMSDL_PIXEL_JPEGXL_PLUGIN "Build src/pixel JPEG-XL shared plugin target." OFF)
option(DICOMSDL_PIXEL_JPEGXL_STATIC_PLUGIN "Build src/pixel JPEG-XL static plugin target." ${DICOMSDL_ENABLE_JPEGXL})
option(DICOMSDL_PIXEL_JPEG_PLUGIN "Build src/pixel JPEG shared plugin target." OFF)
option(DICOMSDL_PIXEL_JPEG_STATIC_PLUGIN "Build src/pixel JPEG static plugin target." ON)
option(DICOMSDL_PIXEL_JPEGLS_STATIC_PLUGIN "Build src/pixel JPEG-LS static plugin target." ON)
option(DICOMSDL_PIXEL_HTJ2K_STATIC_PLUGIN "Build src/pixel HTJ2K static plugin target." ${DICOMSDL_ENABLE_OPENJPH})
option(DICOMSDL_PIXEL_CORE "Build src/pixel core uncompressed helpers." ON)
option(DICOMSDL_PIXEL_RLE_STATIC_PLUGIN "Build src/pixel static RLE plugin target." ON)
option(DICOMSDL_PIXEL_RUNTIME "Build src/pixel runtime registry target." ON)

# Dependency graph setup.
dicomsdl_fetch_fmt()
dicomsdl_fetch_openjpeg()
dicomsdl_fetch_charls()
dicomsdl_fetch_libdeflate()
dicomsdl_fetch_openjph()
dicomsdl_fetch_libjxl()
dicomsdl_prepare_libturbojpeg()

add_dependencies(dicomsdl dicomsdl_libturbojpeg)

target_link_libraries(dicomsdl PUBLIC fmt::fmt-header-only)

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

include(DicomsdlPixelTargets)
dicomsdl_apply_pixel_static_plugin_defines(dicomsdl)

include(DicomsdlBenchmarks)
include(DicomsdlExamples)
include(DicomsdlPython)
include(DicomsdlTests)
