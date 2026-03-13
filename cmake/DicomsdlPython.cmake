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

    target_include_directories(_dicomsdl PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(_dicomsdl PRIVATE dicomsdl fmt::fmt-header-only)
    dicomsdl_apply_pixel_static_plugin_defines(_dicomsdl)
    if(DICOMSDL_PIXEL_RUNTIME)
        target_compile_definitions(_dicomsdl PRIVATE DICOMSDL_PIXEL_RUNTIME_ENABLED=1)
    endif()
endif()
