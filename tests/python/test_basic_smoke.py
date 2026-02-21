import pathlib

import dicomsdl as dicom

def _test_file(name: str = "test_le.dcm") -> str:
	return str(pathlib.Path(__file__).resolve().parent.parent / name)


def test_keyword_roundtrip():
	tag, vr = dicom.keyword_to_tag_vr("PatientName")
	assert int(tag) == 0x00100010
	assert vr.str() == "PN"
	assert dicom.tag_to_keyword(tag) == "PatientName"
	assert dicom.tag_to_keyword(int(tag)) == "PatientName"


def test_custom_px_vr():
	px = dicom.VR.from_string("PX")
	assert not px.is_sequence()
	assert px.is_pixel_sequence()
	assert px.is_binary()
	assert px.str() == "PX"


def test_literal_and_file(tmp_path):
	using_literal = dicom.Tag("Rows")
	assert int(using_literal) == 0x00280010

	dcm_path = tmp_path / "dummy.dcm"
	dcm_path.write_bytes(b"DICM")
	obj = dicom.read_file(str(dcm_path))
	assert pathlib.Path(obj.path).resolve() == dcm_path.resolve()

	mem = dicom.read_bytes(b"TEST", name="memory-buffer")
	assert mem.path == "memory-buffer"


def test_uid_lookup_roundtrip():
	uid = dicom.Uid("ImplicitVRLittleEndian")
	assert uid.value == "1.2.840.10008.1.2"
	assert uid.keyword == "ImplicitVRLittleEndian"

	by_value = dicom.uid_from_value("1.2.840.10008.1.2")
	assert by_value == uid

	lookup_keyword = dicom.lookup_uid("ImplicitVRLittleEndian")
	assert lookup_keyword == uid

	lookup_value = dicom.lookup_uid("1.2.840.10008.1.2")
	assert lookup_value == uid

	assert dicom.lookup_uid("1.2.3.4.5.6.7.8.9") is None


def test_uid_keyword_optional():
	uid = dicom.uid_from_keyword("JPEGBaseline8Bit")
	assert uid.value == "1.2.840.10008.1.2.4.50"
	assert uid.keyword == "JPEGBaseline8Bit"
	assert uid.name
	assert uid.type


def test_uid_generation_helpers():
	assert dicom.UID_PREFIX == dicom.uid_prefix()
	assert dicom.IMPLEMENTATION_CLASS_UID == dicom.implementation_class_uid()
	assert dicom.IMPLEMENTATION_VERSION_NAME == dicom.implementation_version_name()

	assert dicom.is_valid_uid_text_strict(dicom.UID_PREFIX)
	assert dicom.is_valid_uid_text_strict(dicom.IMPLEMENTATION_CLASS_UID)
	assert not dicom.is_valid_uid_text_strict("1.2..840")

	suffixed = dicom.make_uid_with_suffix(42)
	assert suffixed == "1.3.6.1.4.1.56559.42"
	assert dicom.is_valid_uid_text_strict(suffixed)

	custom = dicom.make_uid_with_suffix(7, "1.2.840.113619")
	assert custom == "1.2.840.113619.7"
	composed = dicom.make_uid_with_suffix(11, "1.2.840.10008")
	assert composed is not None
	composed = dicom.make_uid_with_suffix(22, composed)
	assert composed is not None
	composed = dicom.make_uid_with_suffix(33, composed)
	assert composed == "1.2.840.10008.11.22.33"
	zero_component = dicom.make_uid_with_suffix(7, "1.2.3")
	assert zero_component is not None
	zero_component = dicom.make_uid_with_suffix(0, zero_component)
	assert zero_component == "1.2.3.7.0"

	long_root = "1"
	while len(long_root) + 2 <= 61:
		long_root += ".1"
	assert dicom.make_uid_with_suffix(1234567890123456789, long_root) is None
	extended_long = dicom.try_append_uid(long_root, 1234567890123456789)
	assert extended_long is not None
	assert len(extended_long) <= 64
	assert dicom.is_valid_uid_text_strict(extended_long)
	assert extended_long.startswith(long_root[:30])
	if not long_root[:30].endswith("."):
		assert extended_long.startswith(long_root[:30] + ".")

	assert dicom.try_append_uid("not-a-uid", 7) is None
	extended_short = dicom.try_append_uid("1.2.840.10008", 7)
	assert extended_short == "1.2.840.10008.7"
	assert dicom.append_uid("1.2.840.10008", 8) == "1.2.840.10008.8"

	generated = dicom.generate_uid()
	assert generated.startswith(dicom.UID_PREFIX + ".")
	assert dicom.is_valid_uid_text_strict(generated)
	generated_with_components = dicom.make_uid_with_suffix(7, generated)
	assert generated_with_components is not None
	generated_with_components = dicom.make_uid_with_suffix(8, generated_with_components)
	assert generated_with_components is not None
	generated_with_components = dicom.make_uid_with_suffix(9, generated_with_components)
	assert generated_with_components is not None
	assert dicom.is_valid_uid_text_strict(generated_with_components)
	assert len(generated_with_components) <= 64

	optional_generated = dicom.try_generate_uid()
	assert optional_generated is not None
	assert dicom.is_valid_uid_text_strict(optional_generated)
	optional_generated_with_components = dicom.make_uid_with_suffix(9, optional_generated)
	assert optional_generated_with_components is not None
	optional_generated_with_components = dicom.make_uid_with_suffix(8, optional_generated_with_components)
	assert optional_generated_with_components is not None
	optional_generated_with_components = dicom.make_uid_with_suffix(7, optional_generated_with_components)
	assert optional_generated_with_components is not None
	assert dicom.is_valid_uid_text_strict(optional_generated_with_components)

	assert dicom.is_valid_uid_text_strict(dicom.generate_sop_instance_uid())
	assert dicom.is_valid_uid_text_strict(dicom.generate_series_instance_uid())
	assert dicom.is_valid_uid_text_strict(dicom.generate_study_instance_uid())


def test_dicomfile_dir_includes_dataset_members():
	df = dicom.read_file(_test_file())
	names = dir(df)
	assert "dataset" in names
	assert "path" in names
	assert "to_array" in names
	assert "Rows" in names


def test_len_delegates_to_root_dataset():
	df = dicom.read_file(_test_file())
	assert len(df) == df.dataset.size()
	assert len(df) > 0


def test_dump_available_on_file_and_dataset():
	df = dicom.read_file(_test_file())
	text_from_file = df.dump()
	text_from_dataset = df.dataset.dump()
	text_from_file_short = df.dump(40)
	text_from_dataset_short = df.dataset.dump(40)
	text_from_file_no_offset = df.dump(80, False)
	text_from_dataset_no_offset = df.dataset.dump(80, False)
	assert "TAG\tVR\tLEN\tVM\tOFFSET\tVALUE\tKEYWORD" in text_from_file
	assert "TAG\tVR\tLEN\tVM\tOFFSET\tVALUE\tKEYWORD" in text_from_dataset
	assert "TAG\tVR\tLEN\tVM\tVALUE\tKEYWORD" in text_from_file_no_offset
	assert "TAG\tVR\tLEN\tVM\tVALUE\tKEYWORD" in text_from_dataset_no_offset
	assert "OFFSET\tVALUE" not in text_from_file_no_offset
	assert "OFFSET\tVALUE" not in text_from_dataset_no_offset
	assert "'00020010'" in text_from_file
	assert isinstance(text_from_file_short, str)
	assert isinstance(text_from_dataset_short, str)
