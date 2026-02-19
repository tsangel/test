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

if(MSVC)
    set(_dicom_release_flags /Ox /Ob2 /Oi /Ot /GL /DNDEBUG)
    foreach(flag IN LISTS _dicom_release_flags)
        add_compile_options($<$<CONFIG:Release>:${flag}>)
        add_compile_options($<$<CONFIG:RelWithDebInfo>:${flag}>)
    endforeach()
    add_link_options(
        $<$<CONFIG:Release>:/LTCG>
        $<$<CONFIG:RelWithDebInfo>:/LTCG>
    )
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

add_library(dicomsdl STATIC
	src/dataset.cpp
	src/sequence.cpp
	src/dataelement.cpp
	src/instream.cpp
	src/pixelsequence.cpp
	src/pixel_decoder.cpp
	src/pixel_decoder_raw.cpp
	src/pixel_decoder_rle.cpp
	src/pixel_decoder_jpeg2k.cpp
)
set_target_properties(dicomsdl PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(dicomsdl PROPERTIES
	ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
)
target_include_directories(dicomsdl
	PUBLIC
		${DICOM_INCLUDE_DIR}
		${GENERATED_INCLUDE_DIR}
)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/extern/fmt EXCLUDE_FROM_ALL)
target_link_libraries(dicomsdl PUBLIC fmt::fmt-header-only)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/openjpeg/CMakeLists.txt")
	message(FATAL_ERROR
		"openjpeg not found at ${CMAKE_CURRENT_SOURCE_DIR}/extern/openjpeg. "
		"Initialize submodules (git submodule update --init --recursive).")
endif()

set(_dicomsdl_build_testing "${BUILD_TESTING}")
# Keep OpenJPEG tests disabled in this superbuild. Otherwise CTest registers
# hundreds of OpenJPEG cases while EXCLUDE_FROM_ALL prevents their binaries
# from being built, which causes mass "Not Run" failures.
set(BUILD_TESTING OFF)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/extern/openjpeg EXCLUDE_FROM_ALL)
set(BUILD_TESTING "${_dicomsdl_build_testing}")
unset(_dicomsdl_build_testing)

unset(EXECUTABLE_OUTPUT_PATH)
unset(EXECUTABLE_OUTPUT_PATH CACHE)
unset(LIBRARY_OUTPUT_PATH)
unset(LIBRARY_OUTPUT_PATH CACHE)

target_include_directories(dicomsdl PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/extern/openjpeg/src/lib/openjp2
	${CMAKE_CURRENT_BINARY_DIR}/extern/openjpeg/src/lib/openjp2
)
if(TARGET openjp2_static)
	target_link_libraries(dicomsdl PRIVATE openjp2_static)
else()
	target_link_libraries(dicomsdl PRIVATE openjp2)
endif()
option(DICOMSDL_ENABLE_INFO_LOGS "Compile in INFO-level logging (otherwise info logs are compiled out)" OFF)
if(DICOMSDL_ENABLE_INFO_LOGS)
	target_compile_definitions(dicomsdl PUBLIC DICOMSDL_ENABLE_INFO_LOGS=1)
else()
	target_compile_definitions(dicomsdl PUBLIC DICOMSDL_ENABLE_INFO_LOGS=0)
endif()
target_compile_features(dicomsdl PUBLIC cxx_std_20)

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
endif()

option(DICOM_BUILD_PYTHON "Build Python bindings with nanobind" ON)
option(DICOM_PYTHON_STABLE_ABI "Build Python bindings with nanobind STABLE_ABI (abi3, CPython>=3.12)" OFF)

if(DICOM_BUILD_PYTHON)
	set(NANOBIND_DIR ${CMAKE_CURRENT_SOURCE_DIR}/extern/nanobind)
	if(NOT EXISTS "${NANOBIND_DIR}/CMakeLists.txt")
		message(FATAL_ERROR
			"nanobind not found at ${NANOBIND_DIR}. "
			"Initialize submodules (git submodule update --init --recursive).")
	endif()
	find_package(Python REQUIRED COMPONENTS Interpreter Development.Module OPTIONAL_COMPONENTS Development.SABIModule)
	add_subdirectory(${NANOBIND_DIR} EXCLUDE_FROM_ALL)
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
endif()
