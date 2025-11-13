#include <cstring>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <pybind11/operators.h>
#include <pybind11/pybind11.h>

#include <dicom.h>

namespace py = pybind11;

using dicom::DataSet;
using dicom::Tag;
using dicom::VR;

namespace {

std::string tag_repr(const Tag& tag) {
	std::ostringstream oss;
	oss << "Tag(group=0x" << std::hex << std::uppercase << tag.group()
	    << ", element=0x" << std::hex << std::uppercase << tag.element() << ")";
	return oss.str();
}

std::string vr_repr(const VR& vr) {
	std::ostringstream oss;
	oss << "VR('" << vr.first() << vr.second() << "')";
	return oss.str();
}

std::string_view vr_to_string_view(const VR& vr) {
	return vr.str();
}

}  // namespace

PYBIND11_MODULE(_dicomsdl, m) {
	m.doc() = "pybind11 bindings for DataSet";

	py::class_<DataSet, std::unique_ptr<DataSet>>(m, "DataSet")
		.def_property_readonly("path", &DataSet::path, "Return the stored file path");

	m.def("read_file", &dicom::read_file, py::arg("path"),
	    "Read a DICOM file from disk and return a DataSet");

	m.def("read_bytes",
	    [] (py::buffer buffer, const std::string& name) {
	        py::buffer_info info = buffer.request();
	        if (info.ndim != 1) {
	            throw std::invalid_argument("read_bytes expects a 1-D bytes-like object");
	        }
	        const std::size_t elem_size = static_cast<std::size_t>(info.itemsize);
	        const std::size_t count = static_cast<std::size_t>(info.size);
	        const std::size_t total = elem_size * count;
	        std::vector<std::uint8_t> owned(total);
	        if (total > 0) {
	            std::memcpy(owned.data(), info.ptr, total);
	        }
	        return dicom::read_bytes(std::string{name}, std::move(owned));
	    },
	    py::arg("data"),
	    py::arg("name") = std::string{"<memory>"},
	    "Read a DataSet from a bytes-like object");

	py::class_<Tag>(m, "Tag")
		.def(py::init<>())
		.def(py::init<std::uint16_t, std::uint16_t>(), py::arg("group"), py::arg("element"))
		.def(py::init([](const std::string& keyword) { return Tag(keyword); }), py::arg("keyword"))
		.def_static("from_value", &Tag::from_value, py::arg("value"))
		.def_property_readonly("group", &Tag::group)
		.def_property_readonly("element", &Tag::element)
		.def_property_readonly("value", &Tag::value)
		.def("is_private", &Tag::is_private)
		.def("__int__", &Tag::value)
		.def("__bool__", [](const Tag& tag) { return static_cast<bool>(tag); })
		.def("__repr__", &tag_repr)
		.def(py::self == py::self);

	py::class_<VR>(m, "VR")
		.def(py::init<>())
		.def(py::init<std::uint16_t>(), py::arg("value"))
		.def_static("from_string", &VR::from_string, py::arg("value"))
		.def_static("from_chars", [](char a, char b) { return VR::from_chars(a, b); },
		         py::arg("first"), py::arg("second"))
		.def_property_readonly("value", [] (const VR& vr) { return static_cast<std::uint16_t>(vr); })
		.def_property_readonly("is_known", &VR::is_known)
		.def("is_string", &VR::is_string)
		.def("is_binary", &VR::is_binary)
		.def("is_sequence", &VR::is_sequence)
		.def("padding_byte", &VR::padding_byte)
		.def("uses_explicit_32bit_vl", &VR::uses_explicit_32bit_vl)
		.def("fixed_length", &VR::fixed_length)
		.def("str", [] (const VR& vr) { return std::string(vr_to_string_view(vr)); })
		.def("first", &VR::first)
		.def("second", &VR::second)
		.def("__str__", [] (const VR& vr) { return std::string(vr_to_string_view(vr)); })
		.def("__repr__", &vr_repr)
		.def(py::self == py::self);

	m.def("keyword_to_tag_vr",
	    [] (const std::string& keyword) -> py::object {
	        auto [tag, vr] = dicom::lookup::keyword_to_tag_vr(keyword);
	        if (!static_cast<bool>(tag)) {
	            return py::none();
	        }
	        return py::make_tuple(tag, vr);
	    },
	    py::arg("keyword"),
	    "Return (Tag, VR) for the provided DICOM keyword or None if missing.");

	m.def("tag_to_keyword",
	    [] (const Tag& tag) -> py::object {
	        const auto keyword = dicom::lookup::tag_to_keyword(tag.value());
	        if (keyword.empty()) {
	            return py::none();
	        }
	        return py::str(keyword);
	    },
	    py::arg("tag"),
	    "Return the DICOM keyword for this Tag or None if missing.");

	m.def("tag_to_keyword",
	    [] (std::uint32_t tag_value) -> py::object {
	        const auto keyword = dicom::lookup::tag_to_keyword(tag_value);
	        if (keyword.empty()) {
	            return py::none();
	        }
	        return py::str(keyword);
	    },
	    py::arg("tag_value"),
	    "Return the DICOM keyword for a 32-bit tag value or None if missing.");
}
