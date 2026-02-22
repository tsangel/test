#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <dicom.h>

namespace {

[[noreturn]] void fail(const std::string& msg) {
	std::cerr << msg << std::endl;
	std::exit(1);
}

void require(bool cond, const std::string& msg) {
	if (!cond) {
		fail(msg);
	}
}

std::vector<std::uint32_t> collect_tags(const dicom::DataSet& ds) {
	std::vector<std::uint32_t> tags;
	for (const auto& elem : ds) {
		tags.push_back(elem.tag().value());
	}
	return tags;
}

void require_order(const dicom::DataSet& ds, const std::vector<std::uint32_t>& expected,
    const std::string& context) {
	const auto tags = collect_tags(ds);
	if (tags != expected) {
		fail(context + ": iterator order mismatch");
	}
	if (ds.size() != expected.size()) {
		fail(context + ": DataSet::size mismatch");
	}
}

void require_missing(dicom::DataSet& ds, dicom::Tag tag, const std::string& context) {
	auto* elem = ds.get_dataelement(tag);
	require(elem->is_missing(), context + ": tag must be missing");
}

void add_elem(dicom::DataSet& ds, std::uint32_t tag_value,
    std::size_t offset = 0, std::size_t length = 0) {
	auto* elem = ds.add_dataelement(
	    dicom::Tag::from_value(tag_value), dicom::VR::LO, offset, length);
	require(elem != nullptr && *elem,
	    "add_dataelement returned null/NullElement");
}

}  // namespace

int main() {
	constexpr std::uint32_t kTag10 = 0x00100010u;
	constexpr std::uint32_t kTag20 = 0x00100020u;
	constexpr std::uint32_t kTag25 = 0x00100025u;
	constexpr std::uint32_t kTag30 = 0x00100030u;
	constexpr std::uint32_t kTag40 = 0x00100040u;

	// Case 1) vector middle remove: tombstone must be skipped by iterator/size.
	{
		dicom::DataSet ds;
		add_elem(ds, kTag10);
		add_elem(ds, kTag20);
		add_elem(ds, kTag30);
		add_elem(ds, kTag40);
		require_order(ds, {kTag10, kTag20, kTag30, kTag40}, "case1 baseline");

		ds.remove_dataelement(dicom::Tag::from_value(kTag20));
		require_order(ds, {kTag10, kTag30, kTag40}, "case1 remove-middle");
		require_missing(ds, dicom::Tag::from_value(kTag20), "case1 remove-middle");
	}

	// Case 2) vector middle remove -> re-add same tag: must restore original order.
	{
		dicom::DataSet ds;
		add_elem(ds, kTag10);
		add_elem(ds, kTag20);
		add_elem(ds, kTag30);
		add_elem(ds, kTag40);

		ds.remove_dataelement(dicom::Tag::from_value(kTag20));
		add_elem(ds, kTag20, 123, 7);
		require_order(ds, {kTag10, kTag20, kTag30, kTag40}, "case2 readd-middle");
		auto* elem = ds.get_dataelement(dicom::Tag::from_value(kTag20));
		require(elem->is_present() && elem->offset() == 123 && elem->length() == 7,
		    "case2 readd-middle payload mismatch");
	}

	// Case 3) vector front remove -> re-add same tag.
	{
		dicom::DataSet ds;
		add_elem(ds, kTag10);
		add_elem(ds, kTag20);
		add_elem(ds, kTag30);
		add_elem(ds, kTag40);

		ds.remove_dataelement(dicom::Tag::from_value(kTag10));
		require_order(ds, {kTag20, kTag30, kTag40}, "case3 remove-front");
		add_elem(ds, kTag10);
		require_order(ds, {kTag10, kTag20, kTag30, kTag40}, "case3 readd-front");
	}

	// Case 4) vector back remove -> re-add same tag.
	{
		dicom::DataSet ds;
		add_elem(ds, kTag10);
		add_elem(ds, kTag20);
		add_elem(ds, kTag30);
		add_elem(ds, kTag40);

		ds.remove_dataelement(dicom::Tag::from_value(kTag40));
		require_order(ds, {kTag10, kTag20, kTag30}, "case4 remove-back");
		add_elem(ds, kTag40);
		require_order(ds, {kTag10, kTag20, kTag30, kTag40}, "case4 readd-back");
	}

	// Case 5) map insert (out-of-order add): iterator must merge in sorted order.
	{
		dicom::DataSet ds;
		add_elem(ds, kTag10);
		add_elem(ds, kTag30);
		add_elem(ds, kTag40);
		add_elem(ds, kTag20);  // out-of-order => map storage
		require_order(ds, {kTag10, kTag20, kTag30, kTag40}, "case5 map-insert-order");
	}

	// Case 6) map remove -> re-add same tag: order must remain stable.
	{
		dicom::DataSet ds;
		add_elem(ds, kTag10);
		add_elem(ds, kTag30);
		add_elem(ds, kTag40);
		add_elem(ds, kTag20);  // map
		ds.remove_dataelement(dicom::Tag::from_value(kTag20));  // erase map entry
		require_order(ds, {kTag10, kTag30, kTag40}, "case6 map-remove");
		require_missing(ds, dicom::Tag::from_value(kTag20), "case6 map-remove");

		add_elem(ds, kTag20, 999, 11);
		require_order(ds, {kTag10, kTag20, kTag30, kTag40}, "case6 map-readd");
		auto* elem = ds.get_dataelement(dicom::Tag::from_value(kTag20));
		require(elem->is_present() && elem->offset() == 999 && elem->length() == 11,
		    "case6 map-readd payload mismatch");
	}

	// Case 7) vector tombstone + map coexistence:
	//         middle remove, map insert, then middle re-add must keep global sort.
	{
		dicom::DataSet ds;
		add_elem(ds, kTag10);
		add_elem(ds, kTag20);
		add_elem(ds, kTag30);
		add_elem(ds, kTag40);

		ds.remove_dataelement(dicom::Tag::from_value(kTag20));  // vector tombstone
		add_elem(ds, kTag25);  // map insert
		require_order(ds, {kTag10, kTag25, kTag30, kTag40}, "case7 tombstone+map");

		add_elem(ds, kTag20);  // revive tombstone slot
		require_order(ds, {kTag10, kTag20, kTag25, kTag30, kTag40}, "case7 revive-middle");
	}

	return 0;
}
