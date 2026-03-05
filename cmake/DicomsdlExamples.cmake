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

    add_executable(dicomconv
        examples/dicomconv.cpp
    )
    target_link_libraries(dicomconv PRIVATE dicomsdl)
    target_include_directories(dicomconv PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

    add_executable(batch_assign_with_error_check
        examples/batch_assign_with_error_check.cpp
    )
    target_link_libraries(batch_assign_with_error_check PRIVATE dicomsdl)
endif()
