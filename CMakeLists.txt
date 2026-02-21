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
include(ExternalProject)

add_library(dicomsdl STATIC
	src/dataset.cpp
	src/dumpdataset.cpp
	src/dicomfile.cpp
	src/sequence.cpp
	src/dataelement.cpp
	src/instream.cpp
	src/pixelsequence.cpp
	src/pixel_decoder.cpp
	src/pixel_decoder_raw.cpp
	src/pixel_decoder_rle.cpp
	src/pixel_decoder_jpeg2k.cpp
	src/pixel_decoder_htj2k.cpp
	src/pixel_decoder_jpegls.cpp
	src/pixel_decoder_jpeg.cpp
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
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/charls/CMakeLists.txt")
	message(FATAL_ERROR
		"charls not found at ${CMAKE_CURRENT_SOURCE_DIR}/extern/charls. "
		"Initialize submodules (git submodule update --init --recursive).")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/libjpeg-turbo/CMakeLists.txt")
	message(FATAL_ERROR
		"libjpeg-turbo not found at ${CMAKE_CURRENT_SOURCE_DIR}/extern/libjpeg-turbo. "
		"Initialize submodules (git submodule update --init --recursive).")
endif()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/libdeflate/CMakeLists.txt")
	message(FATAL_ERROR
		"libdeflate not found at ${CMAKE_CURRENT_SOURCE_DIR}/extern/libdeflate. "
		"Initialize submodules (git submodule update --init --recursive).")
endif()

option(DICOMSDL_ENABLE_OPENJPH "Enable OpenJPH backend for HTJ2K decoding" ON)
if(DICOMSDL_ENABLE_OPENJPH)
	if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/openjph/CMakeLists.txt")
		message(FATAL_ERROR
			"openjph not found at ${CMAKE_CURRENT_SOURCE_DIR}/extern/openjph. "
			"Initialize submodules (git submodule update --init --recursive).")
	endif()
endif()

# Keep CharLS pinned while upstream regression remains unresolved.
# The known-good submodule revision corresponds to tag 2.2.1.
option(DICOMSDL_ALLOW_UNPINNED_CHARLS
	"Allow building with a CharLS revision other than the pinned known-good commit." OFF)
set(DICOMSDL_CHARLS_PINNED_SHA "0ae38a5e106a788d6895b0d2db1cfcc845c5759c")
if(NOT DICOMSDL_ALLOW_UNPINNED_CHARLS)
	find_package(Git QUIET)
	if(Git_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/extern/charls/.git")
		execute_process(
			COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}/extern/charls" rev-parse HEAD
			OUTPUT_VARIABLE _dicomsdl_charls_head
			OUTPUT_STRIP_TRAILING_WHITESPACE
			ERROR_QUIET
		)
		if(NOT _dicomsdl_charls_head STREQUAL DICOMSDL_CHARLS_PINNED_SHA)
			message(FATAL_ERROR
				"extern/charls is not pinned to known-good revision ${DICOMSDL_CHARLS_PINNED_SHA} (tag 2.2.1).\n"
				"Current revision: ${_dicomsdl_charls_head}\n"
				"This pin is temporary due to a JPEG-LS regression with WG04 MR1 samples in newer CharLS tags.\n"
				"If you intentionally want to build with another revision, set -DDICOMSDL_ALLOW_UNPINNED_CHARLS=ON.")
		endif()
	else()
		message(WARNING
			"Could not verify CharLS pinned revision (git metadata unavailable). "
			"Expected known-good tag: 2.2.1. "
			"Set -DDICOMSDL_ALLOW_UNPINNED_CHARLS=ON to silence this warning.")
	endif()
endif()

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
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/extern/openjpeg EXCLUDE_FROM_ALL)
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
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/extern/charls EXCLUDE_FROM_ALL)

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
set(LIBDEFLATE_COMPRESSION_SUPPORT OFF CACHE BOOL "Disable libdeflate compression codepaths" FORCE)
set(LIBDEFLATE_DECOMPRESSION_SUPPORT ON CACHE BOOL "Enable libdeflate decompression codepaths" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/extern/libdeflate EXCLUDE_FROM_ALL)

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
	add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/extern/openjph EXCLUDE_FROM_ALL)

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
	SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/extern/libjpeg-turbo"
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
	${CMAKE_CURRENT_SOURCE_DIR}/extern/openjpeg/src/lib/openjp2
	${CMAKE_CURRENT_BINARY_DIR}/extern/openjpeg/src/lib/openjp2
	${CMAKE_CURRENT_SOURCE_DIR}/extern/libjpeg-turbo/src
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
endif()
