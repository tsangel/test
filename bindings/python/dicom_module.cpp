#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <dicom.h>

namespace py = pybind11;

using dicom::DataSet;
using dicom::DataElement;
using dicom::Tag;
using dicom::Uid;
using dicom::VR;

namespace {

std::string_view vr_to_string_view(const VR& vr);

std::string tag_repr(const Tag& tag) {
	std::ostringstream oss;
	oss << "Tag(group=0x" << std::hex << std::uppercase << tag.group()
	    << ", element=0x" << std::hex << std::uppercase << tag.element() << ")";
	return oss.str();
}

std::string vr_repr(const VR& vr) {
	std::string s = std::string(vr_to_string_view(vr));
	std::ostringstream oss;
	oss << "VR('" << s << "')";
	return oss.str();
}

std::string_view vr_to_string_view(const VR& vr) {
	return vr.str();
}

std::string dataelement_repr(const DataElement& element) {
	std::ostringstream oss;
	oss << "DataElement(tag=" << tag_repr(element.tag())
	    << ", vr=" << vr_repr(element.vr())
	    << ", length=" << element.length()
	    << ", offset=" << element.offset()
	    << ")";
	return oss.str();
}

std::string uid_repr(const Uid& uid) {
	if (!uid) {
		return "Uid(<invalid>)";
	}
	std::ostringstream oss;
	oss << "Uid(value='" << uid.value() << "'";
	if (!uid.keyword().empty()) {
		oss << ", keyword='" << uid.keyword() << "'";
	}
	oss << ", type='" << uid.type() << "')";
	return oss.str();
}

py::object uid_or_none(Uid uid) {
	if (!uid) {
		return py::none();
	}
	return py::cast(uid);
}

Uid require_uid(Uid uid, const char* origin, const std::string& text) {
	if (!uid) {
		std::ostringstream oss;
		oss << "Unknown DICOM UID from " << origin << ": " << text;
		throw py::value_error(oss.str());
	}
	return uid;
}

struct PyDataElementIterator {
	explicit PyDataElementIterator(DataSet& data_set)
	    : data_set_(&data_set), current_(data_set.begin()), end_(data_set.end()) {}

	DataElement& next() {
		if (current_ == end_) {
			throw py::stop_iteration();
		}
		DataElement& element = *current_;
		++current_;
		return element;
	}

	DataSet* data_set_;
	DataSet::iterator current_;
	DataSet::iterator end_;
};

}  // namespace

PYBIND11_MODULE(_dicomsdl, m) {
	m.doc() = "pybind11 bindings for DataSet";

	m.attr("DICOM_STANDARD_VERSION") = py::str(DICOM_STANDARD_VERSION);
	m.attr("DICOMSDL_VERSION") = py::str(DICOMSDL_VERSION);
	m.attr("__version__") = py::str(DICOMSDL_VERSION);

	py::class_<DataElement>(m, "DataElement")
		.def_property_readonly("tag", &DataElement::tag)
		.def_property_readonly("vr", &DataElement::vr)
		.def_property_readonly("length", &DataElement::length)
		.def_property_readonly("offset", &DataElement::offset)
		.def_property_readonly("is_sequence",
		    [](const DataElement& element) { return element.vr().is_sequence(); })
		.def_property_readonly("is_pixel_sequence",
		    [](const DataElement& element) { return element.vr().is_pixel_sequence(); })
		.def("__repr__", &dataelement_repr);

	py::class_<PyDataElementIterator>(m, "DataElementIterator")
		.def("__iter__", [](PyDataElementIterator& self) -> PyDataElementIterator& { return self; })
		.def("__next__",
		    [](PyDataElementIterator& self) -> DataElement& { return self.next(); },
		    py::return_value_policy::reference_internal);

	py::class_<DataSet, std::unique_ptr<DataSet>>(m, "DataSet")
		.def(py::init<>())
		.def_property_readonly("path", &DataSet::path, "Return the stored file path")
		.def("add_dataelement",
		    [](DataSet& self, const Tag& tag, std::optional<VR> vr,
		        std::size_t length, std::size_t offset) {
		        const VR resolved = vr.value_or(VR::NONE);
		        DataElement* element = self.add_dataelement(tag, resolved, length, offset);
		        return element ? element : dicom::NullElement();
		    },
		    py::arg("tag"), py::arg("vr") = py::none(),
		    py::arg("length") = 0, py::arg("offset") = 0,
		    py::return_value_policy::reference_internal,
		    "Add or update a DataElement and return a reference to it")
		.def("remove_dataelement",
		    [](DataSet& self, const Tag& tag) {
		        self.remove_dataelement(tag);
		    },
		    py::arg("tag"),
		    "Remove a DataElement by tag if it exists")
		.def("dump_elements", &DataSet::dump_elements,
		    "Print internal element storage for debugging")
		.def("get_dataelement",
		    [](DataSet& self, const Tag& tag) -> DataElement& {
		        DataElement* element = self.get_dataelement(tag);
		        if (element == dicom::NullElement()) {
			        throw py::key_error("Tag not found in DataSet");
		        }
		        return *element;
		    },
		    py::arg("tag"), py::return_value_policy::reference_internal)
		.def("__iter__",
		    [](DataSet& self) {
			    return PyDataElementIterator(self);
		    },
		    py::keep_alive<0, 1>(), "Iterate over DataElements in tag order");

m.def("read_file",
    [](const std::string& path, std::optional<Tag> load_until, std::optional<bool> keep_on_error) {
	    dicom::ReadOptions opts;
	    if (load_until) {
		    opts.load_until = *load_until;
	    }
	    if (keep_on_error) {
		    opts.keep_on_error = *keep_on_error;
	    }
	    return dicom::read_file(path, opts);
    },
    py::arg("path"),
    py::arg("load_until") = py::none(),
    py::arg("keep_on_error") = py::none(),
    "Read a DICOM file from disk and return a DataSet");

m.def("read_bytes",
    [] (py::buffer buffer, const std::string& name, std::optional<Tag> load_until,
        std::optional<bool> keep_on_error, bool copy) {
        py::buffer_info info = buffer.request();
        if (info.ndim != 1) {
            throw std::invalid_argument("read_bytes expects a 1-D bytes-like object");
        }
        const std::size_t elem_size = static_cast<std::size_t>(info.itemsize);
        const std::size_t count = static_cast<std::size_t>(info.size);
        const std::size_t total = elem_size * count;
        dicom::ReadOptions opts;
        if (load_until) {
	        opts.load_until = *load_until;
        }
        if (keep_on_error) {
	        opts.keep_on_error = *keep_on_error;
        }
        opts.copy = copy;

        std::unique_ptr<dicom::DataSet> dataset;
        if (copy || total == 0) {
	        std::vector<std::uint8_t> owned(total);
	        if (total > 0) {
		        std::memcpy(owned.data(), info.ptr, total);
	        }
	        dataset = dicom::read_bytes(std::string{name}, std::move(owned), opts);
        } else {
	        if (elem_size != 1) {
		        throw std::invalid_argument("read_bytes(copy=False) requires a byte-oriented buffer");
	        }
	        dataset = dicom::read_bytes(std::string{name},
	            static_cast<const std::uint8_t*>(info.ptr), total, opts);
        }

        py::object py_dataset = py::cast(std::move(dataset));
        if (!copy && total > 0) {
	        py_dataset.attr("_buffer_owner") = buffer;
        }
        return py_dataset;
    },
    py::arg("data"),
    py::arg("name") = std::string{"<memory>"},
    py::arg("load_until") = py::none(),
    py::arg("keep_on_error") = py::none(),
    py::arg("copy") = true,
    "Read a DataSet from a bytes-like object.\n\n"
    "Warning: When copy=False, the source buffer must remain alive for as long as the returned "
    "DataSet; the binding keeps a reference to the Python object internally, but mutating or "
    "freeing the underlying memory can still corrupt the dataset.");

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

	auto uid_cls = py::class_<Uid>(m, "Uid")
		.def(py::init<>())
		.def(py::init([](const std::string& text) { return Uid(text); }), py::arg("text"),
		    "Construct a UID from either a dotted value or keyword, raising ValueError if unknown.")
		.def_static("lookup",
		    [](const std::string& text) -> py::object {
			    return uid_or_none(Uid::lookup(text));
		    },
		    py::arg("text"),
		    "Lookup a UID from value or keyword; returns None if missing.")
		.def_static("from_value",
		    [](const std::string& value) {
			    return require_uid(Uid::from_value(value), "Uid.from_value", value);
		    },
		    py::arg("value"),
		    "Resolve a dotted UID value, raising ValueError if unknown.")
		.def_static("from_keyword",
		    [](const std::string& keyword) {
			    return require_uid(Uid::from_keyword(keyword), "Uid.from_keyword", keyword);
		    },
		    py::arg("keyword"),
		    "Resolve a UID keyword, raising ValueError if unknown.")
		.def_property_readonly("value",
		    [](const Uid& uid) { return std::string(uid.value()); },
		    "Return the dotted UID value or empty string if invalid.")
		.def_property_readonly("keyword",
		    [](const Uid& uid) -> py::object {
			    if (uid.keyword().empty()) {
				    return py::none();
			    }
			    return py::str(uid.keyword());
		    },
		    "Return the UID keyword or None if missing.")
		.def_property_readonly("name",
		    [](const Uid& uid) { return std::string(uid.name()); },
		    "Return the descriptive UID name or empty string if invalid.")
		.def_property_readonly("type",
		    [](const Uid& uid) { return std::string(uid.type()); },
		    "Return the UID type (Transfer Syntax, SOP Class, ...).")
		.def_property_readonly("raw_index", &Uid::raw_index, "Return the registry index.")
		.def_property_readonly("is_valid", &Uid::valid)
		.def("__bool__", [](const Uid& uid) { return uid.valid(); })
		.def("__repr__", &uid_repr)
		.def(py::self == py::self);

	auto vr_cls = py::class_<VR>(m, "VR")
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
		.def("is_pixel_sequence", &VR::is_pixel_sequence)
		.def("padding_byte", &VR::padding_byte)
		.def("uses_explicit_32bit_vl", &VR::uses_explicit_32bit_vl)
		.def("fixed_length", &VR::fixed_length)
		.def("str", [] (const VR& vr) { return std::string(vr_to_string_view(vr)); })
		.def("__str__", [] (const VR& vr) { return std::string(vr_to_string_view(vr)); })
		.def("__repr__", &vr_repr)
		.def(py::self == py::self);

	vr_cls.attr("NONE") = dicom::VR::NONE;
	vr_cls.attr("AE") = dicom::VR::AE;
	vr_cls.attr("AS") = dicom::VR::AS;
	vr_cls.attr("AT") = dicom::VR::AT;
	vr_cls.attr("CS") = dicom::VR::CS;
	vr_cls.attr("DA") = dicom::VR::DA;
	vr_cls.attr("DS") = dicom::VR::DS;
	vr_cls.attr("DT") = dicom::VR::DT;
	vr_cls.attr("FD") = dicom::VR::FD;
	vr_cls.attr("FL") = dicom::VR::FL;
	vr_cls.attr("IS") = dicom::VR::IS;
	vr_cls.attr("LO") = dicom::VR::LO;
	vr_cls.attr("LT") = dicom::VR::LT;
	vr_cls.attr("OB") = dicom::VR::OB;
	vr_cls.attr("OD") = dicom::VR::OD;
	vr_cls.attr("OF") = dicom::VR::OF;
	vr_cls.attr("OV") = dicom::VR::OV;
	vr_cls.attr("OL") = dicom::VR::OL;
	vr_cls.attr("OW") = dicom::VR::OW;
	vr_cls.attr("PN") = dicom::VR::PN;
	vr_cls.attr("SH") = dicom::VR::SH;
	vr_cls.attr("SL") = dicom::VR::SL;
	vr_cls.attr("SQ") = dicom::VR::SQ;
	vr_cls.attr("SS") = dicom::VR::SS;
	vr_cls.attr("ST") = dicom::VR::ST;
	vr_cls.attr("SV") = dicom::VR::SV;
	vr_cls.attr("TM") = dicom::VR::TM;
	vr_cls.attr("UC") = dicom::VR::UC;
	vr_cls.attr("UI") = dicom::VR::UI;
	vr_cls.attr("UL") = dicom::VR::UL;
	vr_cls.attr("UN") = dicom::VR::UN;
	vr_cls.attr("UR") = dicom::VR::UR;
	vr_cls.attr("US") = dicom::VR::US;
	vr_cls.attr("UT") = dicom::VR::UT;
	vr_cls.attr("UV") = dicom::VR::UV;
	vr_cls.attr("PX") = dicom::VR::PX;

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

	m.def("lookup_uid",
	    [] (const std::string& text) -> py::object {
	        return uid_or_none(Uid::lookup(text));
	    },
	    py::arg("text"),
	    "Lookup a UID by either dotted value or keyword; returns None if missing.");

	m.def("uid_from_value",
	    [] (const std::string& value) {
	        return require_uid(Uid::from_value(value), "uid_from_value", value);
	    },
	    py::arg("value"),
	    "Resolve a dotted UID value, raising ValueError if unknown.");

	m.def("uid_from_keyword",
	    [] (const std::string& keyword) {
	        return require_uid(Uid::from_keyword(keyword), "uid_from_keyword", keyword);
	    },
	    py::arg("keyword"),
	    "Resolve a UID keyword, raising ValueError if unknown.");

	m.attr("__all__") = py::make_tuple(
	    "DataSet",
	    "Tag",
	    "VR",
	    "Uid",
	    "read_file",
	    "read_bytes",
	    "keyword_to_tag_vr",
	    "tag_to_keyword",
	    "lookup_uid",
	    "uid_from_value",
	    "uid_from_keyword");
}
