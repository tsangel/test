import os
import pathlib
import shutil
import struct
import sys

import dicomsdl as dicom
import pytest

def _test_file(name: str = "test_le.dcm") -> str:
	return str(pathlib.Path(__file__).resolve().parent.parent / name)


def _pad_ui(text: str) -> bytes:
	raw = text.encode("ascii")
	if len(raw) % 2 == 1:
		raw += b"\x00"
	return raw


def _pack_explicit_le(group: int, element: int, vr: str, value: bytes, *, undefined: bool = False) -> bytes:
	header = struct.pack("<HH", group, element) + vr.encode("ascii")
	if undefined:
		return header + b"\x00\x00" + struct.pack("<I", 0xFFFFFFFF)
	if vr in {"OB", "OD", "OF", "OL", "OV", "OW", "SQ", "UC", "UR", "UT", "UN"}:
		return header + b"\x00\x00" + struct.pack("<I", len(value)) + value
	return header + struct.pack("<H", len(value)) + value


def _pack_item(tag_element: int, value: bytes, *, undefined: bool = False) -> bytes:
	length = 0xFFFFFFFF if undefined else len(value)
	return struct.pack("<HHI", 0xFFFE, tag_element, length) + value


def _build_sequence_pixel_sample() -> bytes:
	meta_version = _pack_explicit_le(0x0002, 0x0001, "OB", b"\x00\x01")
	meta_ts = _pack_explicit_le(0x0002, 0x0010, "UI", _pad_ui("1.2.840.10008.1.2.1"))
	meta_group_length = _pack_explicit_le(
	    0x0002, 0x0000, "UL", struct.pack("<I", len(meta_version) + len(meta_ts)))

	sop_class = _pack_explicit_le(0x0008, 0x0016, "UI", _pad_ui("1.2.840.10008.5.1.4.1.1.7"))
	sop_instance = _pack_explicit_le(0x0008, 0x0018, "UI", _pad_ui("1.2.3.4.5.6.7.8.9"))

	seq_item_payload = _pack_explicit_le(0x0008, 0x1155, "UI", _pad_ui("1.2.3.4.5.6"))
	seq_item = _pack_item(0xE000, seq_item_payload, undefined=True) + _pack_item(0xE00D, b"")
	seq_elem = (
	    _pack_explicit_le(0x0008, 0x1110, "SQ", b"", undefined=True)
	    + seq_item
	    + _pack_item(0xE0DD, b"")
	)

	pixel_frag = _pack_item(0xE000, b"\x01\x02\x03\x04")
	pixel_elem = (
	    _pack_explicit_le(0x7FE0, 0x0010, "OB", b"", undefined=True)
	    + _pack_item(0xE000, b"")
	    + pixel_frag
	    + _pack_item(0xE0DD, b"")
	)

	return b"\x00" * 128 + b"DICM" + meta_group_length + meta_version + meta_ts + sop_class + sop_instance + seq_elem + pixel_elem


def _build_malformed_sample() -> bytes:
	malformed_elem = struct.pack("<HH", 0x0010, 0x0010) + b"PN" + struct.pack("<H", 8) + b"AB"
	return b"\x00" * 128 + b"DICM" + malformed_elem


def _build_no_preamble_explicit_sample() -> bytes:
	sop_class = _pack_explicit_le(0x0008, 0x0016, "UI", _pad_ui("1.2.840.10008.5.1.4.1.1.7"))
	sop_instance = _pack_explicit_le(0x0008, 0x0018, "UI", _pad_ui("1.2.3.4.5.6.7.8.9"))
	patient_name = _pack_explicit_le(0x0010, 0x0010, "PN", b"PROBE^RAW")
	rows = _pack_explicit_le(0x0028, 0x0010, "US", struct.pack("<H", 1))
	return sop_class + sop_instance + patient_name + rows


def _build_no_preamble_three_element_sample() -> bytes:
	sop_class = _pack_explicit_le(0x0008, 0x0016, "UI", _pad_ui("1.2.840.10008.5.1.4.1.1.7"))
	sop_instance = _pack_explicit_le(0x0008, 0x0018, "UI", _pad_ui("1.2.3.4.5.6.7.8.9"))
	patient_name = _pack_explicit_le(0x0010, 0x0010, "PN", b"PROBE^THREE")
	return sop_class + sop_instance + patient_name


def _build_dicom_marker_without_meta_sample() -> bytes:
	return b"\x00" * 128 + b"DICM" + _build_no_preamble_three_element_sample()


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
	assert not px.uses_specific_character_set()
	assert not px.allows_multiple_text_values()
	assert px.str() == "PX"

	pn = dicom.VR.from_string("PN")
	assert pn.is_string()
	assert pn.uses_specific_character_set()
	assert pn.allows_multiple_text_values()

	ut = dicom.VR.from_string("UT")
	assert ut.uses_specific_character_set()
	assert not ut.allows_multiple_text_values()


def test_literal_and_file(tmp_path):
	using_literal = dicom.Tag("Rows")
	assert int(using_literal) == 0x00280010

	dcm_path = tmp_path / "dummy.dcm"
	dcm_path.write_bytes(b"DICM")
	obj = dicom.read_file(dcm_path)
	assert pathlib.Path(obj.path).resolve() == dcm_path.resolve()
	assert obj.has_error is False
	assert obj.error_message is None

	mem = dicom.read_bytes(b"TEST", name="memory-buffer")
	assert mem.path == "memory-buffer"


def test_is_dicom_file(tmp_path):
	fixture_path = pathlib.Path(_test_file())
	assert dicom.is_dicom_file(fixture_path)

	short_dcm = tmp_path / "short-dicm.dcm"
	short_dcm.write_bytes(b"DICM")
	assert dicom.is_dicom_file(short_dcm) is False

	raw_path = tmp_path / "raw-explicit.dcm"
	raw_path.write_bytes(_build_no_preamble_explicit_sample())
	assert dicom.is_dicom_file(raw_path)

	raw_three_path = tmp_path / "raw-three-explicit.dcm"
	raw_three_path.write_bytes(_build_no_preamble_three_element_sample())
	assert dicom.is_dicom_file(raw_three_path)

	dicm_without_meta = tmp_path / "dicm-no-meta.dcm"
	dicm_without_meta.write_bytes(_build_dicom_marker_without_meta_sample())
	assert dicom.is_dicom_file(dicm_without_meta)


def test_encoded_pixel_frame_access(tmp_path):
	encap_path = tmp_path / "encap-frame-access.dcm"
	encap_path.write_bytes(_build_sequence_pixel_sample())
	obj = dicom.read_file(encap_path)

	assert obj.encoded_pixel_frame_bytes(0) == b"\x01\x02\x03\x04"
	assert obj.encoded_pixel_frame_view(0).tobytes() == b"\x01\x02\x03\x04"

	obj.set_encoded_pixel_frame(0, memoryview(b"\x0A\x0B\x0C\x0D"))
	assert obj.encoded_pixel_frame_bytes(0) == b"\x0A\x0B\x0C\x0D"

	obj.add_encoded_pixel_frame(bytearray(b"\xAA\xBB"))
	assert int(obj.get_value("NumberOfFrames")) == 2
	assert obj.encoded_pixel_frame_bytes(1) == b"\xAA\xBB"

	with pytest.raises(Exception):
		dicom.read_file(pathlib.Path(_test_file())).encoded_pixel_frame_bytes(0)


def test_walk_api(tmp_path):
	encap_path = tmp_path / "walk-sequence-sample.dcm"
	encap_path.write_bytes(_build_sequence_pixel_sample())
	obj = dicom.read_file(encap_path)

	file_tags = []
	sequence_path = None
	sequence_contains = None
	nested_path = None
	nested_contains = None
	nested_unpacked_path = None
	nested_unpacked_tag = None
	for entry in obj.walk():
		file_tags.append(int(entry.element.tag))
		if int(entry.element.tag) == 0x00081110:
			sequence_path = entry.path.to_string()
			sequence_contains = entry.path.contains_sequence(dicom.Tag("ReferencedStudySequence"))
		if int(entry.element.tag) == 0x00081155 and nested_path is None:
			nested_path = entry.path.to_string()
			nested_contains = entry.path.contains_sequence(dicom.Tag("ReferencedStudySequence"))
			path, elem = entry
			nested_unpacked_path = path.to_string()
			nested_unpacked_tag = int(elem.tag)

	assert 0x00081110 in file_tags
	assert 0x00081155 in file_tags
	assert 0x7FE00010 in file_tags

	assert sequence_path == ""
	assert sequence_contains is False
	assert nested_path == "00081110.0"
	assert nested_contains is True
	assert nested_unpacked_path == "00081110.0"
	assert nested_unpacked_tag == 0x00081155

	dataset_entries = list(obj.dataset.walk())
	assert [int(entry.element.tag) for entry in dataset_entries] == file_tags

	walker = obj.walk()
	pruned_tags = []
	for entry in walker:
		pruned_tags.append(int(entry.element.tag))
		if int(entry.element.tag) == 0x00081110:
			entry.skip_sequence()

	assert 0x00081110 in pruned_tags
	assert 0x00081155 not in pruned_tags
	assert 0x7FE00010 in pruned_tags

	root_skip_walker = obj.walk()
	root_skip_tags = []
	for entry in root_skip_walker:
		root_skip_tags.append(int(entry.element.tag))
		if int(entry.element.tag) == 0x00081110:
			entry.skip_current_dataset()

	assert 0x00081110 in root_skip_tags
	assert 0x00081155 not in root_skip_tags
	assert 0x7FE00010 not in root_skip_tags

	ds = dicom.DataSet()
	ds.ensure_dataelement("SOPInstanceUID", dicom.VR.UI).value = "1.2.840.10008.1"
	ds.ensure_dataelement("SeriesInstanceUID", dicom.VR.UI).value = "1.2.840.10008.10"
	ds.ensure_dataelement(
		"ReferencedStudySequence.0.ReferencedSOPInstanceUID", dicom.VR.UI
	).value = "1.2.3"
	ds.ensure_dataelement("ReferencedStudySequence.0.SeriesInstanceUID", dicom.VR.UI).value = (
		"1.2.30"
	)
	ds.ensure_dataelement(
		"ReferencedStudySequence.1.ReferencedSOPInstanceUID", dicom.VR.UI
	).value = "1.2.4"
	ds.ensure_dataelement("ReferencedStudySequence.1.SeriesInstanceUID", dicom.VR.UI).value = (
		"1.2.40"
	)

	nested_skip_walker = ds.walk()
	saw_item0_series = False
	saw_item1_referenced = False
	saw_top_level_series = False
	for entry in nested_skip_walker:
		path, elem = entry
		if path.to_string() == "00081110.0" and int(elem.tag) == 0x00081155:
			entry.skip_current_dataset()
		if path.to_string() == "00081110.0" and int(elem.tag) == 0x0020000E:
			saw_item0_series = True
		if path.to_string() == "00081110.1" and int(elem.tag) == 0x00081155:
			saw_item1_referenced = True
		if path.to_string() == "" and int(elem.tag) == 0x0020000E:
			saw_top_level_series = True

	assert not saw_item0_series
	assert saw_item1_referenced
	assert saw_top_level_series


def test_python_pathlike_support(tmp_path):
	unicode_label = "\ud55c\uae00"
	source = dicom.read_file(pathlib.Path(_test_file()))
	out_path = tmp_path / f"pathlike-{unicode_label}-roundtrip.dcm"
	source.write_file(out_path)
	roundtrip = dicom.read_file(out_path)
	assert pathlib.Path(roundtrip.path).resolve() == out_path.resolve()
	assert roundtrip.has_error is False
	assert dicom.is_dicom_file(out_path)

	log_path = tmp_path / f"dicomsdl-{unicode_label}.log"
	reporter = dicom.FileReporter(log_path)
	dicom.set_thread_reporter(reporter)
	try:
		dicom.log_warn("python-pathlike-smoke")
	finally:
		dicom.set_thread_reporter(None)
	assert "python-pathlike-smoke" in log_path.read_text()

	with pytest.raises(ValueError):
		dicom.register_external_codec_plugin(tmp_path / f"missing-{unicode_label}.plugin")

	native_module = pathlib.Path(os.environ["DICOMSDL_NATIVE_MODULE_PATH"])
	plugin_copy = tmp_path / f"plugin-{unicode_label}{native_module.suffix}"
	shutil.copy2(native_module, plugin_copy)
	with pytest.raises(ValueError) as excinfo:
		dicom.register_external_codec_plugin(plugin_copy)
	assert "No mapping for the Unicode character" not in str(excinfo.value)
	assert plugin_copy.name in str(excinfo.value)


def test_keep_on_error_records_parse_failure():
	malformed = _build_malformed_sample()

	threw = False
	try:
		dicom.read_bytes(malformed, name="malformed-default")
	except Exception:
		threw = True
	assert threw

	kept = dicom.read_bytes(malformed, name="malformed-keep", keep_on_error=True)
	assert kept.has_error is True
	assert isinstance(kept.error_message, str)
	assert kept.error_message
	assert kept.dataset.size() >= 1


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


def test_dataelement_from_helpers_roundtrip():
	ds = dicom.DataSet()

	fd_elem = ds.add_dataelement(dicom.Tag("SliceThickness"), dicom.VR.FD)
	assert fd_elem.from_double(2.5)
	assert fd_elem.to_double() == 2.5

	ds_elem = ds.add_dataelement(dicom.Tag("WindowCenter"), dicom.VR.DS)
	assert ds_elem.from_double_vector([1.5, 2.5, 3.5])
	assert ds_elem.to_double_vector() == [1.5, 2.5, 3.5]

	at_single = ds.add_dataelement(dicom.Tag.from_value(0x00000901), dicom.VR.AT)
	patient_id = dicom.Tag("PatientID")
	assert at_single.from_tag(patient_id)
	assert int(at_single.to_tag()) == int(patient_id)

	at_vector = ds.add_dataelement(dicom.Tag.from_value(0x00000902), dicom.VR.AT)
	expected_tags = [dicom.Tag("Rows"), dicom.Tag("Columns")]
	assert at_vector.from_tag_vector(expected_tags)
	assert [int(tag) for tag in at_vector.to_tag_vector()] == [int(tag) for tag in expected_tags]


def test_zero_length_numeric_like_get_value_returns_empty_list_not_default():
	ds = dicom.DataSet()

	window_center = ds.add_dataelement(dicom.Tag("WindowCenter"), dicom.VR.DS)
	number_of_frames = ds.add_dataelement(dicom.Tag("NumberOfFrames"), dicom.VR.IS)
	slice_thickness = ds.add_dataelement(dicom.Tag("SliceThickness"), dicom.VR.FD)
	rows = ds.add_dataelement(dicom.Tag("Rows"), dicom.VR.US)
	frame_increment_pointer = ds.add_dataelement(dicom.Tag("FrameIncrementPointer"), dicom.VR.AT)

	assert ds.get_value("PatientName", default="DEFAULT") == "DEFAULT"

	assert ds.get_value("WindowCenter", default="DEFAULT") == []
	assert window_center.value == []

	assert ds.get_value("NumberOfFrames", default="DEFAULT") == []
	assert number_of_frames.value == []

	assert ds.get_value("SliceThickness", default="DEFAULT") == []
	assert slice_thickness.value == []

	assert ds.get_value("Rows", default="DEFAULT") == []
	assert rows.value == []

	assert ds.get_value("FrameIncrementPointer", default="DEFAULT") == []
	assert frame_increment_pointer.value == []


def test_zero_length_vector_accessors_return_empty_lists():
	ds = dicom.DataSet()

	rows = ds.add_dataelement(dicom.Tag("Rows"), dicom.VR.US)
	window_center = ds.add_dataelement(dicom.Tag("WindowCenter"), dicom.VR.DS)
	frame_increment_pointer = ds.add_dataelement(dicom.Tag("FrameIncrementPointer"), dicom.VR.AT)

	assert rows.to_int_vector() == []
	assert rows.to_long_vector() == []
	assert rows.to_longlong_vector() == []

	assert window_center.to_double_vector() == []
	assert frame_increment_pointer.to_tag_vector() == []


def test_remove_dataelement_accepts_tag_int_and_str_keys():
	ds = dicom.DataSet()
	ds.Rows = 512
	ds.Columns = 256
	ds.PatientName = "DOE^JOHN"

	ds.remove_dataelement("Rows")
	assert ds.get_value("Rows") is None

	ds.remove_dataelement(0x00280011)
	assert ds.get_value("Columns") is None

	ds.remove_dataelement(dicom.Tag("PatientName"))
	assert ds.get_value("PatientName") is None


def test_ensure_dataelement_preserves_or_resets_existing_elements():
	ds = dicom.DataSet()
	ds.Rows = 512

	preserved = ds.ensure_dataelement("Rows")
	assert preserved.vr == dicom.VR.US
	assert preserved.value == 512

	overridden = ds.ensure_dataelement("Rows", dicom.VR.UL)
	assert overridden.vr == dicom.VR.UL
	assert overridden.length == 0

	assert not ds.set_value("Rows", dicom.VR.SQ, 1)
	assert ds["Rows"].vr == dicom.VR.SQ
	assert ds["Rows"].length == 0

	inserted = ds.ensure_dataelement("Columns")
	assert inserted.vr == dicom.VR.US
	assert inserted.length == 0

	with pytest.raises(Exception):
		ds.ensure_dataelement(0x00091030)

	df = dicom.DicomFile()
	df.ensure_dataelement("BitsAllocated", dicom.VR.US)
	assert df["BitsAllocated"].vr == dicom.VR.US


def test_partial_load_set_value_add_and_ensure_fail_beyond_frontier():
	df = dicom.read_file(_test_file(), load_until=dicom.Tag("StudyTime"))
	assert df.get_value("Rows") is None

	with pytest.raises(Exception):
		df.set_value("Rows", 1024)
	with pytest.raises(Exception):
		df.set_value("PixelRepresentation", 0)

	partial_add = dicom.read_file(_test_file(), load_until=dicom.Tag("StudyTime"))
	with pytest.raises(Exception):
		partial_add.add_dataelement(dicom.Tag("Rows"), dicom.VR.US)

	partial_ensure = dicom.read_file(_test_file(), load_until=dicom.Tag("StudyTime"))
	with pytest.raises(Exception):
		partial_ensure.ensure_dataelement("Rows", dicom.VR.US)


def test_partial_load_ensure_loaded_advances_frontier_for_file_and_dataset():
	expected = dicom.read_file(_test_file())
	df = dicom.read_file(_test_file(), load_until=dicom.Tag("StudyTime"))

	assert df.get_value("Rows") is None
	df.ensure_loaded("Rows")
	assert df.get_value("Rows") == expected.get_value("Rows")
	assert df["Rows"].value == expected["Rows"].value

	assert df.get_value("Columns") is None
	df.dataset.ensure_loaded(dicom.Tag("Columns"))
	assert df.get_value("Columns") == expected.get_value("Columns")
	assert df.dataset["Columns"].value == expected.dataset["Columns"].value


def test_attribute_assignment_preserves_dicom_errors():
	ds = dicom.DataSet()
	with pytest.raises((RuntimeError, ValueError)) as excinfo:
		ds.Rows = -1
	assert "Failed to assign value to Rows (US)" in str(excinfo.value)

	df = dicom.DicomFile()
	with pytest.raises((RuntimeError, ValueError)) as excinfo:
		df.Rows = -1
	assert "Failed to assign value to Rows (US)" in str(excinfo.value)


def test_keyword_attribute_access_returns_none_for_missing_elements():
	ds = dicom.DataSet()
	assert ds.Rows is None
	with pytest.raises(AttributeError):
		_ = ds.NotARealKeyword

	df = dicom.DicomFile()
	assert df.Rows is None
	with pytest.raises(AttributeError):
		_ = df.NotARealKeyword


def test_out_of_order_add_dataelement_keeps_sorted_iteration_and_lookup():
	ds = dicom.DataSet()
	ds.add_dataelement(dicom.Tag("Rows"), dicom.VR.US).value = 512
	ds.add_dataelement(dicom.Tag("Columns"), dicom.VR.US).value = 256

	private_mid = ds.add_dataelement(dicom.Tag.from_value(0x00091030), dicom.VR.US)
	private_mid.value = 16

	private_late = ds.add_dataelement(dicom.Tag.from_value(0x00091031), dicom.VR.LO)
	assert private_late.length == 0

	assert [int(elem.tag) for elem in ds] == [
		0x00091030,
		0x00091031,
		int(dicom.Tag("Rows")),
		int(dicom.Tag("Columns")),
	]

	ds.remove_dataelement(0x00091030)
	assert not ds.get_dataelement(0x00091030)
	assert ds.get_dataelement(0x00091031).length == 0


def test_nested_path_add_ensure_and_set_value():
	ds = dicom.DataSet()

	ensured = ds.ensure_dataelement(
		"ReferencedStudySequence.0.ReferencedSOPInstanceUID", dicom.VR.UI
	)
	assert ensured
	assert ensured.vr == dicom.VR.UI

	assert ds.set_value("ReferencedStudySequence.0.ReferencedSOPInstanceUID", "1.2.3.4")
	assert ds.get_value("ReferencedStudySequence.0.ReferencedSOPInstanceUID") == "1.2.3.4"

	reset_leaf = ds.add_dataelement(
		"ReferencedStudySequence.0.ReferencedSOPInstanceUID", dicom.VR.LO
	)
	assert reset_leaf is ensured
	assert reset_leaf.vr == dicom.VR.LO

	ds.add_dataelement(dicom.Tag("Rows"), dicom.VR.US)
	nested_under_existing = ds.ensure_dataelement("Rows.0.Columns", dicom.VR.US)
	assert nested_under_existing
	assert nested_under_existing.vr == dicom.VR.US
	assert ds["Rows"].is_sequence


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


def test_dicomfile_indexing_returns_dataelement():
	df = dicom.read_file(_test_file())
	elem = df["Rows"]
	assert isinstance(elem, dicom.DataElement)
	assert elem.value == df.Rows
	assert df.get_value("Rows") == df.Rows


def test_get_dataelement_truthiness_hides_sentinel():
	df = dicom.read_file(_test_file())
	present = df.dataset.get_dataelement("PatientName")
	assert bool(present) is True
	assert present.is_present() is True
	assert present.is_missing() is False

	missing = df.dataset.get_dataelement("NotARealKeyword")
	assert bool(missing) is False
	assert missing.is_present() is False
	assert missing.is_missing() is True
	assert missing.vr == getattr(dicom.VR, "None")

	indexed = df.dataset["PatientName"]
	assert bool(indexed) is True
	assert indexed.value is not None

	indexed_missing = df.dataset["NotARealKeyword"]
	assert bool(indexed_missing) is False
	assert indexed_missing.is_missing() is True
	assert indexed_missing.value is None


def test_contains_probes_dataset_and_file_presence():
	df = dicom.read_file(_test_file())
	ds = df.dataset

	assert "Rows" in ds
	assert dicom.Tag("Rows") in ds
	assert 0x00280010 in ds
	assert "NotARealKeyword" not in ds
	assert "0008,GGGG" not in ds

	assert "Rows" in df
	assert dicom.Tag("Rows") in df
	assert 0x00280010 in df
	assert "NotARealKeyword" not in df


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


def test_write_roundtrip_sequence_and_pixel(tmp_path):
	source_bytes = _build_sequence_pixel_sample()
	source = dicom.read_bytes(source_bytes, name="seq-pixel-src")

	seq = source.get_dataelement("ReferencedStudySequence")
	assert seq.is_sequence
	assert seq.sequence is not None
	assert len(seq.sequence) == 1
	pixel = source.get_dataelement("PixelData")
	assert pixel.is_pixel_sequence
	assert pixel.pixel_sequence is not None
	assert pixel.pixel_sequence.number_of_frames == 1

	out_bytes = source.write_bytes()
	assert out_bytes[128:132] == b"DICM"
	roundtrip = dicom.read_bytes(out_bytes, name="seq-pixel-roundtrip")
	assert roundtrip.get_dataelement("ReferencedStudySequence").is_sequence
	rt_pixel = roundtrip.get_dataelement("PixelData")
	assert rt_pixel.is_pixel_sequence
	assert rt_pixel.pixel_sequence is not None
	assert rt_pixel.pixel_sequence.number_of_frames == 1

	out_path = tmp_path / "roundtrip_write.dcm"
	roundtrip.write_file(out_path)
	from_file = dicom.read_file(out_path)
	assert from_file.get_dataelement("ReferencedStudySequence").is_sequence
	assert from_file.get_dataelement("PixelData").is_pixel_sequence


def test_selected_read_python_api_with_dataset_selection_keeps_selected_tags_only():
	source_bytes = _build_sequence_pixel_sample()
	selection = dicom.DataSetSelection(
	    [
	        "SOPInstanceUID",
	        ("ReferencedStudySequence", ["ReferencedSOPInstanceUID"]),
	    ]
	)

	selected = dicom.read_bytes_selected(source_bytes, selection, name="selected-seq")
	assert selected.transfer_syntax_uid.keyword == "ExplicitVRLittleEndian"
	assert selected["TransferSyntaxUID"].to_transfer_syntax_uid().keyword == "ExplicitVRLittleEndian"
	assert selected["SOPInstanceUID"].to_uid_string() == "1.2.3.4.5.6.7.8.9"
	assert not selected["PixelData"]

	seq = selected["ReferencedStudySequence"]
	assert seq.is_sequence
	assert seq.sequence is not None
	assert len(seq.sequence) == 1
	item = seq.sequence[0]
	assert item["ReferencedSOPInstanceUID"].to_uid_string() == "1.2.3.4.5.6"
	assert not item["StudyInstanceUID"]

	with pytest.raises(Exception):
		dicom.read_bytes_selected(
		    _build_malformed_sample(),
		    dicom.DataSetSelection(["PatientName"]),
		    name="malformed-selected-default",
		)

	partial = dicom.read_bytes_selected(
	    _build_malformed_sample(),
	    dicom.DataSetSelection(["PatientName"]),
	    name="malformed-selected-keep",
	    keep_on_error=True,
	)
	assert partial.has_error
	assert partial.error_message


def test_selected_read_python_api_accepts_raw_selection_nodes_for_one_shot_reads():
	source_bytes = _build_sequence_pixel_sample()

	selected = dicom.read_bytes_selected(
	    source_bytes,
	    [
	        "SOPInstanceUID",
	        ("ReferencedStudySequence", ["ReferencedSOPInstanceUID"]),
	    ],
	    name="selected-seq-raw",
	)

	assert selected.transfer_syntax_uid.keyword == "ExplicitVRLittleEndian"
	assert selected["TransferSyntaxUID"].to_transfer_syntax_uid().keyword == "ExplicitVRLittleEndian"
	assert selected["SOPInstanceUID"].to_uid_string() == "1.2.3.4.5.6.7.8.9"
	assert not selected["PixelData"]

	seq = selected["ReferencedStudySequence"]
	assert seq.is_sequence
	assert seq.sequence is not None
	assert len(seq.sequence) == 1
	item = seq.sequence[0]
	assert item["ReferencedSOPInstanceUID"].to_uid_string() == "1.2.3.4.5.6"
	assert not item["StudyInstanceUID"]


def test_selected_read_preserves_metadata_needed_for_viewer_folder_scan():
	selection = dicom.DataSetSelection(
	    [
	        "StudyDescription",
	        "SeriesDescription",
	        "StudyDate",
	        "PatientID",
	        "PatientName",
	        "Modality",
	        "Rows",
	        "Columns",
	        "BitsAllocated",
	        "NumberOfFrames",
	    ]
	)
	selected = dicom.read_file_selected(_test_file(), selection)

	assert selected["TransferSyntaxUID"].to_transfer_syntax_uid().keyword == "ExplicitVRLittleEndian"
	assert selected["SpecificCharacterSet"].to_string_view() == "ISO_IR 100"
	assert selected["StudyDescription"].to_utf8_string(errors="replace_fffd") == "SAMPLE FOR TEST"
	assert selected["StudyDate"].to_string_view() == "20060103"
	assert selected["PatientName"].to_utf8_string(errors="replace_fffd") == "SAMPLENAME"
	assert selected["Modality"].to_string_view() == "CT"
	assert selected["Rows"].to_long() == 4
	assert selected["Columns"].to_long() == 4
	assert selected["BitsAllocated"].to_long() == 16
	assert selected["NumberOfFrames"].to_long() is None
	assert not selected["PixelData"]


def test_write_with_transfer_syntax_roundtrip_without_mutating_source(tmp_path):
	supported = {
		uid.keyword or uid.value for uid in dicom.transfer_syntax_uids_encode_supported()
	}
	if "RLELossless" not in supported:
		pytest.skip("RLELossless encoder is not available in this build")

	df = dicom.read_file(_test_file())
	baseline_ts = df.transfer_syntax_uid.keyword or df.transfer_syntax_uid.value
	baseline_frame = df.pixel_data(0)

	out_path = tmp_path / "write_with_transfer_syntax_rle.dcm"
	df.write_with_transfer_syntax(out_path, "RLELossless", options="rle")

	roundtrip = dicom.read_file(out_path)
	assert roundtrip.transfer_syntax_uid.keyword == "RLELossless"
	assert roundtrip.get_dataelement("PixelData").is_pixel_sequence
	assert roundtrip.pixel_data(0) == baseline_frame
	assert (df.transfer_syntax_uid.keyword or df.transfer_syntax_uid.value) == baseline_ts
	assert df.pixel_data(0) == baseline_frame

	ctx_path = tmp_path / "write_with_transfer_syntax_rle_ctx.dcm"
	ctx = dicom.create_encoder_context("RLELossless")
	df.write_with_transfer_syntax(ctx_path, "RLELossless", encoder_context=ctx)

	roundtrip_ctx = dicom.read_file(ctx_path)
	assert roundtrip_ctx.transfer_syntax_uid.keyword == "RLELossless"
	assert roundtrip_ctx.get_dataelement("PixelData").is_pixel_sequence
	assert roundtrip_ctx.pixel_data(0) == baseline_frame


def test_binary_memoryviews_keep_dataset_owner_alive():
	df = dicom.read_file(_test_file())
	base_refcount = sys.getrefcount(df)

	pixel_fast = df.get_value("PixelData")
	assert isinstance(pixel_fast, memoryview)
	assert pixel_fast.obj is not None
	assert sys.getrefcount(df) == base_refcount + 1
	del pixel_fast
	assert sys.getrefcount(df) == base_refcount

	pixel_attr = df.PixelData
	assert isinstance(pixel_attr, memoryview)
	assert pixel_attr.obj is not None
	assert sys.getrefcount(df) == base_refcount + 1
	del pixel_attr
	assert sys.getrefcount(df) == base_refcount

	pixel_elem = df["PixelData"]
	elem_refcount = sys.getrefcount(pixel_elem)
	pixel_value = pixel_elem.value
	assert isinstance(pixel_value, memoryview)
	assert pixel_value.obj is not None
	assert sys.getrefcount(pixel_elem) == elem_refcount + 1
	del pixel_value
	assert sys.getrefcount(pixel_elem) == elem_refcount

	raw_span = pixel_elem.value_span()
	assert isinstance(raw_span, memoryview)
	assert raw_span.obj is not None
	assert sys.getrefcount(pixel_elem) == elem_refcount + 1
	assert pixel_elem.value_bytes() == bytes(raw_span)


def test_set_transfer_syntax_encapsulated_to_encapsulated_cycle():
	df = dicom.read_file(_test_file())
	baseline_frame = df.pixel_data(0)

	df.set_transfer_syntax("RLELossless")
	assert df.transfer_syntax_uid.keyword == "RLELossless"
	assert df.get_dataelement("PixelData").is_pixel_sequence
	assert df.pixel_data(0) == baseline_frame

	df.set_transfer_syntax("JPEG2000Lossless")
	assert df.transfer_syntax_uid.keyword == "JPEG2000Lossless"
	assert df.get_dataelement("PixelData").is_pixel_sequence
	assert df.pixel_data(0) == baseline_frame

	df.set_transfer_syntax("JPEGLSLossless")
	assert df.transfer_syntax_uid.keyword == "JPEGLSLossless"
	assert df.get_dataelement("PixelData").is_pixel_sequence
	assert df.pixel_data(0) == baseline_frame


def test_encoder_context_reuse_for_set_transfer_syntax():
	df = dicom.read_file(_test_file())
	baseline_frame = df.pixel_data(0)

	ctx = dicom.create_encoder_context("RLELossless")
	assert ctx.configured is True
	assert ctx.transfer_syntax_uid.keyword == "RLELossless"

	df.set_transfer_syntax("RLELossless", encoder_context=ctx)
	assert df.transfer_syntax_uid.keyword == "RLELossless"
	assert df.get_dataelement("PixelData").is_pixel_sequence
	assert df.pixel_data(0) == baseline_frame

	ctx.configure("JPEG2000Lossless")
	assert ctx.transfer_syntax_uid.keyword == "JPEG2000Lossless"
	df.set_transfer_syntax("JPEG2000Lossless", encoder_context=ctx)
	assert df.transfer_syntax_uid.keyword == "JPEG2000Lossless"
	assert df.get_dataelement("PixelData").is_pixel_sequence
	assert df.pixel_data(0) == baseline_frame


def test_encoder_context_supports_with_statement():
	df = dicom.read_file(_test_file())
	baseline_frame = df.pixel_data(0)

	with dicom.create_encoder_context("RLELossless") as ctx:
		assert ctx.configured is True
		assert ctx.transfer_syntax_uid.keyword == "RLELossless"
		df.set_transfer_syntax("RLELossless", encoder_context=ctx)

	assert df.transfer_syntax_uid.keyword == "RLELossless"
	assert df.get_dataelement("PixelData").is_pixel_sequence
	assert df.pixel_data(0) == baseline_frame


def test_set_transfer_syntax_options_keyword():
	df = dicom.read_file(_test_file())
	baseline_frame = df.pixel_data(0)

	df.set_transfer_syntax("RLELossless", options="rle")
	assert df.transfer_syntax_uid.keyword == "RLELossless"
	assert df.get_dataelement("PixelData").is_pixel_sequence
	assert df.pixel_data(0) == baseline_frame

	ctx = dicom.create_encoder_context("JPEG2000Lossless", options="j2k")
	df.set_transfer_syntax("JPEG2000Lossless", encoder_context=ctx)
	assert df.transfer_syntax_uid.keyword == "JPEG2000Lossless"
	assert df.get_dataelement("PixelData").is_pixel_sequence
	assert df.pixel_data(0) == baseline_frame


def test_set_transfer_syntax_rejects_invalid_options_type():
	df = dicom.read_file(_test_file())
	with pytest.raises(TypeError, match="options must be None, str, or dict"):
		df.set_transfer_syntax("RLELossless", options=123)


def test_set_transfer_syntax_rejects_unknown_option_key():
	df = dicom.read_file(_test_file())
	with pytest.raises(ValueError, match="options has unknown key"):
		df.set_transfer_syntax("RLELossless", options={"foo": 1})


def test_set_transfer_syntax_rejects_incompatible_option_type():
	df = dicom.read_file(_test_file())
	with pytest.raises(ValueError, match="incompatible with transfer syntax"):
		df.set_transfer_syntax("RLELossless", options="j2k")


def test_set_transfer_syntax_requires_configured_encoder_context():
	df = dicom.read_file(_test_file())
	ctx = dicom.EncoderContext()
	with pytest.raises(RuntimeError, match="encoder context is not configured"):
		df.set_transfer_syntax("RLELossless", encoder_context=ctx)


def test_set_transfer_syntax_rejects_encoder_context_transfer_syntax_mismatch():
	df = dicom.read_file(_test_file())
	ctx = dicom.create_encoder_context("RLELossless")
	with pytest.raises(RuntimeError, match="encoder context transfer syntax mismatch"):
		df.set_transfer_syntax("ExplicitVRLittleEndian", encoder_context=ctx)
