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

    add_executable(dicomsdl_codec_cycle_roundtrip
        tests/codec_cycle_roundtrip.cpp
    )
    target_link_libraries(dicomsdl_codec_cycle_roundtrip PRIVATE dicomsdl)
    add_test(NAME codec_cycle_roundtrip COMMAND dicomsdl_codec_cycle_roundtrip)
    set_tests_properties(codec_cycle_roundtrip PROPERTIES LABELS "dicomsdl")

    add_executable(dicomsdl_codec_error_boundary
        tests/codec_error_boundary.cpp
    )
    target_link_libraries(dicomsdl_codec_error_boundary PRIVATE dicomsdl)
    add_test(NAME codec_error_boundary COMMAND dicomsdl_codec_error_boundary)
    set_tests_properties(codec_error_boundary PROPERTIES LABELS "dicomsdl")

    add_executable(dicomsdl_codec_cycle_realdata
        tests/codec_cycle_realdata.cpp
    )
    target_link_libraries(dicomsdl_codec_cycle_realdata PRIVATE dicomsdl)

    if(TARGET dicomsdl_pixel_core)
        add_executable(dicomsdl_pixel_core_uncompressed_smoke
            tests/pixel_core_uncompressed_smoke.cpp
        )
        target_include_directories(dicomsdl_pixel_core_uncompressed_smoke
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/abi
                ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/core
        )
        target_link_libraries(dicomsdl_pixel_core_uncompressed_smoke PRIVATE
            dicomsdl_pixel_core
        )
        target_compile_features(dicomsdl_pixel_core_uncompressed_smoke PRIVATE cxx_std_20)
        add_test(NAME pixel_core_uncompressed_smoke
            COMMAND dicomsdl_pixel_core_uncompressed_smoke)
        set_tests_properties(pixel_core_uncompressed_smoke PROPERTIES LABELS "dicomsdl")
    endif()

    if(TARGET dicomsdl_pixel_rle_plugin_static)
        add_executable(dicomsdl_pixel_rle_static_plugin_smoke
            tests/pixel_rle_static_plugin_smoke.cpp
        )
        target_include_directories(dicomsdl_pixel_rle_static_plugin_smoke
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/abi
                ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/plugins/static/rle_v2
        )
        target_link_libraries(dicomsdl_pixel_rle_static_plugin_smoke PRIVATE
            dicomsdl_pixel_rle_plugin_static
        )
        target_compile_features(dicomsdl_pixel_rle_static_plugin_smoke PRIVATE cxx_std_20)
        add_test(NAME pixel_rle_static_plugin_smoke
            COMMAND dicomsdl_pixel_rle_static_plugin_smoke)
        set_tests_properties(pixel_rle_static_plugin_smoke PROPERTIES LABELS "dicomsdl")
    endif()

    if(TARGET dicomsdl_pixel_jpeg_plugin)
        add_executable(dicomsdl_pixel_jpeg_plugin_smoke
            tests/pixel_jpeg_plugin_smoke.cpp
        )
        target_compile_features(dicomsdl_pixel_jpeg_plugin_smoke PRIVATE cxx_std_20)
        target_include_directories(dicomsdl_pixel_jpeg_plugin_smoke
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/abi
        )
        if(CMAKE_DL_LIBS)
            target_link_libraries(dicomsdl_pixel_jpeg_plugin_smoke PRIVATE ${CMAKE_DL_LIBS})
        endif()
        add_dependencies(dicomsdl_pixel_jpeg_plugin_smoke dicomsdl_pixel_jpeg_plugin)
        add_test(NAME pixel_jpeg_plugin_smoke
            COMMAND dicomsdl_pixel_jpeg_plugin_smoke
            $<TARGET_FILE:dicomsdl_pixel_jpeg_plugin>)
        set_tests_properties(pixel_jpeg_plugin_smoke PROPERTIES LABELS "dicomsdl")
    endif()

    if(TARGET dicomsdl_pixel_htj2k_plugin)
        add_executable(dicomsdl_pixel_htj2k_plugin_smoke
            tests/pixel_htj2k_plugin_smoke.cpp
        )
        target_compile_features(dicomsdl_pixel_htj2k_plugin_smoke PRIVATE cxx_std_20)
        target_include_directories(dicomsdl_pixel_htj2k_plugin_smoke
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/abi
        )
        if(CMAKE_DL_LIBS)
            target_link_libraries(dicomsdl_pixel_htj2k_plugin_smoke PRIVATE ${CMAKE_DL_LIBS})
        endif()
        add_dependencies(dicomsdl_pixel_htj2k_plugin_smoke dicomsdl_pixel_htj2k_plugin)
        add_test(NAME pixel_htj2k_plugin_smoke
            COMMAND dicomsdl_pixel_htj2k_plugin_smoke
            $<TARGET_FILE:dicomsdl_pixel_htj2k_plugin>)
        set_tests_properties(pixel_htj2k_plugin_smoke PROPERTIES LABELS "dicomsdl")
    endif()

    if(TARGET dicomsdl_pixel_runtime AND TARGET dicomsdl_pixel_jpeg_plugin)
        add_executable(dicomsdl_pixel_runtime_registry_bootstrap_smoke
            tests/pixel_runtime_registry_bootstrap_smoke.cpp
        )
        target_link_libraries(dicomsdl_pixel_runtime_registry_bootstrap_smoke PRIVATE
            dicomsdl_pixel_runtime
        )
        target_compile_features(dicomsdl_pixel_runtime_registry_bootstrap_smoke PRIVATE cxx_std_20)
        add_dependencies(dicomsdl_pixel_runtime_registry_bootstrap_smoke dicomsdl_pixel_jpeg_plugin)
        add_test(NAME pixel_runtime_registry_bootstrap_smoke
            COMMAND dicomsdl_pixel_runtime_registry_bootstrap_smoke
            $<TARGET_FILE:dicomsdl_pixel_jpeg_plugin>)
        set_tests_properties(pixel_runtime_registry_bootstrap_smoke PROPERTIES LABELS "dicomsdl")
    endif()

    if(TARGET dicomsdl_pixel_runtime)
        add_executable(dicomsdl_pixel_host_adapter_smoke
            tests/pixel_host_adapter_smoke.cpp
        )
        target_include_directories(dicomsdl_pixel_host_adapter_smoke
            PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}/include
                ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/abi
                ${CMAKE_CURRENT_SOURCE_DIR}/src/pixel/runtime
                ${CMAKE_CURRENT_BINARY_DIR}/generated/include
        )
        target_link_libraries(dicomsdl_pixel_host_adapter_smoke PRIVATE
            dicomsdl_pixel_runtime
        )
        target_compile_features(dicomsdl_pixel_host_adapter_smoke PRIVATE cxx_std_20)
        add_test(NAME pixel_host_adapter_smoke
            COMMAND dicomsdl_pixel_host_adapter_smoke)
        set_tests_properties(pixel_host_adapter_smoke PROPERTIES LABELS "dicomsdl")
    endif()

    foreach(_dicomsdl_test_target IN ITEMS
        dicomsdl_dictionary_consistency
        dicomsdl_basic_smoke
        dicomsdl_numeric_values
        dicomsdl_uid_consistency
        dicomsdl_separation_regression
        dicomsdl_dataset_ordering_regression
        dicomsdl_codec_cycle_roundtrip
        dicomsdl_codec_error_boundary
        dicomsdl_codec_cycle_realdata
        dicomsdl_pixel_core_uncompressed_smoke
        dicomsdl_pixel_rle_static_plugin_smoke
        dicomsdl_pixel_jpeg_plugin_smoke
        dicomsdl_pixel_htj2k_plugin_smoke
        dicomsdl_pixel_runtime_registry_bootstrap_smoke
        dicomsdl_pixel_host_adapter_smoke
    )
        if(TARGET ${_dicomsdl_test_target})
            dicomsdl_apply_pixel_static_plugin_defines(${_dicomsdl_test_target})
        endif()
    endforeach()
    unset(_dicomsdl_test_target)
endif()
