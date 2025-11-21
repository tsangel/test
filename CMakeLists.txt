cmake_minimum_required(VERSION 3.16)

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

project(DicomProject VERSION ${DICOMPROJECT_VERSION} LANGUAGES CXX)

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
)
set_target_properties(dicomsdl PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(dicomsdl
	PUBLIC
		${DICOM_INCLUDE_DIR}
		${GENERATED_INCLUDE_DIR}
)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/extern/fmt EXCLUDE_FROM_ALL)
target_link_libraries(dicomsdl PUBLIC fmt::fmt-header-only)
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
endif()

option(DICOM_BUILD_PYTHON "Build Python bindings with pybind11" ON)

if(DICOM_BUILD_PYTHON)
	set(PYBIND11_DIR ${CMAKE_CURRENT_SOURCE_DIR}/extern/pybind11)
	add_subdirectory(${PYBIND11_DIR} EXCLUDE_FROM_ALL)
	pybind11_add_module(_dicomsdl MODULE
		bindings/python/dicom_module.cpp
	)
	target_link_libraries(_dicomsdl PRIVATE dicomsdl fmt::fmt-header-only)
endif()

if(BUILD_TESTING)
	add_executable(dicomsdl_dictionary_consistency
		tests/dictionary_consistency.cpp
	)
	target_link_libraries(dicomsdl_dictionary_consistency PRIVATE dicomsdl)
	add_test(NAME dictionary_consistency COMMAND dicomsdl_dictionary_consistency)

	add_executable(dicomsdl_basic_smoke
		tests/basic_smoke.cpp
	)
	target_link_libraries(dicomsdl_basic_smoke PRIVATE dicomsdl)
	add_test(NAME basic_smoke COMMAND dicomsdl_basic_smoke)

	add_executable(dicomsdl_numeric_values
		tests/numeric_values.cpp
	)
	target_link_libraries(dicomsdl_numeric_values PRIVATE dicomsdl)
	add_test(NAME numeric_values COMMAND dicomsdl_numeric_values)

	add_executable(dicomsdl_uid_consistency
		tests/uid_consistency.cpp
	)
	target_link_libraries(dicomsdl_uid_consistency PRIVATE dicomsdl)
	add_test(NAME uid_consistency COMMAND dicomsdl_uid_consistency)
endif()
