import pathlib
import re

import dicomsdl as dicom



def iter_entries(root: pathlib.Path):
	# Parse the generated header; skip retired entries or ones without a concrete 2-letter VR.
	pattern = re.compile(
		r'DataElementEntry\{0x([0-9A-Fa-f]+)u,\s*\d+u,\s*"\([^\"]+\)",'  # tag value
		 r'\s*"[^\"]+",\s*"([^\"]+)",'  # keyword
		 r'\s*"([^\"]+)",'  # VR
		 r'\s*"[^\"]+",\s*"([^\"]*)"\}'  # retired
	)
	text = (root / "include" / "dataelement_registry.hpp").read_text()
	for match in pattern.finditer(text):
		tag_hex, keyword, vr, retired = match.groups()
		if retired:
			continue
		if len(vr) != 2 or not vr.isalpha():
			continue
		yield int(tag_hex, 16), keyword


def test_dictionary_roundtrip():
	root = pathlib.Path(__file__).resolve().parents[2]
	for tag_value, keyword in iter_entries(root):
		result = dicom.keyword_to_tag_vr(keyword)
		assert result is not None, f"keyword {keyword} missing from lookup"
		tag, vr = result
		assert int(tag) == tag_value, keyword
		assert dicom.tag_to_keyword(tag_value) == keyword
		assert vr.str() != "??"
