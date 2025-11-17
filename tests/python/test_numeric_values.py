import math

import dicomsdl as dicom


def get(ds, group, element):
	return ds.get_dataelement(dicom.Tag(group, element))


def test_numeric_scalars():
	ds = dicom.read_file("tests/test_le.dcm")
	assert get(ds, 0x0009, 0x1070).to_long() == 337
	assert get(ds, 0x0009, 0x1071).to_longlong() == 337
	assert get(ds, 0x0009, 0x1072).to_long() == 337
	assert get(ds, 0x0009, 0x1073).to_long() == 337
	assert get(ds, 0x0009, 0x1074).to_long() == 337
	assert get(ds, 0x0009, 0x1075).to_longlong() == 337


def test_numeric_vectors():
	ds = dicom.read_file("tests/test_le.dcm")
	assert get(ds, 0x0009, 0x1076).to_long_vector() == [337, -338, 339, -340]
	assert get(ds, 0x0009, 0x1077).to_long_vector() == [337, -338, 339, -340]
	assert get(ds, 0x0009, 0x1078).to_long_vector() == [337, -338, 339, -340]
	assert get(ds, 0x0009, 0x1079).to_long_vector() == [337, 338, 339, 340]
	assert get(ds, 0x0009, 0x107A).to_long_vector() == [337, 338, 339, 340]
	assert get(ds, 0x0009, 0x107B).to_long_vector() == [337, 338, 339, 340]


def test_decimal_and_float():
	ds = dicom.read_file("tests/test_le.dcm")
	assert math.isclose(get(ds, 0x0009, 0x1007).to_double(), 12.34, rel_tol=0, abs_tol=1e-6)
	assert all(
		math.isclose(a, b, rel_tol=0, abs_tol=1e-6)
		for a, b in zip(get(ds, 0x0009, 0x1008).to_double_vector(), [1.2, 3.4, 5.6, 7.8, 9.0])
	)
	assert math.isclose(get(ds, 0x0009, 0x1010).to_double(), 12.3400002, rel_tol=0, abs_tol=1e-6)
	assert math.isclose(get(ds, 0x0009, 0x1012).to_double(), 12.34, rel_tol=0, abs_tol=1e-6)
	assert all(
		math.isclose(a, b, rel_tol=0, abs_tol=1e-6)
		for a, b in zip(
			get(ds, 0x0009, 0x1013).to_double_vector(),
			[1.2, 3.4, 5.6, 7.8, 9.0],
		)
	)


def test_is_from_string():
	ds = dicom.read_file("tests/test_le.dcm")
	assert get(ds, 0x0009, 0x1014).to_long() == 12345
	assert get(ds, 0x0009, 0x1015).to_long_vector() == [12345, 67890, 98765, 43210]


def test_at_tags():
	ds = dicom.read_file("tests/test_le.dcm")
	t1 = get(ds, 0x0009, 0x1004).to_tag()
	assert t1.group == 0x0009 and t1.element == 0x1001
	tv = get(ds, 0x0009, 0x1005).to_tag_vector()
	assert len(tv) == 4
	assert [(t.group, t.element) for t in tv] == [
	    (0x0009, 0x1001),
	    (0x0009, 0x1002),
	    (0x0009, 0x1003),
	    (0x0009, 0x1004),
	]
