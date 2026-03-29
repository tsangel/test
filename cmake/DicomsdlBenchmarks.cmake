option(DICOM_BUILD_BENCHMARKS "Build benchmark executables for dicomsdl" OFF)

if(DICOM_BUILD_BENCHMARKS)
    add_executable(dicomsdl_keyword_lookup_bench
        benchmarks/keyword_lookup_bench.cpp
    )
    target_link_libraries(dicomsdl_keyword_lookup_bench PRIVATE dicomsdl)
    target_compile_features(dicomsdl_keyword_lookup_bench PRIVATE cxx_std_20)

    add_executable(dicomsdl_uid_lookup_bench
        benchmarks/uid_lookup_bench.cpp
    )
    target_link_libraries(dicomsdl_uid_lookup_bench PRIVATE dicomsdl)
    target_compile_features(dicomsdl_uid_lookup_bench PRIVATE cxx_std_20)

    add_executable(dicomsdl_uid_remapper_bench
        benchmarks/uid_remapper_bench.cpp
    )
    target_link_libraries(dicomsdl_uid_remapper_bench PRIVATE dicomsdl)
    target_compile_features(dicomsdl_uid_remapper_bench PRIVATE cxx_std_20)

    add_executable(dicomsdl_tag_path_access_bench
        benchmarks/tag_path_access_bench.cpp
    )
    target_link_libraries(dicomsdl_tag_path_access_bench PRIVATE dicomsdl)
    target_compile_features(dicomsdl_tag_path_access_bench PRIVATE cxx_std_20)

    add_executable(dicomsdl_tag_path_parser_microbench
        benchmarks/tag_path_parser_microbench.cpp
    )
    target_link_libraries(dicomsdl_tag_path_parser_microbench PRIVATE dicomsdl)
    target_compile_features(dicomsdl_tag_path_parser_microbench PRIVATE cxx_std_20)

    add_executable(dicomsdl_dataelement_queue_bench
        benchmarks/dataelement_queue_bench.cpp
    )
    target_link_libraries(dicomsdl_dataelement_queue_bench PRIVATE dicomsdl)
    target_compile_features(dicomsdl_dataelement_queue_bench PRIVATE cxx_std_20)

    add_executable(dicomsdl_dataset_read_microbench
        benchmarks/dataset_read_microbench.cpp
    )
    target_link_libraries(dicomsdl_dataset_read_microbench PRIVATE dicomsdl)
    target_compile_features(dicomsdl_dataset_read_microbench PRIVATE cxx_std_20)

    add_executable(dicomsdl_read_all_dcm
        benchmarks/read_all_dcm.cpp
    )
    target_link_libraries(dicomsdl_read_all_dcm PRIVATE dicomsdl)
    target_compile_features(dicomsdl_read_all_dcm PRIVATE cxx_std_20)

    add_executable(dicomsdl_streaming_write_stress
        benchmarks/streaming_write_stress.cpp
    )
    target_link_libraries(dicomsdl_streaming_write_stress PRIVATE dicomsdl)
    target_compile_features(dicomsdl_streaming_write_stress PRIVATE cxx_std_20)
endif()
