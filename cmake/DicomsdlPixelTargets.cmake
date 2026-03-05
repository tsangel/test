# Pixel v2 static/shared plugin targets and runtime wiring.

function(dicomsdl_apply_pixel_static_plugin_defines TARGET_NAME)
    set(_dicomsdl_jpeg_builtin 0)
    set(_dicomsdl_jpegls_builtin 0)
    set(_dicomsdl_jpeg2k_builtin 0)
    set(_dicomsdl_htj2k_builtin 0)
    set(_dicomsdl_jpegxl_builtin 0)

    if(DICOMSDL_PIXEL_RUNTIME AND DICOMSDL_PIXEL_JPEG_STATIC_PLUGIN)
        set(_dicomsdl_jpeg_builtin 1)
    endif()
    if(DICOMSDL_PIXEL_RUNTIME AND DICOMSDL_PIXEL_JPEGLS_STATIC_PLUGIN)
        set(_dicomsdl_jpegls_builtin 1)
    endif()
    if(DICOMSDL_PIXEL_RUNTIME AND DICOMSDL_PIXEL_OPENJPEG_STATIC_PLUGIN)
        set(_dicomsdl_jpeg2k_builtin 1)
    endif()
    if(DICOMSDL_PIXEL_RUNTIME AND DICOMSDL_PIXEL_HTJ2K_STATIC_PLUGIN)
        set(_dicomsdl_htj2k_builtin 1)
    endif()
    if(DICOMSDL_PIXEL_RUNTIME AND DICOMSDL_PIXEL_JPEGXL_STATIC_PLUGIN)
        set(_dicomsdl_jpegxl_builtin 1)
    endif()

    target_compile_definitions(${TARGET_NAME} PRIVATE
        DICOMSDL_PIXEL_JPEG_STATIC_PLUGIN_ENABLED=${_dicomsdl_jpeg_builtin}
        DICOMSDL_PIXEL_JPEGLS_STATIC_PLUGIN_ENABLED=${_dicomsdl_jpegls_builtin}
        DICOMSDL_PIXEL_OPENJPEG_STATIC_PLUGIN_ENABLED=${_dicomsdl_jpeg2k_builtin}
        DICOMSDL_PIXEL_HTJ2K_STATIC_PLUGIN_ENABLED=${_dicomsdl_htj2k_builtin}
        DICOMSDL_PIXEL_JPEGXL_STATIC_PLUGIN_ENABLED=${_dicomsdl_jpegxl_builtin}
    )

    if(DICOMSDL_ENABLE_OPENJPH)
        target_compile_definitions(${TARGET_NAME} PRIVATE DICOMSDL_HAS_OPENJPH=1)
    else()
        target_compile_definitions(${TARGET_NAME} PRIVATE DICOMSDL_HAS_OPENJPH=0)
    endif()

    if(DICOMSDL_ENABLE_JPEGXL)
        target_compile_definitions(${TARGET_NAME} PRIVATE DICOMSDL_HAS_JPEGXL=1)
    else()
        target_compile_definitions(${TARGET_NAME} PRIVATE DICOMSDL_HAS_JPEGXL=0)
    endif()
endfunction()

function(dicomsdl_set_target_output_dirs TARGET_NAME)
    set_target_properties(${TARGET_NAME} PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
endfunction()

function(dicomsdl_set_shared_plugin_rpath TARGET_NAME)
    if(APPLE)
        set_target_properties(${TARGET_NAME} PROPERTIES
            BUILD_RPATH "@loader_path"
            INSTALL_RPATH "@loader_path"
        )
    elseif(UNIX)
        set_target_properties(${TARGET_NAME} PROPERTIES
            BUILD_RPATH "\$ORIGIN"
            INSTALL_RPATH "\$ORIGIN"
        )
    endif()
endfunction()

if(DICOMSDL_PIXEL_CORE)
    add_library(dicomsdl_pixel_core STATIC
        src/pixel/core/common.cpp
        src/pixel/core/decode_uncompressed.cpp
        src/pixel/core/encode_uncompressed.cpp
    )
    target_include_directories(dicomsdl_pixel_core
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/abi
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/core
    )
    target_compile_features(dicomsdl_pixel_core PRIVATE cxx_std_20)
    dicomsdl_set_target_output_dirs(dicomsdl_pixel_core)
    set_target_properties(dicomsdl_pixel_core PROPERTIES
        OUTPUT_NAME "dicomsdl_pixel_core"
    )
endif()

if(DICOMSDL_PIXEL_RLE_STATIC_PLUGIN)
    add_library(dicomsdl_pixel_rle_plugin_static STATIC
        src/pixel/plugins/static/rle_v2/common.cpp
        src/pixel/plugins/static/rle_v2/decode.cpp
        src/pixel/plugins/static/rle_v2/encode.cpp
        src/pixel/plugins/static/rle_v2/entry.cpp
    )
    target_include_directories(dicomsdl_pixel_rle_plugin_static
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/abi
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/plugins/static/rle_v2
    )
    target_compile_features(dicomsdl_pixel_rle_plugin_static PRIVATE cxx_std_20)
    dicomsdl_set_target_output_dirs(dicomsdl_pixel_rle_plugin_static)
    set_target_properties(dicomsdl_pixel_rle_plugin_static PROPERTIES
        OUTPUT_NAME "dicomsdl_pixel_rle_plugin_static"
    )
endif()

if(DICOMSDL_PIXEL_OPENJPEG_STATIC_PLUGIN)
    add_library(dicomsdl_pixel_openjpeg_plugin_static STATIC
        src/pixel/plugins/shared/openjpeg_v2/error.cpp
        src/pixel/plugins/shared/openjpeg_v2/value_utils.cpp
        src/pixel/plugins/shared/openjpeg_v2/openjpeg_helpers.cpp
        src/pixel/plugins/shared/openjpeg_v2/context_api.cpp
        src/pixel/plugins/shared/openjpeg_v2/decode.cpp
        src/pixel/plugins/shared/openjpeg_v2/encode.cpp
        src/pixel/plugins/shared/openjpeg_v2/static_api.cpp
    )
    target_include_directories(dicomsdl_pixel_openjpeg_plugin_static
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/abi
            ${dicomsdl_openjpeg_SOURCE_DIR}/src/lib/openjp2
            ${dicomsdl_openjpeg_BINARY_DIR}/src/lib/openjp2
    )
    if(TARGET openjp2_static)
        target_link_libraries(dicomsdl_pixel_openjpeg_plugin_static PRIVATE openjp2_static)
    else()
        target_link_libraries(dicomsdl_pixel_openjpeg_plugin_static PRIVATE openjp2)
    endif()
    target_compile_features(dicomsdl_pixel_openjpeg_plugin_static PRIVATE cxx_std_20)
    dicomsdl_set_target_output_dirs(dicomsdl_pixel_openjpeg_plugin_static)
    set_target_properties(dicomsdl_pixel_openjpeg_plugin_static PROPERTIES
        OUTPUT_NAME "dicomsdl_pixel_openjpeg_plugin_static"
    )
endif()

if(DICOMSDL_PIXEL_JPEG_STATIC_PLUGIN)
    add_library(dicomsdl_pixel_jpeg_plugin_static STATIC
        src/pixel/plugins/shared/jpeg_v2/common.cpp
        src/pixel/plugins/shared/jpeg_v2/decode.cpp
        src/pixel/plugins/shared/jpeg_v2/encode.cpp
        src/pixel/plugins/shared/jpeg_v2/static_api.cpp
    )
    target_include_directories(dicomsdl_pixel_jpeg_plugin_static
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/abi
            ${dicomsdl_libjpeg_turbo_SOURCE_DIR}/src
            ${dicomsdl_libjpeg_turbo_BINARY_DIR}
    )
    target_link_libraries(dicomsdl_pixel_jpeg_plugin_static PRIVATE dicomsdl_turbojpeg)
    target_compile_features(dicomsdl_pixel_jpeg_plugin_static PRIVATE cxx_std_20)
    dicomsdl_set_target_output_dirs(dicomsdl_pixel_jpeg_plugin_static)
    set_target_properties(dicomsdl_pixel_jpeg_plugin_static PROPERTIES
        OUTPUT_NAME "dicomsdl_pixel_jpeg_plugin_static"
    )
endif()

if(DICOMSDL_PIXEL_JPEGLS_STATIC_PLUGIN)
    add_library(dicomsdl_pixel_jpegls_plugin_static STATIC
        src/pixel/plugins/shared/jpegls_v2/common.cpp
        src/pixel/plugins/shared/jpegls_v2/decode.cpp
        src/pixel/plugins/shared/jpegls_v2/encode.cpp
        src/pixel/plugins/shared/jpegls_v2/static_api.cpp
    )
    target_include_directories(dicomsdl_pixel_jpegls_plugin_static
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/abi
    )
    target_link_libraries(dicomsdl_pixel_jpegls_plugin_static PRIVATE charls)
    target_compile_features(dicomsdl_pixel_jpegls_plugin_static PRIVATE cxx_std_20)
    dicomsdl_set_target_output_dirs(dicomsdl_pixel_jpegls_plugin_static)
    set_target_properties(dicomsdl_pixel_jpegls_plugin_static PROPERTIES
        OUTPUT_NAME "dicomsdl_pixel_jpegls_plugin_static"
    )
endif()

if(DICOMSDL_PIXEL_HTJ2K_STATIC_PLUGIN)
    if(NOT TARGET openjph)
        message(FATAL_ERROR "DICOMSDL_PIXEL_HTJ2K_STATIC_PLUGIN requires OpenJPH (target openjph).")
    endif()
    add_library(dicomsdl_pixel_htj2k_plugin_static STATIC
        src/pixel/plugins/shared/htj2k_v2/common.cpp
        src/pixel/plugins/shared/htj2k_v2/decode.cpp
        src/pixel/plugins/shared/htj2k_v2/encode.cpp
        src/pixel/plugins/shared/htj2k_v2/static_api.cpp
    )
    target_include_directories(dicomsdl_pixel_htj2k_plugin_static
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/abi
    )
    target_link_libraries(dicomsdl_pixel_htj2k_plugin_static PRIVATE openjph)
    target_compile_features(dicomsdl_pixel_htj2k_plugin_static PRIVATE cxx_std_20)
    dicomsdl_set_target_output_dirs(dicomsdl_pixel_htj2k_plugin_static)
    set_target_properties(dicomsdl_pixel_htj2k_plugin_static PROPERTIES
        OUTPUT_NAME "dicomsdl_pixel_htj2k_plugin_static"
    )
endif()

if(DICOMSDL_PIXEL_JPEGXL_STATIC_PLUGIN)
    if(NOT TARGET jxl OR NOT TARGET jxl_threads OR NOT TARGET jxl_cms)
        message(FATAL_ERROR "DICOMSDL_PIXEL_JPEGXL_STATIC_PLUGIN requires libjxl targets (jxl/jxl_threads/jxl_cms).")
    endif()
    add_library(dicomsdl_pixel_jpegxl_plugin_static STATIC
        src/pixel/plugins/shared/jpegxl_v2/common.cpp
        src/pixel/plugins/shared/jpegxl_v2/decode.cpp
        src/pixel/plugins/shared/jpegxl_v2/encode.cpp
        src/pixel/plugins/shared/jpegxl_v2/static_api.cpp
    )
    target_include_directories(dicomsdl_pixel_jpegxl_plugin_static
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/abi
    )
    target_link_libraries(dicomsdl_pixel_jpegxl_plugin_static PRIVATE jxl jxl_threads jxl_cms)
    target_compile_features(dicomsdl_pixel_jpegxl_plugin_static PRIVATE cxx_std_20)
    dicomsdl_set_target_output_dirs(dicomsdl_pixel_jpegxl_plugin_static)
    set_target_properties(dicomsdl_pixel_jpegxl_plugin_static PROPERTIES
        OUTPUT_NAME "dicomsdl_pixel_jpegxl_plugin_static"
    )
endif()

if(DICOMSDL_PIXEL_RUNTIME)
    if(NOT TARGET dicomsdl_pixel_core)
        message(FATAL_ERROR "DICOMSDL_PIXEL_RUNTIME requires DICOMSDL_PIXEL_CORE.")
    endif()

    add_library(dicomsdl_pixel_runtime STATIC
        src/pixel/runtime/plugin_registry_v2.cpp
        src/pixel/runtime/shared_plugin_loader_v2.cpp
        src/pixel/runtime/registry_bootstrap_v2.cpp
        src/pixel/runtime/host_adapter_v2.cpp
        src/pixel/runtime/runtime_registry_v2.cpp
    )
    target_include_directories(dicomsdl_pixel_runtime
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/abi
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/runtime
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/core
            ${CMAKE_CURRENT_SOURCE_DIR}/include
            ${CMAKE_CURRENT_BINARY_DIR}/generated/include
    )
    target_link_libraries(dicomsdl_pixel_runtime PRIVATE
        dicomsdl_pixel_core
        ${CMAKE_DL_LIBS}
    )

    if(TARGET dicomsdl_pixel_rle_plugin_static)
        target_link_libraries(dicomsdl_pixel_runtime PRIVATE dicomsdl_pixel_rle_plugin_static)
        target_compile_definitions(dicomsdl_pixel_runtime PRIVATE DICOMSDL_PIXEL_RUNTIME_WITH_RLE_STATIC=1)
    endif()
    if(TARGET dicomsdl_pixel_openjpeg_plugin_static)
        target_link_libraries(dicomsdl_pixel_runtime PRIVATE dicomsdl_pixel_openjpeg_plugin_static)
        target_compile_definitions(dicomsdl_pixel_runtime PRIVATE DICOMSDL_PIXEL_RUNTIME_WITH_OPENJPEG_STATIC=1)
    endif()
    if(TARGET dicomsdl_pixel_jpeg_plugin_static)
        target_link_libraries(dicomsdl_pixel_runtime PRIVATE dicomsdl_pixel_jpeg_plugin_static)
        target_compile_definitions(dicomsdl_pixel_runtime PRIVATE DICOMSDL_PIXEL_RUNTIME_WITH_JPEG_STATIC=1)
    endif()
    if(TARGET dicomsdl_pixel_jpegls_plugin_static)
        target_link_libraries(dicomsdl_pixel_runtime PRIVATE dicomsdl_pixel_jpegls_plugin_static)
        target_compile_definitions(dicomsdl_pixel_runtime PRIVATE DICOMSDL_PIXEL_RUNTIME_WITH_JPEGLS_STATIC=1)
    endif()
    if(TARGET dicomsdl_pixel_htj2k_plugin_static)
        target_link_libraries(dicomsdl_pixel_runtime PRIVATE dicomsdl_pixel_htj2k_plugin_static)
        target_compile_definitions(dicomsdl_pixel_runtime PRIVATE DICOMSDL_PIXEL_RUNTIME_WITH_HTJ2K_STATIC=1)
    endif()
    if(TARGET dicomsdl_pixel_jpegxl_plugin_static)
        target_link_libraries(dicomsdl_pixel_runtime PRIVATE dicomsdl_pixel_jpegxl_plugin_static)
        target_compile_definitions(dicomsdl_pixel_runtime PRIVATE DICOMSDL_PIXEL_RUNTIME_WITH_JPEGXL_STATIC=1)
    endif()

    target_compile_features(dicomsdl_pixel_runtime PRIVATE cxx_std_20)
    dicomsdl_set_target_output_dirs(dicomsdl_pixel_runtime)
    set_target_properties(dicomsdl_pixel_runtime PROPERTIES
        OUTPUT_NAME "dicomsdl_pixel_runtime"
    )

    target_link_libraries(dicomsdl PRIVATE dicomsdl_pixel_runtime)
    target_compile_definitions(dicomsdl PRIVATE DICOMSDL_PIXEL_RUNTIME_ENABLED=1)
endif()

if(DICOMSDL_PIXEL_OPENJPEG_PLUGIN)
    add_library(dicomsdl_pixel_openjpeg_plugin SHARED
        src/pixel/plugins/shared/openjpeg_v2/error.cpp
        src/pixel/plugins/shared/openjpeg_v2/value_utils.cpp
        src/pixel/plugins/shared/openjpeg_v2/openjpeg_helpers.cpp
        src/pixel/plugins/shared/openjpeg_v2/context_api.cpp
        src/pixel/plugins/shared/openjpeg_v2/decode.cpp
        src/pixel/plugins/shared/openjpeg_v2/encode.cpp
        src/pixel/plugins/shared/openjpeg_v2/shared_entry.cpp
    )
    target_include_directories(dicomsdl_pixel_openjpeg_plugin
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/abi
            ${dicomsdl_openjpeg_SOURCE_DIR}/src/lib/openjp2
            ${dicomsdl_openjpeg_BINARY_DIR}/src/lib/openjp2
    )
    if(TARGET openjp2_static)
        target_link_libraries(dicomsdl_pixel_openjpeg_plugin PRIVATE openjp2_static)
    else()
        target_link_libraries(dicomsdl_pixel_openjpeg_plugin PRIVATE openjp2)
    endif()
    target_compile_features(dicomsdl_pixel_openjpeg_plugin PRIVATE cxx_std_20)
    dicomsdl_set_target_output_dirs(dicomsdl_pixel_openjpeg_plugin)
    set_target_properties(dicomsdl_pixel_openjpeg_plugin PROPERTIES
        OUTPUT_NAME "dicomsdl_pixel_openjpeg_plugin"
    )
    dicomsdl_set_shared_plugin_rpath(dicomsdl_pixel_openjpeg_plugin)
endif()

if(DICOMSDL_PIXEL_JPEG_PLUGIN)
    add_library(dicomsdl_pixel_jpeg_plugin SHARED
        src/pixel/plugins/shared/jpeg_v2/common.cpp
        src/pixel/plugins/shared/jpeg_v2/decode.cpp
        src/pixel/plugins/shared/jpeg_v2/encode.cpp
        src/pixel/plugins/shared/jpeg_v2/shared_entry.cpp
    )
    target_include_directories(dicomsdl_pixel_jpeg_plugin
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/abi
            ${dicomsdl_libjpeg_turbo_SOURCE_DIR}/src
            ${dicomsdl_libjpeg_turbo_BINARY_DIR}
    )
    target_link_libraries(dicomsdl_pixel_jpeg_plugin PRIVATE dicomsdl_turbojpeg)
    target_compile_features(dicomsdl_pixel_jpeg_plugin PRIVATE cxx_std_20)
    dicomsdl_set_target_output_dirs(dicomsdl_pixel_jpeg_plugin)
    set_target_properties(dicomsdl_pixel_jpeg_plugin PROPERTIES
        OUTPUT_NAME "dicomsdl_pixel_jpeg_plugin"
    )
    dicomsdl_set_shared_plugin_rpath(dicomsdl_pixel_jpeg_plugin)
endif()

if(DICOMSDL_PIXEL_JPEGLS_PLUGIN)
    add_library(dicomsdl_pixel_jpegls_plugin SHARED
        src/pixel/plugins/shared/jpegls_v2/common.cpp
        src/pixel/plugins/shared/jpegls_v2/decode.cpp
        src/pixel/plugins/shared/jpegls_v2/encode.cpp
        src/pixel/plugins/shared/jpegls_v2/shared_entry.cpp
    )
    target_include_directories(dicomsdl_pixel_jpegls_plugin
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/abi
    )
    target_link_libraries(dicomsdl_pixel_jpegls_plugin PRIVATE charls)
    target_compile_features(dicomsdl_pixel_jpegls_plugin PRIVATE cxx_std_20)
    dicomsdl_set_target_output_dirs(dicomsdl_pixel_jpegls_plugin)
    set_target_properties(dicomsdl_pixel_jpegls_plugin PROPERTIES
        OUTPUT_NAME "dicomsdl_pixel_jpegls_plugin"
    )
    dicomsdl_set_shared_plugin_rpath(dicomsdl_pixel_jpegls_plugin)
endif()

if(DICOMSDL_PIXEL_HTJ2K_PLUGIN)
    if(NOT TARGET openjph)
        message(FATAL_ERROR "DICOMSDL_PIXEL_HTJ2K_PLUGIN requires OpenJPH (target openjph).")
    endif()
    add_library(dicomsdl_pixel_htj2k_plugin SHARED
        src/pixel/plugins/shared/htj2k_v2/common.cpp
        src/pixel/plugins/shared/htj2k_v2/decode.cpp
        src/pixel/plugins/shared/htj2k_v2/encode.cpp
        src/pixel/plugins/shared/htj2k_v2/shared_entry.cpp
    )
    target_include_directories(dicomsdl_pixel_htj2k_plugin
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/abi
    )
    target_link_libraries(dicomsdl_pixel_htj2k_plugin PRIVATE openjph)
    target_compile_features(dicomsdl_pixel_htj2k_plugin PRIVATE cxx_std_20)
    dicomsdl_set_target_output_dirs(dicomsdl_pixel_htj2k_plugin)
    set_target_properties(dicomsdl_pixel_htj2k_plugin PROPERTIES
        OUTPUT_NAME "dicomsdl_pixel_htj2k_plugin"
    )
    dicomsdl_set_shared_plugin_rpath(dicomsdl_pixel_htj2k_plugin)
endif()

if(DICOMSDL_PIXEL_JPEGXL_PLUGIN)
    if(NOT TARGET jxl OR NOT TARGET jxl_threads OR NOT TARGET jxl_cms)
        message(FATAL_ERROR "DICOMSDL_PIXEL_JPEGXL_PLUGIN requires libjxl targets (jxl/jxl_threads/jxl_cms).")
    endif()
    add_library(dicomsdl_pixel_jpegxl_plugin SHARED
        src/pixel/plugins/shared/jpegxl_v2/common.cpp
        src/pixel/plugins/shared/jpegxl_v2/decode.cpp
        src/pixel/plugins/shared/jpegxl_v2/encode.cpp
        src/pixel/plugins/shared/jpegxl_v2/shared_entry.cpp
    )
    target_include_directories(dicomsdl_pixel_jpegxl_plugin
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/abi
    )
    target_link_libraries(dicomsdl_pixel_jpegxl_plugin PRIVATE jxl jxl_threads jxl_cms)
    target_compile_features(dicomsdl_pixel_jpegxl_plugin PRIVATE cxx_std_20)
    dicomsdl_set_target_output_dirs(dicomsdl_pixel_jpegxl_plugin)
    set_target_properties(dicomsdl_pixel_jpegxl_plugin PROPERTIES
        OUTPUT_NAME "dicomsdl_pixel_jpegxl_plugin"
    )
    dicomsdl_set_shared_plugin_rpath(dicomsdl_pixel_jpegxl_plugin)
endif()
