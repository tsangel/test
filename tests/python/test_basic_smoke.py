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
