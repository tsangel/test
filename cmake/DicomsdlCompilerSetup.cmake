# Compiler defaults shared by the dicomsdl top-level build.

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(
    DICOMSDL_MSVC_ENABLE_LTCG
    "Enable MSVC /GL and /LTCG in Release and RelWithDebInfo (MSVC cl.exe only)"
    ON
)
set(
    DICOMSDL_MSVC_PGO
    "OFF"
    CACHE STRING
    "MSVC PGO mode for Release and RelWithDebInfo (OFF, GEN, USE; MSVC cl.exe only)"
)
set_property(CACHE DICOMSDL_MSVC_PGO PROPERTY STRINGS OFF GEN USE)
set(
    DICOMSDL_MSVC_PGO_DIR
    "${CMAKE_BINARY_DIR}/pgo/msvc"
    CACHE PATH
    "Directory used for MSVC PGO profile data (.pgd/.pgc)"
)

if(MSVC AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(_dicomsdl_msvc_pgo_modes OFF GEN USE)
    string(TOUPPER "${DICOMSDL_MSVC_PGO}" _dicomsdl_msvc_pgo_mode)
    if(NOT _dicomsdl_msvc_pgo_mode IN_LIST _dicomsdl_msvc_pgo_modes)
        message(FATAL_ERROR
            "DICOMSDL_MSVC_PGO must be one of OFF, GEN, USE (got: ${DICOMSDL_MSVC_PGO})")
    endif()
    if(NOT DICOMSDL_MSVC_ENABLE_LTCG AND NOT _dicomsdl_msvc_pgo_mode STREQUAL "OFF")
        message(FATAL_ERROR
            "DICOMSDL_MSVC_PGO=${_dicomsdl_msvc_pgo_mode} requires DICOMSDL_MSVC_ENABLE_LTCG=ON")
    endif()

    set(_dicom_release_flags /Ox /Ob2 /Oi /Ot /DNDEBUG)
    set(_dicom_release_link_flags)
    if(DICOMSDL_MSVC_ENABLE_LTCG)
        list(APPEND _dicom_release_flags /GL)
        if(_dicomsdl_msvc_pgo_mode STREQUAL "OFF")
            list(APPEND _dicom_release_link_flags /LTCG)
        else()
            file(MAKE_DIRECTORY "${DICOMSDL_MSVC_PGO_DIR}")
            file(TO_CMAKE_PATH "${DICOMSDL_MSVC_PGO_DIR}" _dicomsdl_msvc_pgo_dir_norm)
            set(_dicomsdl_msvc_pgd "${_dicomsdl_msvc_pgo_dir_norm}/dicomsdl.pgd")
            list(APPEND _dicom_release_link_flags "/PGD:${_dicomsdl_msvc_pgd}")
            if(_dicomsdl_msvc_pgo_mode STREQUAL "GEN")
                list(APPEND _dicom_release_link_flags /LTCG:PGINSTRUMENT /GENPROFILE)
            else()
                if(NOT EXISTS "${_dicomsdl_msvc_pgd}")
                    message(FATAL_ERROR
                        "DICOMSDL_MSVC_PGO=USE requires ${_dicomsdl_msvc_pgd}. "
                        "Run a GEN build and execute representative workloads first.")
                endif()
                list(APPEND _dicom_release_link_flags /LTCG:PGOPTIMIZE /USEPROFILE)
            endif()
        endif()
    endif()
    foreach(flag IN LISTS _dicom_release_flags)
        add_compile_options($<$<CONFIG:Release>:${flag}>)
        add_compile_options($<$<CONFIG:RelWithDebInfo>:${flag}>)
    endforeach()
    foreach(flag IN LISTS _dicom_release_link_flags)
        add_link_options($<$<CONFIG:Release>:${flag}>)
        add_link_options($<$<CONFIG:RelWithDebInfo>:${flag}>)
    endforeach()
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

unset(_dicom_release_flags)
unset(_dicom_release_link_flags)
unset(_dicomsdl_msvc_pgo_modes)
unset(_dicomsdl_msvc_pgo_mode)
unset(_dicomsdl_msvc_pgo_dir_norm)
unset(_dicomsdl_msvc_pgd)
