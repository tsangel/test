#include <cassert>
#include <array>
#include <string>
#include <vector>
#include <iostream>
#include <cmath>

#include <dicom.h>

using namespace dicom::literals;

namespace {
using dicom::Tag;
using dicom::DataSet;

[[noreturn]] void fail(const std::string& msg) {
	std::cerr << msg << std::endl;
	std::exit(1);
}

template <typename T>
void require_equal(const T& actual, const T& expected, const std::string& context) {
	if (actual != expected) {
		fail(context + " expected=" + std::to_string(expected) + " actual=" + std::to_string(actual));
	}
}

void check_scalar(DataSet& ds, Tag tag, long expected_long, long long expected_ll) {
	auto* de = ds.get_dataelement(tag);
	if (!de || de == dicom::NullElement()) fail("scalar: missing tag");
	auto v = de->to_long();
	if (!v) fail("scalar: to_long returned null");
	require_equal(*v, expected_long, "scalar long");
	auto ll = de->to_longlong();
	if (!ll) fail("scalar: to_longlong returned null");
	require_equal(*ll, expected_ll, "scalar longlong");
}

void check_vector(DataSet& ds, Tag tag, const std::vector<long>& expected) {
	auto* de = ds.get_dataelement(tag);
	if (!de || de == dicom::NullElement()) fail("vector: missing tag");
	auto vec = de->to_long_vector();
	if (!vec) {
		fail("vector: to_long_vector returned null vr=" + std::string(de->vr().str())
		     + " len=" + std::to_string(de->length()) + " vm=" + std::to_string(de->vm()));
	}
	if (vec->size() != expected.size()) fail("vector: size mismatch tag=" + std::to_string(tag.value()) +
	    " expected=" + std::to_string(expected.size()) + " actual=" + std::to_string(vec->size()));
	if (!std::equal(vec->begin(), vec->end(), expected.begin())) fail("vector: contents mismatch tag=" + std::to_string(tag.value()));
}

void check_double(DataSet& ds, Tag tag, double expected) {
	auto* de = ds.get_dataelement(tag);
	if (!de || de == dicom::NullElement()) fail("double: missing tag");
	auto v = de->to_double();
	if (!v) fail("double: to_double returned null");
	if (std::fabs(*v - expected) >= 1e-6) fail("double: value mismatch");
}

void check_double_vec(DataSet& ds, Tag tag, const std::vector<double>& expected) {
	auto* de = ds.get_dataelement(tag);
	if (!de || de == dicom::NullElement()) fail("double_vec: missing tag");
	auto vec = de->to_double_vector();
	if (!vec) fail("double_vec: to_double_vector returned null");
	if (vec->size() != expected.size()) fail("double_vec: size mismatch");
	for (std::size_t i = 0; i < expected.size(); ++i) {
		if (std::fabs((*vec)[i] - expected[i]) >= 1e-6) {
			fail("double_vec: value mismatch at index " + std::to_string(i));
		}
	}
}

} // namespace

int main() {
    auto ds = dicom::read_file("tests/test_le.dcm");
    assert(ds);

    // Scalars
    check_scalar(*ds, Tag(0x0009, 0x1070), 337, 337);
    check_scalar(*ds, Tag(0x0009, 0x1071), 337, 337);
    check_scalar(*ds, Tag(0x0009, 0x1072), 337, 337);
    check_scalar(*ds, Tag(0x0009, 0x1073), 337, 337);
    check_scalar(*ds, Tag(0x0009, 0x1074), 337, 337);
    check_scalar(*ds, Tag(0x0009, 0x1075), 337, 337);

    // Vectors
    check_vector(*ds, Tag(0x0009, 0x1076), {337, -338, 339, -340});
    check_vector(*ds, Tag(0x0009, 0x1077), {337, -338, 339, -340});
    check_vector(*ds, Tag(0x0009, 0x1078), {337, -338, 339, -340});
    check_vector(*ds, Tag(0x0009, 0x1079), {337, 338, 339, 340});
    check_vector(*ds, Tag(0x0009, 0x107A), {337, 338, 339, 340});
    check_vector(*ds, Tag(0x0009, 0x107B), {337, 338, 339, 340});

    // Floating/decimal strings
    check_double(*ds, Tag(0x0009, 0x1007), 12.34);
    check_double_vec(*ds, Tag(0x0009, 0x1008), {1.2, 3.4, 5.6, 7.8, 9.0});
    check_double(*ds, Tag(0x0009, 0x1010), 12.3400002);
    check_double_vec(*ds, Tag(0x0009, 0x1011), {1.20000005, 3.4000001, 5.5999999, 7.80000019, 9.0});
    check_double(*ds, Tag(0x0009, 0x1012), 12.34);
	check_double_vec(*ds, Tag(0x0009, 0x1013), {1.2, 3.4, 5.6, 7.8, 9.0});

	// Integer-on-strings
	check_scalar(*ds, Tag(0x0009, 0x1014), 12345, 12345);
	check_vector(*ds, Tag(0x0009, 0x1015), {12345, 67890, 98765, 43210});

	// AT tags
	if (auto t = ds->get_dataelement(Tag(0x0009, 0x1004))->to_tag()) {
		assert(t->group() == 0x0009 && t->element() == 0x1001);
	} else {
		assert(false && "AT single tag missing");
	}
	if (auto tv = ds->get_dataelement(Tag(0x0009, 0x1005))->to_tag_vector()) {
		assert(tv->size() == 4);
		std::array<Tag,4> expected{Tag(0x0009,0x1001), Tag(0x0009,0x1002), Tag(0x0009,0x1003), Tag(0x0009,0x1004)};
		for (size_t i=0;i<4;++i) assert((*tv)[i] == expected[i]);
	} else {
		assert(false && "AT vector missing");
	}

	return 0;
}
