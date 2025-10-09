cmake_minimum_required(VERSION 3.16)

project(DicomProject VERSION 0.1.0 LANGUAGES CXX)

set(DICOM_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(dicomsdl STATIC
	src/dicomfile.cpp
)
set_target_properties(dicomsdl PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(dicomsdl
	PUBLIC
		${DICOM_INCLUDE_DIR}
)
target_compile_features(dicomsdl PUBLIC cxx_std_20)

option(DICOM_BUILD_EXAMPLES "Build example executables for DicomProject" ON)

if(DICOM_BUILD_EXAMPLES)
	add_executable(dicom_example
		examples/dicom_example.cpp
	)
	target_link_libraries(dicom_example PRIVATE dicomsdl)
endif()

option(DICOM_BUILD_PYTHON "Build Python bindings with pybind11" ON)

if(DICOM_BUILD_PYTHON)
	set(PYBIND11_DIR ${CMAKE_CURRENT_SOURCE_DIR}/extern/pybind11)
	add_subdirectory(${PYBIND11_DIR} EXCLUDE_FROM_ALL)
	pybind11_add_module(_dicomsdl MODULE
		bindings/python/dicom_module.cpp
	)
	target_link_libraries(_dicomsdl PRIVATE dicomsdl)
endif()
