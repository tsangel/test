import pathlib
import struct

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


def test_python_pathlike_support(tmp_path):
	source = dicom.read_file(pathlib.Path(_test_file()))
	out_path = tmp_path / "pathlike-roundtrip.dcm"
	source.write_file(out_path)
	roundtrip = dicom.read_file(out_path)
	assert pathlib.Path(roundtrip.path).resolve() == out_path.resolve()
	assert roundtrip.has_error is False

	log_path = tmp_path / "dicomsdl.log"
	reporter = dicom.FileReporter(log_path)
	dicom.set_thread_reporter(reporter)
	try:
		dicom.log_warn("python-pathlike-smoke")
	finally:
		dicom.set_thread_reporter(None)
	assert "python-pathlike-smoke" in log_path.read_text()

	with pytest.raises(ValueError):
		dicom.register_external_codec_plugin(tmp_path / "missing.plugin")


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
