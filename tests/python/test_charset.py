import array

import dicomsdl as dicom
import pytest


def test_dataelement_utf8_helpers_roundtrip():
	ds = dicom.DataSet()
	pn_elem = ds.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)
	korean_name = "\ud64d\uae38\ub3d9"

	assert pn_elem.from_string_view("DOE^JOHN")
	assert pn_elem.to_string_view() == "DOE^JOHN"
	assert pn_elem.to_utf8_string() == "DOE^JOHN"
	assert pn_elem.from_string_views(["DOE^JOHN", "SMITH^ALICE"])
	assert pn_elem.to_string_views() == ["DOE^JOHN", "SMITH^ALICE"]
	assert pn_elem.to_utf8_strings() == ["DOE^JOHN", "SMITH^ALICE"]
	assert not pn_elem.from_utf8_view(korean_name)

	df = dicom.DicomFile()
	df.set_declared_specific_charset("ISO_IR 192")
	pn_elem = df.dataset.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)
	assert pn_elem.from_utf8_view(korean_name)
	assert pn_elem.to_utf8_string() == korean_name
	assert pn_elem.to_string_view() == korean_name
	assert pn_elem.from_utf8_views([korean_name, "DOE^JOHN"])
	assert pn_elem.to_utf8_strings() == [korean_name, "DOE^JOHN"]


def test_from_utf8_errors_replacement_modes():
	korean_name = "\ud64d\uae38\ub3d9"
	df = dicom.DicomFile()
	df.set_declared_specific_charset("ISO_IR 100")
	pn_elem = df.dataset.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)

	assert pn_elem.from_utf8_view(korean_name, errors="replace_qmark")
	assert pn_elem.to_string_view() == "???"
	ok, replaced = pn_elem.from_utf8_view(
	    korean_name, errors="replace_qmark", return_replaced=True
	)
	assert ok
	assert replaced is True

	assert pn_elem.from_utf8_view(korean_name, errors="replace_unicode_escape")
	assert pn_elem.to_string_view() == "(U+D64D)(U+AE38)(U+B3D9)"


def test_raw_specific_character_set_edits_refresh_cache():
	korean_name = "\ud64d\uae38\ub3d9"
	df = dicom.DicomFile()
	cs_elem = df.dataset.add_dataelement(dicom.Tag("SpecificCharacterSet"), dicom.VR.CS)
	assert cs_elem.from_string_view("ISO_IR 192")
	pn_elem = df.dataset.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)
	assert pn_elem.from_utf8_view(korean_name)
	assert cs_elem.from_string_view("")
	other_elem = df.dataset.add_dataelement(dicom.Tag("OtherPatientNames"), dicom.VR.PN)
	assert not other_elem.from_utf8_view(korean_name)

	remove_df = dicom.DicomFile()
	remove_df.set_declared_specific_charset("ISO_IR 192")
	remove_df.dataset.remove_dataelement(dicom.Tag("SpecificCharacterSet"))
	retry_elem = remove_df.dataset.add_dataelement(dicom.Tag("PatientID"), dicom.VR.LO)
	assert not retry_elem.from_utf8_view(korean_name)

def test_write_bytes_uses_declared_specific_character_set():
	korean_name = "\ud64d\uae38\ub3d9"
	df = dicom.DicomFile()
	df.set_declared_specific_charset("ISO_IR 192")
	pn_elem = df.dataset.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)
	assert pn_elem.from_utf8_view(korean_name)

	out_bytes = df.write_bytes()
	roundtrip = dicom.read_bytes(out_bytes, name="utf8-write-py-roundtrip")
	assert roundtrip.get_dataelement("SpecificCharacterSet").to_string_view() == "ISO_IR 192"
	assert roundtrip.get_dataelement("PatientName").to_utf8_string() == korean_name


def test_write_bytes_supports_multi_term_iso2022_charset():
	japanese_name = "\uff76\uff85^\u4e9c"
	df = dicom.DicomFile()
	df.set_declared_specific_charset(["ISO 2022 IR 13", "ISO 2022 IR 87"])
	pn_elem = df.dataset.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)
	assert pn_elem.from_utf8_view(japanese_name)

	out_bytes = df.write_bytes()
	roundtrip = dicom.read_bytes(out_bytes, name="multi-term-iso2022-py-roundtrip")
	assert roundtrip.get_dataelement("SpecificCharacterSet").to_string_views() == [
	    "ISO 2022 IR 13",
	    "ISO 2022 IR 87",
	]
	assert roundtrip.get_dataelement("PatientName").length == 8
	assert roundtrip.get_dataelement("PatientName").to_utf8_string() == japanese_name


def test_write_bytes_preserves_declared_multi_term_iso2022_charset():
	japanese_name = "\uff76\uff85^\u4e9c"
	df = dicom.DicomFile()
	df.set_declared_specific_charset(["ISO 2022 IR 13", "ISO 2022 IR 87"])
	pn_elem = df.dataset.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)
	assert pn_elem.from_utf8_view(japanese_name)

	out_bytes = df.write_bytes()
	roundtrip = dicom.read_bytes(out_bytes, name="multi-term-target-iso2022-py-roundtrip")
	assert roundtrip.get_dataelement("SpecificCharacterSet").to_string_views() == [
	    "ISO 2022 IR 13",
	    "ISO 2022 IR 87",
	]
	assert roundtrip.get_dataelement("PatientName").length == 8
	assert roundtrip.get_dataelement("PatientName").to_utf8_string() == japanese_name


def test_leading_empty_iso2022_ir87_encodes_and_roundtrips_pn():
	name = "Yamada^Tarou=\u5c71\u7530^\u592a\u90ce=\u3084\u307e\u3060^\u305f\u308d\u3046"
	df = dicom.DicomFile()
	df.set_declared_specific_charset(["", "ISO 2022 IR 87"])
	pn_elem = df.dataset.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)
	assert pn_elem.from_utf8_view(name)
	assert pn_elem.to_utf8_string() == name

	out_bytes = df.write_bytes()
	roundtrip = dicom.read_bytes(out_bytes, name="leading-empty-iso2022-ir87-pn-roundtrip")
	assert roundtrip.get_dataelement("SpecificCharacterSet").to_string_views() == [
	    "",
	    "ISO 2022 IR 87",
	]
	assert roundtrip.get_dataelement("PatientName").to_utf8_string() == name


def test_write_bytes_omits_initial_iso2022_g0_designation():
	jis_name = "\u4e9c"
	df = dicom.DicomFile()
	df.set_declared_specific_charset("ISO 2022 IR 87")
	pn_elem = df.dataset.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)
	assert pn_elem.from_utf8_view(jis_name)

	out_bytes = df.write_bytes()
	roundtrip = dicom.read_bytes(out_bytes, name="iso2022-ir87-first-escape-omitted")
	assert roundtrip.get_dataelement("SpecificCharacterSet").to_string_view() == "ISO 2022 IR 87"
	assert roundtrip.get_dataelement("PatientName").length == 2
	assert roundtrip.get_dataelement("PatientName").to_utf8_string() == jis_name


def test_to_string_views_rejects_raw_iso2022_splitting():
	df = dicom.DicomFile()
	df.set_declared_specific_charset("ISO 2022 IR 87")
	lo_elem = df.dataset.add_dataelement(dicom.Tag(0x0008, 0x1030), dicom.VR.LO)
	assert lo_elem.from_string_view(b"\x21\x5c".decode("latin1"))

	assert lo_elem.to_string_views() is None
	assert lo_elem.to_utf8_string() == "\uff0b"
	assert lo_elem.to_utf8_strings() == ["\uff0b"]


def test_to_utf8_string_keeps_backslash_in_single_valued_gbk_st():
	df = dicom.DicomFile()
	df.set_declared_specific_charset("GBK")
	st_elem = df.dataset.add_dataelement(dicom.Tag(0x4000, 0x4000), dicom.VR.ST)
	assert st_elem.from_utf8_view("\u4e2d\\B")
	assert st_elem.to_utf8_string() == "\u4e2d\\B"


def test_to_utf8_string_keeps_backslash_in_single_valued_iso2022_st():
	df = dicom.DicomFile()
	df.set_declared_specific_charset("ISO 2022 IR 87")
	st_elem = df.dataset.add_dataelement(dicom.Tag(0x4000, 0x4001), dicom.VR.ST)
	assert st_elem.from_utf8_view("\u4e9c\\B")
	assert st_elem.to_utf8_string() == "\u4e9c\\B"


def test_write_bytes_omits_initial_iso2022_g1_designation():
	latin1_name = "\u00f6"
	df = dicom.DicomFile()
	df.set_declared_specific_charset("ISO 2022 IR 100")
	pn_elem = df.dataset.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)
	assert pn_elem.from_utf8_view(latin1_name)

	out_bytes = df.write_bytes()
	roundtrip = dicom.read_bytes(out_bytes, name="iso2022-ir100-first-escape-omitted")
	assert roundtrip.get_dataelement("SpecificCharacterSet").to_string_view() == "ISO 2022 IR 100"
	assert roundtrip.get_dataelement("PatientName").length == 2
	assert roundtrip.get_dataelement("PatientName").to_utf8_string() == latin1_name


def test_to_utf8_errors_replacement_modes():
	df = dicom.DicomFile()
	df.set_declared_specific_charset("ISO 2022 IR 100")
	pn_elem = df.dataset.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)
	assert pn_elem.from_string_view("\x1b%GA")

	assert pn_elem.to_utf8_string() is None
	assert pn_elem.to_utf8_string(errors="replace_fffd") == "\ufffd"
	assert pn_elem.to_utf8_string(errors="replace_hex_escape") == "(0x1B)(0x25)(0x47)(0x41)"
	value, replaced = pn_elem.to_utf8_string(errors="replace_fffd", return_replaced=True)
	assert value == "\ufffd"
	assert replaced is True


def test_to_utf8_string_stops_after_first_component():
	iso2022_df = dicom.DicomFile()
	iso2022_df.set_declared_specific_charset("ISO 2022 IR 100")
	iso2022_elem = iso2022_df.dataset.add_dataelement(
	    dicom.Tag("SeriesDescription"), dicom.VR.LO
	)
	assert iso2022_elem.from_string_view("A\\\x1b%GA")
	assert iso2022_elem.to_utf8_string() == "A"
	assert iso2022_elem.to_utf8_strings() is None


def test_set_specific_charset_errors_replacement_modes():
	korean_name = "\ud64d\uae38\ub3d9"
	strict_df = dicom.DicomFile()
	strict_df.set_declared_specific_charset("ISO_IR 192")
	strict_elem = strict_df.dataset.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)
	assert strict_elem.from_utf8_view(korean_name)

	try:
		strict_df.set_specific_charset("ISO_IR 100", errors="strict")
		assert False
	except Exception:
		pass
	assert strict_df.get_dataelement("SpecificCharacterSet").to_string_view() == "ISO_IR 192"

	qmark_df = dicom.DicomFile()
	qmark_df.set_declared_specific_charset("ISO_IR 192")
	qmark_elem = qmark_df.dataset.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)
	assert qmark_elem.from_utf8_view(korean_name)
	assert qmark_df.set_specific_charset(
	    "ISO_IR 100", errors="replace_qmark", return_replaced=True
	) is True
	assert qmark_df.get_dataelement("SpecificCharacterSet").to_string_view() == "ISO_IR 100"
	assert qmark_df.get_dataelement("PatientName").to_string_view() == "???"

	unicode_df = dicom.DicomFile()
	unicode_df.set_declared_specific_charset("ISO_IR 192")
	unicode_elem = unicode_df.dataset.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)
	assert unicode_elem.from_utf8_view(korean_name)
	unicode_df.set_specific_charset("ISO_IR 100", errors="replace_unicode_escape")
	assert unicode_df.get_dataelement("PatientName").to_string_view() == "(U+D64D)(U+AE38)(U+B3D9)"


def test_person_name_group_short_components_are_padded():
	group = dicom.PersonNameGroup(("\u5c71\u7530", "\u592a\u90ce"))
	assert group.components == ["\u5c71\u7530", "\u592a\u90ce", "", "", ""]
	assert group.family_name == "\u5c71\u7530"
	assert group.given_name == "\u592a\u90ce"
	assert group.to_dicom_string() == "\u5c71\u7530^\u592a\u90ce"


def test_person_name_api_roundtrip():
	name = "Yamada^Tarou=\u5c71\u7530^\u592a\u90ce=\u3084\u307e\u3060^\u305f\u308d\u3046"
	df = dicom.DicomFile()
	df.set_declared_specific_charset("ISO_IR 192")
	pn_elem = df.dataset.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)
	assert pn_elem.from_utf8_view(name)

	parsed = pn_elem.to_person_name()
	assert parsed is not None
	assert parsed.alphabetic is not None
	assert parsed.ideographic is not None
	assert parsed.phonetic is not None
	assert parsed.alphabetic.family_name == "Yamada"
	assert parsed.alphabetic.given_name == "Tarou"
	assert parsed.ideographic.family_name == "\u5c71\u7530"
	assert parsed.ideographic.given_name == "\u592a\u90ce"
	assert parsed.phonetic.family_name == "\u3084\u307e\u3060"
	assert parsed.phonetic.given_name == "\u305f\u308d\u3046"

	rebuilt = dicom.PersonName(
	    alphabetic=("Yamada", "Tarou"),
	    ideographic=("\u5c71\u7530", "\u592a\u90ce"),
	    phonetic=("\u3084\u307e\u3060", "\u305f\u308d\u3046"),
	)
	other = df.dataset.add_dataelement(dicom.Tag(0x0010, 0x1001), dicom.VR.PN)
	assert other.from_person_name(rebuilt)
	assert other.to_utf8_string() == name

	ideographic_only = dicom.PersonName(
	    ideographic=("\u5c71\u7530", "\u592a\u90ce"),
	    phonetic=("\u3084\u307e\u3060", "\u305f\u308d\u3046"),
	)
	assert ideographic_only.to_dicom_string() == "=\u5c71\u7530^\u592a\u90ce=\u3084\u307e\u3060^\u305f\u308d\u3046"


def test_binary_vr_assignment_accepts_matching_typed_arrays():
	ds = dicom.DataSet()

	cases = [
	    (dicom.VR.OB, array.array("B", [1, 2, 3])),
	    (dicom.VR.OD, array.array("d", [1.25, 2.5])),
	    (dicom.VR.OF, array.array("f", [1.25, 2.5])),
	    (dicom.VR.OL, array.array("I", [1, 2, 3])),
	    (dicom.VR.OW, array.array("H", [1, 2, 3])),
	    (dicom.VR.OV, array.array("Q", [1, 2, 3])),
	]

	for index, (vr, values) in enumerate(cases, start=1):
		tag = dicom.Tag(0x0011, 0x1000 + index)
		elem = ds.add_dataelement(tag, vr)
		elem.value = values
		expected = values.tobytes()
		if len(expected) % 2 != 0:
			expected += b"\x00"
		assert bytes(elem.value_span()) == expected


def test_person_name_parse_preserves_trailing_empty_group():
	name = "Wang^XiaoDong=\u738b^\u5c0f\u6771="
	df = dicom.DicomFile()
	df.set_declared_specific_charset("ISO_IR 192")
	pn_elem = df.dataset.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)
	assert pn_elem.from_utf8_view(name)

	parsed = pn_elem.to_person_name()
	assert parsed is not None
	assert parsed.to_dicom_string() == name

	other = df.dataset.add_dataelement(dicom.Tag(0x0010, 0x1002), dicom.VR.PN)
	assert other.from_person_name(parsed)
	assert other.to_utf8_string() == name


def test_person_name_api_preserves_explicit_empty_components():
	df = dicom.DicomFile()
	df.set_declared_specific_charset("ISO_IR 192")
	pn_elem = df.dataset.add_dataelement(dicom.Tag("ReferringPhysicianName"), dicom.VR.PN)
	assert pn_elem.from_string_view("^^^^")

	parsed = pn_elem.to_person_name()
	assert parsed is not None
	assert parsed.alphabetic is not None
	assert parsed.alphabetic.components == ["", "", "", "", ""]
	assert parsed.alphabetic.to_dicom_string() == "^^^^"
	assert parsed.to_dicom_string() == "^^^^"


def test_get_value_returns_person_name_for_pn():
	name = "Yamada^Tarou=\u5c71\u7530^\u592a\u90ce=\u3084\u307e\u3060^\u305f\u308d\u3046"
	df = dicom.DicomFile()
	df.set_declared_specific_charset("ISO_IR 192")
	pn_elem = df.dataset.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)
	assert pn_elem.from_utf8_view(name)

	value = pn_elem.get_value()
	assert isinstance(value, dicom.PersonName)
	assert value.alphabetic is not None
	assert value.ideographic is not None
	assert value.phonetic is not None
	assert value.alphabetic.family_name == "Yamada"
	assert df.PatientName.alphabetic.given_name == "Tarou"


def test_get_value_returns_utf8_for_charset_aware_text():
	df = dicom.DicomFile()
	df.set_declared_specific_charset("GBK")
	lo_elem = df.dataset.add_dataelement(dicom.Tag(0x0008, 0x1030), dicom.VR.LO)
	assert lo_elem.from_utf8_view("\u4e2d\u6587")

	value = lo_elem.get_value()
	assert value == "\u4e2d\u6587"
	assert df.StudyDescription == "\u4e2d\u6587"


def test_get_value_returns_bytes_when_pn_parse_or_decode_fails():
	df = dicom.DicomFile()
	df.set_declared_specific_charset("ISO 2022 IR 100")
	pn_elem = df.dataset.add_dataelement(dicom.Tag("PatientName"), dicom.VR.PN)
	assert pn_elem.from_string_view("\x1b%GA")

	value = pn_elem.get_value()
	assert isinstance(value, bytes)
	assert value == b"\x1b%GA"


def test_get_value_returns_bytes_when_charset_text_decode_fails():
	df = dicom.DicomFile()
	df.set_declared_specific_charset("ISO 2022 IR 100")
	lo_elem = df.dataset.add_dataelement(dicom.Tag(0x0008, 0x1030), dicom.VR.LO)
	assert lo_elem.from_string_view("\x1b%GA")

	value = lo_elem.get_value()
	assert isinstance(value, bytes)
	assert value == b"\x1b%GA"


def test_dataset_attribute_assignment_and_element_value_assignment_sets_numeric_values():
	ds = dicom.DataSet()
	ds.Rows = 512
	ds.add_dataelement(dicom.Tag("Columns"), dicom.VR.US).value = 256

	assert ds.Rows == 512
	assert isinstance(ds["Rows"], dicom.DataElement)
	assert ds["Rows"].value == 512
	assert ds.get_value("Rows") == 512
	assert ds.Columns == 256
	assert ds["Columns"].value == 256
	assert ds.get_value("Columns") == 256

	with pytest.raises(TypeError):
		ds["Columns"] = 128


def test_dataelement_value_property_and_set_value_update_dataset():
	ds = dicom.DataSet()
	elem = ds.add_dataelement(dicom.Tag("Rows"), dicom.VR.US)

	assert elem.set_value(512) is True
	assert elem.value == 512
	assert ds.Rows == 512
	assert ds.get_value("Rows") == 512

	elem.value = 256
	assert elem.value == 256
	assert ds.Rows == 256
	assert ds.get_value("Rows") == 256

	assert ds.set_value("Rows", 1024) is True
	assert elem.value == 1024
	assert ds.Rows == 1024
	assert ds.get_value("Rows") == 1024

	assert elem.set_value(-1) is False
	assert ds.set_value("Rows", -1) is False
	assert ds.set_value("Columns", 128) is True
	assert ds.get_value("Columns") == 128


def test_set_value_accepts_explicit_vr_for_private_creation_and_override():
	ds = dicom.DataSet()
	private_tag = 0x00090030

	assert ds.set_value(private_tag, dicom.VR.US, 16) is True
	assert ds[private_tag].vr == dicom.VR.US
	assert ds[private_tag].value == 16

	assert ds.set_value(private_tag, dicom.VR.UL, 17) is True
	assert ds[private_tag].vr == dicom.VR.UL
	assert ds[private_tag].value == 17

	seq = ds.add_dataelement(dicom.Tag("ReferencedStudySequence"), dicom.VR.SQ)
	assert ds.set_value("ReferencedStudySequence", dicom.VR.US, 1) is True
	assert ds["ReferencedStudySequence"].vr == dicom.VR.US
	assert ds["ReferencedStudySequence"].value == 1


def test_dataset_assignment_sugar_sets_text_and_person_name_values():
	df = dicom.DicomFile()
	df.set_declared_specific_charset("ISO_IR 192")

	df.StudyDescription = "테스트 설명"
	assert df.StudyDescription == "테스트 설명"

	pn = dicom.PersonName(
	    alphabetic=dicom.PersonNameGroup(("Hong", "Gildong")),
	    ideographic=dicom.PersonNameGroup(("洪", "吉洞")),
	    phonetic=dicom.PersonNameGroup(("홍", "길동")),
	)
	df.PatientName = pn
	value = df.PatientName
	assert isinstance(value, dicom.PersonName)
	assert value.alphabetic.family_name == "Hong"
	assert value.ideographic.family_name == "洪"


def test_dataset_assignment_none_creates_zero_length_element():
	ds = dicom.DataSet()
	ds.Rows = 512
	assert ds.Rows == 512
	ds.Rows = None
	assert bool(ds["Rows"]) is True
	assert ds["Rows"].length == 0
	assert ds["Rows"].value == []
	assert ds.get_value("Rows") == []

	ds.remove_dataelement("Rows")
	assert ds.get_value("Rows") is None
	assert bool(ds["Rows"]) is False
