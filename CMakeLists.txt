cmake_minimum_required(VERSION 3.16)

project(DicomProject VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    add_compile_options(
        $<$<CONFIG:Release>:/Ox;/Ob2;/Oi;/Ot;/GL;/DNDEBUG>
        $<$<CONFIG:RelWithDebInfo>:/Ox;/Ob2;/Oi;/Ot;/GL;/DNDEBUG>
    )
    add_link_options(
        $<$<CONFIG:Release>:/LTCG>
        $<$<CONFIG:RelWithDebInfo>:/LTCG>
    )
else()
    add_compile_options(
        $<$<CONFIG:Release>:-O3;-ffast-math;-fstrict-aliasing;-funroll-loops>
        $<$<CONFIG:RelWithDebInfo>:-O3;-ffast-math;-fstrict-aliasing;-funroll-loops>
    )
    add_link_options(
        $<$<CONFIG:Release>:-O3>
        $<$<CONFIG:RelWithDebInfo>:-O3>
    )
endif()

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

option(DICOM_BUILD_BENCHMARKS "Build keyword lookup benchmarks" ON)

if(DICOM_BUILD_BENCHMARKS)
	add_executable(keyword_lookup_bench
		examples/keyword_lookup_bench.cpp
	)
	target_link_libraries(keyword_lookup_bench PRIVATE dicomsdl)
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
