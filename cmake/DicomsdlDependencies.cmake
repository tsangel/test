# Third-party dependency setup for the dicomsdl superbuild.

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

set(DICOMSDL_YYJSON_GIT_REPOSITORY "https://github.com/ibireme/yyjson.git"
    CACHE STRING "yyjson upstream git repository used by FetchContent.")
# Release: yyjson 0.12.0 (GitHub latest release, checked 2026-04-05)
set(DICOMSDL_YYJSON_GIT_TAG "0.12.0"
    CACHE STRING "yyjson git ref used by FetchContent (use branch/tag/SHA).")

function(dicomsdl_fetch_fmt)
    FetchContent_Declare(dicomsdl_fmt
        GIT_REPOSITORY "${DICOMSDL_FMT_GIT_REPOSITORY}"
        GIT_TAG "${DICOMSDL_FMT_GIT_TAG}"
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(dicomsdl_fmt)
endfunction()

function(dicomsdl_fetch_yyjson)
    set(BUILD_SHARED_LIBS OFF)
    set(YYJSON_BUILD_TESTS OFF)
    set(YYJSON_BUILD_FUZZER OFF)
    set(YYJSON_BUILD_MISC OFF)
    set(YYJSON_BUILD_DOC OFF)
    set(YYJSON_INSTALL OFF)

    FetchContent_Declare(dicomsdl_yyjson
        GIT_REPOSITORY "${DICOMSDL_YYJSON_GIT_REPOSITORY}"
        GIT_TAG "${DICOMSDL_YYJSON_GIT_TAG}"
        GIT_PROGRESS TRUE
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(dicomsdl_yyjson)
endfunction()

function(dicomsdl_fetch_openjpeg)
    # Keep third-party tests/tools off without mutating top-level BUILD_TESTING.
    set(BUILD_TESTING OFF)
    set(BUILD_CODEC OFF)

    # Avoid runtime DLL dependency for wheel import on Windows.
    if(WIN32)
        set(BUILD_SHARED_LIBS OFF)
    endif()

    FetchContent_Declare(dicomsdl_openjpeg
        GIT_REPOSITORY "${DICOMSDL_OPENJPEG_GIT_REPOSITORY}"
        GIT_TAG "${DICOMSDL_OPENJPEG_GIT_TAG}"
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(dicomsdl_openjpeg)

    set(dicomsdl_openjpeg_SOURCE_DIR "${dicomsdl_openjpeg_SOURCE_DIR}" PARENT_SCOPE)
    set(dicomsdl_openjpeg_BINARY_DIR "${dicomsdl_openjpeg_BINARY_DIR}" PARENT_SCOPE)
endfunction()

function(dicomsdl_fetch_charls)
    set(BUILD_SHARED_LIBS OFF)

    FetchContent_Declare(dicomsdl_charls
        GIT_REPOSITORY "${DICOMSDL_CHARLS_GIT_REPOSITORY}"
        GIT_TAG "${DICOMSDL_CHARLS_GIT_TAG}"
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(dicomsdl_charls)
endfunction()

function(dicomsdl_fetch_libdeflate)
    set(LIBDEFLATE_BUILD_STATIC_LIB ON)
    set(LIBDEFLATE_BUILD_SHARED_LIB OFF)
    set(LIBDEFLATE_BUILD_GZIP OFF)
    set(LIBDEFLATE_BUILD_TESTS OFF)
    set(LIBDEFLATE_INSTALL OFF)
    set(LIBDEFLATE_COMPRESSION_SUPPORT ON)
    set(LIBDEFLATE_DECOMPRESSION_SUPPORT ON)

    FetchContent_Declare(dicomsdl_libdeflate
        GIT_REPOSITORY "${DICOMSDL_LIBDEFLATE_GIT_REPOSITORY}"
        GIT_TAG "${DICOMSDL_LIBDEFLATE_GIT_TAG}"
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(dicomsdl_libdeflate)
endfunction()

function(dicomsdl_fetch_openjph)
    if(NOT (DICOMSDL_ENABLE_OPENJPH OR DICOMSDL_PIXEL_HTJ2K_PLUGIN))
        return()
    endif()

    set(BUILD_SHARED_LIBS OFF)
    set(OJPH_ENABLE_TIFF_SUPPORT OFF)
    set(OJPH_BUILD_TESTS OFF)
    set(OJPH_BUILD_EXECUTABLES OFF)
    set(OJPH_BUILD_STREAM_EXPAND OFF)
    set(OJPH_BUILD_FUZZER OFF)

    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
        # Temporary workaround for clang-cl SIMD flag compatibility.
        set(OJPH_DISABLE_SSSE3 ON)
    endif()

    FetchContent_Declare(dicomsdl_openjph
        GIT_REPOSITORY "${DICOMSDL_OPENJPH_GIT_REPOSITORY}"
        GIT_TAG "${DICOMSDL_OPENJPH_GIT_TAG}"
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(dicomsdl_openjph)
endfunction()

function(dicomsdl_fetch_libjxl)
    if(NOT (DICOMSDL_ENABLE_JPEGXL OR DICOMSDL_PIXEL_JPEGXL_PLUGIN))
        return()
    endif()

    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_TESTING OFF)

    set(JPEGXL_ENABLE_FUZZERS OFF)
    set(JPEGXL_ENABLE_DEVTOOLS OFF)
    set(JPEGXL_ENABLE_TOOLS OFF)
    set(JPEGXL_ENABLE_BENCHMARK OFF)
    set(JPEGXL_ENABLE_EXAMPLES OFF)
    set(JPEGXL_ENABLE_DOXYGEN OFF)
    set(JPEGXL_ENABLE_MANPAGES OFF)
    set(JPEGXL_ENABLE_JNI OFF)
    set(JPEGXL_ENABLE_OPENEXR OFF)
    set(JPEGXL_ENABLE_VIEWERS OFF)
    set(JPEGXL_ENABLE_PLUGINS OFF)
    set(JPEGXL_ENABLE_SJPEG OFF)
    set(JPEGXL_ENABLE_JPEGLI OFF)
    set(JPEGXL_ENABLE_TCMALLOC OFF)
    set(JPEGXL_BUNDLE_LIBPNG OFF)
    set(JPEGXL_ENABLE_SKCMS ON)

    FetchContent_Declare(dicomsdl_libjxl
        GIT_REPOSITORY "${DICOMSDL_LIBJXL_GIT_REPOSITORY}"
        GIT_TAG "${DICOMSDL_LIBJXL_GIT_TAG}"
        GIT_PROGRESS TRUE
        GIT_SUBMODULES "third_party/highway;third_party/brotli;third_party/skcms"
        GIT_SUBMODULES_RECURSE FALSE
    )
    FetchContent_MakeAvailable(dicomsdl_libjxl)
endfunction()

function(dicomsdl_prepare_libturbojpeg)
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

    set(_dicomsdl_libturbojpeg_externalproject_args
        SOURCE_DIR "${dicomsdl_libjpeg_turbo_SOURCE_DIR}"
        BINARY_DIR "${_dicomsdl_libturbojpeg_build_dir}"
        PREFIX "${_dicomsdl_libturbojpeg_prefix}"
        CMAKE_ARGS ${_dicomsdl_libturbojpeg_cmake_args}
        BUILD_BYPRODUCTS "${_dicomsdl_libturbojpeg_archive}"
    )

    if(CMAKE_CONFIGURATION_TYPES)
        # Multi-config generators (Visual Studio, Ninja Multi-Config) default
        # to Debug for ExternalProject unless --config is passed explicitly.
        list(APPEND _dicomsdl_libturbojpeg_externalproject_args
            BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config $<CONFIG>
            INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config $<CONFIG>
        )
    endif()

    ExternalProject_Add(dicomsdl_libturbojpeg
        ${_dicomsdl_libturbojpeg_externalproject_args}
    )

    if(NOT TARGET dicomsdl_turbojpeg)
        add_library(dicomsdl_turbojpeg STATIC IMPORTED GLOBAL)
    endif()
    set_target_properties(dicomsdl_turbojpeg PROPERTIES
        IMPORTED_LOCATION "${_dicomsdl_libturbojpeg_archive}"
    )
    add_dependencies(dicomsdl_turbojpeg dicomsdl_libturbojpeg)

    set(dicomsdl_libjpeg_turbo_SOURCE_DIR "${dicomsdl_libjpeg_turbo_SOURCE_DIR}" PARENT_SCOPE)
    set(dicomsdl_libjpeg_turbo_BINARY_DIR "${dicomsdl_libjpeg_turbo_BINARY_DIR}" PARENT_SCOPE)
endfunction()
