import pathlib

import dicomsdl as dicom


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
