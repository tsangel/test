import dicomsdl as dicom


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
