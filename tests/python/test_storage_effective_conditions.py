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


def _ct_storage_uid() -> str:
    return "1.2.840.10008.5.1.4.1.1.2"


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

    person_name_rows = _nested_effective_rows(ds, "PersonName")
    text_value_rows = _nested_effective_rows(ds, "TextValue")

    assert len(person_name_rows) == 2
    assert len(text_value_rows) == 2

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
