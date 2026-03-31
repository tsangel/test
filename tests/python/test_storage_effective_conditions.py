import dicomsdl as dicom


def _make_basic_text_sr_with_mixed_value_types() -> dicom.DataSet:
    ds = dicom.DataSet()
    ds.set_value("SOPClassUID", "1.2.840.10008.5.1.4.1.1.88.11")

    content = ds.ensure_dataelement("ContentSequence", dicom.VR.SQ).sequence

    text_item = content.add_dataset()
    text_item.set_value("ValueType", "TEXT")
    text_item.set_value("RelationshipType", "CONTAINS")
    text_item.set_value("TextValue", "hello")

    pname_item = content.add_dataset()
    pname_item.set_value("ValueType", "PNAME")
    pname_item.set_value("RelationshipType", "CONTAINS")
    pname_item.set_value("PersonName", "Doe^John")

    return ds


def _make_nested_basic_text_sr() -> dicom.DataSet:
    ds = dicom.DataSet()
    ds.set_value("SOPClassUID", "1.2.840.10008.5.1.4.1.1.88.11")

    root = ds.ensure_dataelement("ContentSequence", dicom.VR.SQ).sequence

    container_item = root.add_dataset()
    container_item.set_value("ValueType", "CONTAINER")
    container_item.set_value("RelationshipType", "CONTAINS")

    nested = container_item.ensure_dataelement("ContentSequence", dicom.VR.SQ).sequence
    text_item = nested.add_dataset()
    text_item.set_value("ValueType", "TEXT")
    text_item.set_value("RelationshipType", "CONTAINS")
    text_item.set_value("TextValue", "hello")

    return ds


def _ct_storage_uid() -> str:
    return "1.2.840.10008.5.1.4.1.1.2"


def _make_rt_image_storage(*image_type_values: str) -> dicom.DataSet:
    ds = dicom.DataSet()
    ds.set_value("SOPClassUID", "1.2.840.10008.5.1.4.1.1.481.1")
    if image_type_values:
        ds.set_value("ImageType", list(image_type_values))
    return ds


def _nested_effective_rows(ds: dicom.DataSet, keyword: str) -> list[dict]:
    return [
        row
        for row in dicom.list_effective_storage_attributes(
            ds,
            keyword=keyword,
            include_prohibited=True,
        )
        if row["keyword"] == keyword and row["path"].startswith("0040A730/")
    ]


def test_list_effective_storage_attributes_uses_item_local_dataset_scope():
    ds = _make_basic_text_sr_with_mixed_value_types()

    all_rows = dicom.list_effective_storage_attributes(
        ds,
        include_prohibited=True,
    )
    person_name_rows = _nested_effective_rows(ds, "PersonName")
    text_value_rows = _nested_effective_rows(ds, "TextValue")
    all_person_name_rows = [row for row in all_rows if row["keyword"] == "PersonName"]
    all_text_value_rows = [row for row in all_rows if row["keyword"] == "TextValue"]

    assert len(person_name_rows) == 2
    assert len(text_value_rows) == 2
    assert len(all_person_name_rows) == 2
    assert len(all_text_value_rows) == 2
    assert all(row["path"].startswith("0040A730/") for row in all_person_name_rows)
    assert all(row["path"].startswith("0040A730/") for row in all_text_value_rows)

    assert sum(
        row["effective_type_name"] == "type1" and row["condition_state_name"] == "active"
        for row in person_name_rows
    ) == 1
    assert sum(
        row["effective_type_name"] == "prohibited" and row["condition_state_name"] == "inactive"
        for row in person_name_rows
    ) == 1

    assert sum(
        row["effective_type_name"] == "type1" and row["condition_state_name"] == "active"
        for row in text_value_rows
    ) == 1
    assert sum(
        row["effective_type_name"] == "prohibited" and row["condition_state_name"] == "inactive"
        for row in text_value_rows
    ) == 1


def test_list_effective_storage_attributes_handles_recursive_sr_items():
    ds = _make_nested_basic_text_sr()

    rows = [
        row
        for row in dicom.list_effective_storage_attributes(
            ds,
            keyword="TextValue",
            include_prohibited=True,
        )
        if row["keyword"] == "TextValue"
    ]

    assert any(
        row["effective_type_name"] == "type1" and row["condition_state_name"] == "active"
        for row in rows
    )


def test_list_effective_storage_attributes_keeps_root_rules_for_context_modules():
    ds = _make_rt_image_storage()

    rows = [
        row
        for row in dicom.list_effective_storage_attributes(
            ds,
            keyword="ImageType",
            include_prohibited=True,
        )
        if row["keyword"] == "ImageType"
        and row["path"] == "00080008"
        and row["component_section_id"] == "sect_C.8.8.2"
    ]

    assert len(rows) == 1
    assert rows[0]["condition_state_name"] == "not_conditional"
    assert rows[0]["effective_type_name"] == "type1"


def test_list_effective_storage_attributes_matches_multivalued_eqtext_conditions():
    ds = _make_rt_image_storage("ORIGINAL", "PRIMARY", "PORTAL")
    exposure_sequence = ds.ensure_dataelement("ExposureSequence", dicom.VR.SQ).sequence
    exposure_sequence.add_dataset()

    image_type_rows = [
        row
        for row in dicom.list_effective_storage_attributes(
            ds,
            keyword="ImageType",
            include_prohibited=True,
        )
        if row["keyword"] == "ImageType"
    ]
    rows = [
        row
        for row in dicom.list_effective_storage_attributes(
            ds,
            keyword="MetersetExposure",
            include_prohibited=True,
        )
        if row["keyword"] == "MetersetExposure"
        and row["path"] == "30020030/30020032"
        and row["component_section_id"] == "sect_C.8.8.2"
    ]

    assert len(image_type_rows) == 2
    assert sum(
        row["component_section_id"] == "sect_C.7.6.1" and row["path"] == "00080008"
        for row in image_type_rows
    ) == 1
    assert len(rows) == 1
    assert rows[0]["condition_state_name"] == "active"
    assert rows[0]["effective_type_name"] == "type2"


def test_list_effective_storage_attributes_keeps_root_conditional_rt_image_rules():
    ds = _make_rt_image_storage("ORIGINAL", "PRIMARY", "PORTAL")

    rows = [
        row
        for row in dicom.list_effective_storage_attributes(
            ds,
            keyword="ReportedValuesOrigin",
            include_prohibited=True,
        )
        if row["keyword"] == "ReportedValuesOrigin"
        and row["path"] == "3002000A"
        and row["component_section_id"] == "sect_C.8.8.2"
    ]

    assert len(rows) == 1
    assert rows[0]["condition_state_name"] == "active"
    assert rows[0]["effective_type_name"] == "type2"


def test_make_storage_classifier_falls_back_to_media_storage_sop_class_uid_for_dataset():
    ds = dicom.DataSet()
    ds.set_value("MediaStorageSOPClassUID", _ct_storage_uid())

    classifier = dicom.make_storage_classifier(ds)

    assert classifier is not None
    assert classifier.sop_class_uid == _ct_storage_uid()
    assert dicom.list_effective_storage_modules(ds)


def test_make_storage_classifier_falls_back_to_media_storage_sop_class_uid_for_dicom_file():
    df = dicom.DicomFile()
    df.set_value("MediaStorageSOPClassUID", _ct_storage_uid())

    classifier = dicom.make_storage_classifier(df)

    assert classifier is not None
    assert classifier.sop_class_uid == _ct_storage_uid()
    assert dicom.list_effective_storage_modules(df)
