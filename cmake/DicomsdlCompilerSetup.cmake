# Compiler defaults shared by the dicomsdl top-level build.

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

unset(_dicom_release_flags)
