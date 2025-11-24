#include <cstring>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <dicom.h>
#include <diagnostics.h>

namespace py = pybind11;

using dicom::DataSet;
using dicom::DataElement;
using dicom::Sequence;
using dicom::Tag;
using dicom::VR;
using dicom::uid::WellKnown;
using Uid = dicom::uid::WellKnown;
namespace diag = dicom::diag;

namespace {

std::string_view vr_to_string_view(const VR& vr);

py::object dataelement_get_value_py(DataElement& element) {
	if (&element == dicom::NullElement()) {
		return py::none();
	}
	if (element.vr().is_sequence()) {
		return py::cast(element.as_sequence(), py::return_value_policy::reference_internal);
	}
	if (element.vr().is_pixel_sequence()) {
		return py::cast(element.as_pixel_sequence(), py::return_value_policy::reference_internal);
	}

	const int vm = element.vm();
	if (vm <= 1) {
		if (auto v = element.to_longlong()) {
			return py::cast(*v);
		}
		if (auto v = element.to_double()) {
			return py::cast(*v);
		}
		if (auto v = element.to_string_view()) {
			return py::str(v->data(), v->size());
		}
	} else {
		if (auto v = element.to_longlong_vector()) {
			return py::cast(*v);
		}
		if (auto v = element.to_double_vector()) {
			return py::cast(*v);
		}
		if (auto v = element.to_string_views()) {
			py::list out;
			for (const auto& sv : *v) {
				out.append(py::str(sv.data(), sv.size()));
			}
			return out;
		}
	}

	auto span = element.value_span();
	return py::memoryview::from_memory(
	    static_cast<const void*>(span.data()),
	    static_cast<ssize_t>(span.size()));
}

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

std::string uid_repr(const WellKnown& uid) {
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

py::object uid_or_none(std::optional<WellKnown> uid) {
	if (!uid) {
		return py::none();
	}
	return py::cast(*uid);
}


WellKnown require_uid(std::optional<WellKnown> uid, const char* origin, const std::string& text) {
	if (!uid) {
		std::ostringstream oss;
		oss << "Unknown DICOM UID from " << origin << ": " << text;
		throw py::value_error(oss.str());
	}
	return *uid;
}

py::dict make_tag_entry_dict(const dicom::DataElementEntry& entry) {
	py::dict info;
	info["tag"] = py::str(entry.tag.data(), entry.tag.size());
	info["keyword"] = py::str(entry.keyword.data(), entry.keyword.size());
	info["name"] = py::str(entry.name.data(), entry.name.size());
	info["vr"] = py::str(entry.vr.data(), entry.vr.size());
	info["vm"] = py::str(entry.vm.data(), entry.vm.size());
	info["retired"] = py::str(entry.retired.data(), entry.retired.size());
	info["tag_value"] = entry.tag_value;
	info["vr_value"] = entry.vr_value;
	return info;
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

struct PySequenceIterator {
	explicit PySequenceIterator(Sequence& sequence)
	    : sequence_(&sequence), index_(0) {}

	DataSet& next() {
		if (!sequence_ || index_ >= static_cast<std::size_t>(sequence_->size())) {
			throw py::stop_iteration();
		}
		DataSet* dataset = sequence_->get_dataset(index_++);
		if (!dataset) {
			throw py::stop_iteration();
		}
		return *dataset;
	}

	Sequence* sequence_;
	std::size_t index_;
};

}  // namespace

PYBIND11_MODULE(_dicomsdl, m) {
	m.doc() = "pybind11 bindings for DataSet";

	m.attr("DICOM_STANDARD_VERSION") = py::str(DICOM_STANDARD_VERSION);
	m.attr("DICOMSDL_VERSION") = py::str(DICOMSDL_VERSION);
	m.attr("__version__") = py::str(DICOMSDL_VERSION);

	// Logging helpers: forward to diag default reporter
	m.def("log_info", [] (const std::string& msg) {
		diag::info(msg);
	});
	m.def("log_warn", [] (const std::string& msg) {
		diag::warn(msg);
	});
	m.def("log_error", [] (const std::string& msg) {
		diag::error(msg);
	});

	// Diagnostics / reporter bindings
	auto loglevel = py::enum_<diag::LogLevel>(m, "LogLevel")
	    .value("Info", diag::LogLevel::Info)
	    .value("Warning", diag::LogLevel::Warning)
	    .value("Error", diag::LogLevel::Error)
	    .export_values();

	py::class_<diag::Reporter, std::shared_ptr<diag::Reporter>>(m, "Reporter");

	py::class_<diag::StderrReporter, diag::Reporter, std::shared_ptr<diag::StderrReporter>>(
	    m, "StderrReporter")
			.def(py::init<>(), "Reporter that writes to stderr");

	py::class_<diag::FileReporter, diag::Reporter, std::shared_ptr<diag::FileReporter>>(
	    m, "FileReporter")
			.def(py::init<std::string>(), py::arg("path"),
			    "Append log lines to the given file path");

	py::class_<diag::BufferingReporter, diag::Reporter, std::shared_ptr<diag::BufferingReporter>>(
	    m, "BufferingReporter")
			.def(py::init<std::size_t>(), py::arg("max_messages") = 0,
			    "Buffer messages in memory; 0 means unbounded, otherwise acts as a ring buffer")
			.def("take_messages", &diag::BufferingReporter::take_messages,
			    py::arg("include_level") = true,
			    "Return buffered messages as strings and clear the buffer")
			.def("for_each",
			    [] (diag::BufferingReporter& self, py::function fn) {
				    self.for_each([&fn](diag::LogLevel sev, const std::string& msg) {
					    fn(sev, msg);
				    });
			    },
			    py::arg("fn"),
			    "Iterate over buffered messages without clearing; fn(severity, message)");

	m.def("set_default_reporter", &diag::set_default_reporter, py::arg("reporter"),
	    "Install a process-wide reporter (None resets to stderr)");
	m.def("set_thread_reporter", &diag::set_thread_reporter, py::arg("reporter"),
	    "Install a reporter for the current thread (None clears it)");
	m.def("set_log_level", &diag::set_log_level, py::arg("level"),
	    "Set process-wide log level; messages below this are dropped");

	py::class_<DataElement>(m, "DataElement")
		.def_property_readonly("tag", &DataElement::tag)
		.def_property_readonly("vr", &DataElement::vr)
		.def_property_readonly("length", &DataElement::length)
		.def_property_readonly("offset", &DataElement::offset)
		.def_property_readonly("vm", &DataElement::vm)
		.def_property_readonly("is_sequence",
	    [](const DataElement& element) { return element.vr().is_sequence(); })
		.def_property_readonly("is_pixel_sequence",
	    [](const DataElement& element) { return element.vr().is_pixel_sequence(); })
		.def_property_readonly("sequence",
	    [](DataElement& element) -> Sequence* {
		    return element.sequence();
	    },
	    py::return_value_policy::reference_internal,
	    "Return the nested Sequence if present; otherwise None.")
		.def_property_readonly("pixel_sequence",
	    [](DataElement& element) -> dicom::PixelSequence* {
		    return element.pixel_sequence();
	    },
	    py::return_value_policy::reference_internal,
	    "Return the nested PixelSequence if present; otherwise None.")
		.def("to_uid_string",
	    [](const DataElement& element) -> py::object {
	        auto v = element.to_uid_string();
	        if (v) {
	            return py::str(*v);
	        }
	        return py::none();
	    },
	    "Return the trimmed UI string value or None if unavailable.")
		.def("to_string_view",
	    [](const DataElement& element) -> py::object {
	        auto v = element.to_string_view();
	        if (v) {
	            return py::str(v->data(), v->size());
	        }
	        return py::none();
	    },
	    "Return a trimmed raw string (no charset decoding) or None if VR is not textual.")
		.def("to_string_views",
	    [](const DataElement& element) -> py::object {
	        auto values = element.to_string_views();
	        if (!values) {
	            return py::none();
	        }
	        py::list out;
	        for (const auto& item : *values) {
	            out.append(py::str(item.data(), item.size()));
	        }
	        return out;
	    },
	    "Return a list of trimmed raw strings for multi-valued VRs, or None if unsupported.")
		.def("to_utf8_view",
	    [](const DataElement& element) -> py::object {
	        auto v = element.to_utf8_view();
	        if (v) {
	            return py::str(v->data(), v->size());
	        }
	        return py::none();
	    },
	    "Return a charset-decoded UTF-8 string when available, else None.")
		.def("to_utf8_views",
	    [](const DataElement& element) -> py::object {
	        auto values = element.to_utf8_views();
	        if (!values) {
	            return py::none();
	        }
	        py::list out;
	        for (const auto& item : *values) {
	            out.append(py::str(item.data(), item.size()));
	        }
	        return out;
	    },
	    "Return a list of UTF-8 strings for multi-valued VRs, or None if unsupported.")
		.def("to_transfer_syntax_uid",
	    [](const DataElement& element) -> py::object {
	        auto uid = element.to_transfer_syntax_uid();
	        if (uid) {
	            return py::cast(*uid);
	        }
	        return py::none();
	    },
	    "Return a well-known transfer syntax UID if the element matches, else None.")
		.def("to_tag",
		    [](const DataElement& element, py::object default_value) -> py::object {
		        if (default_value.is_none()) {
		            auto v = element.to_tag();
		            return v ? py::cast(*v) : py::none();
		        }
		        return py::cast(element.toTag(default_value.cast<Tag>()));
		    },
		    py::arg("default") = py::none())
		.def("to_tag_vector",
		    [](const DataElement& element) -> py::object {
		        auto v = element.to_tag_vector();
		        return v ? py::cast(*v) : py::none();
		    })
		.def("to_long",
		    [](const DataElement& element, py::object default_value) -> py::object {
		        if (default_value.is_none()) {
		            auto v = element.to_long();
		            return v ? py::cast(*v) : py::none();
			    }
			    return py::cast(element.toLong(default_value.cast<long>()));
		    },
		    py::arg("default") = py::none(),
		    "Return int or None; optional default fills on failure")
		.def("to_longlong",
		    [](const DataElement& element, py::object default_value) -> py::object {
			    if (default_value.is_none()) {
				    auto v = element.to_longlong();
				    return v ? py::cast(*v) : py::none();
			    }
			    return py::cast(element.toLongLong(default_value.cast<long long>()));
		    },
		    py::arg("default") = py::none())
		.def("to_double",
		    [](const DataElement& element, py::object default_value) -> py::object {
			    if (default_value.is_none()) {
				    auto v = element.to_double();
				    return v ? py::cast(*v) : py::none();
			    }
			    return py::cast(element.toDouble(default_value.cast<double>()));
		    },
		    py::arg("default") = py::none())
		.def("to_long_vector",
		    [](const DataElement& element, py::object default_value) -> py::object {
			    if (default_value.is_none()) {
				    auto v = element.to_long_vector();
				    return v ? py::cast(*v) : py::none();
			    }
			    return py::cast(element.toLongVector(default_value.cast<std::vector<long>>()));
		    },
		    py::arg("default") = py::none())
		.def("to_longlong_vector",
		    [](const DataElement& element, py::object default_value) -> py::object {
			    if (default_value.is_none()) {
				    auto v = element.to_longlong_vector();
				    return v ? py::cast(*v) : py::none();
			    }
			    return py::cast(element.toLongLongVector(default_value.cast<std::vector<long long>>()));
		    },
		    py::arg("default") = py::none())
		.def("to_double_vector",
		    [](const DataElement& element, py::object default_value) -> py::object {
			    if (default_value.is_none()) {
				    auto v = element.to_double_vector();
				    return v ? py::cast(*v) : py::none();
			    }
			    return py::cast(element.toDoubleVector(default_value.cast<std::vector<double>>()));
		    },
		    py::arg("default") = py::none())
		.def("get_value",
		    [](DataElement& element) -> py::object {
			    return dataelement_get_value_py(element);
		    },
		    "Best-effort typed access: returns int/float/str or list based on VR/VM; "
		    "falls back to raw bytes (memoryview) for binary VRs; "
		    "returns None for NullElement or sequences/pixel sequences.")
		.def("value_span",
		    [](const DataElement& element) {
			    auto span = element.value_span();
			    return py::memoryview::from_memory(
			        static_cast<const void*>(span.data()),
			        static_cast<ssize_t>(span.size()));
		    },
		    "Return the raw value bytes as a read-only memoryview")
		.def("__repr__", &dataelement_repr);

	py::class_<PySequenceIterator>(m, "SequenceIterator")
		.def("__iter__", [](PySequenceIterator& self) -> PySequenceIterator& { return self; })
		.def("__next__",
		    [](PySequenceIterator& self) -> DataSet& { return self.next(); },
		    py::return_value_policy::reference_internal);

	py::class_<Sequence>(m, "Sequence")
		.def("__len__", &Sequence::size)
		.def("__getitem__",
		    [](Sequence& self, std::size_t index) -> DataSet& {
			    DataSet* ds = self.get_dataset(index);
			    if (!ds) {
				    throw py::index_error("Sequence index out of range");
			    }
			    return *ds;
		    },
		    py::arg("index"),
		    py::return_value_policy::reference_internal)
		.def("__iter__",
		    [](Sequence& self) {
			    return PySequenceIterator(self);
		    },
		    py::keep_alive<0, 1>(),
		    "Iterate over child DataSets in insertion order")
		.def("add_dataset",
		    [](Sequence& self) -> DataSet& {
			    DataSet* ds = self.add_dataset();
			    if (!ds) {
				    throw std::runtime_error("Failed to append DataSet to sequence");
			    }
			    return *ds;
		    },
		    py::return_value_policy::reference_internal,
		    "Append a new DataSet to the sequence and return it")
		.def("__repr__",
		    [](Sequence& self) {
			    std::ostringstream oss;
			    oss << "Sequence(len=" << self.size() << ")";
			    return oss.str();
		    });

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
		        std::size_t offset, std::size_t length) {
		        const VR resolved = vr.value_or(VR::None);
		        DataElement* element = self.add_dataelement(tag, resolved, offset, length);
		        return element ? element : dicom::NullElement();
		    },
		    py::arg("tag"), py::arg("vr") = py::none(),
		    py::arg("offset") = 0, py::arg("length") = 0,
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
		        return *self.get_dataelement(tag);
		    },
		    py::arg("tag"), py::return_value_policy::reference_internal,
		    "Return the DataElement for a tag or a VR::None NullElement sentinel if missing")
		.def("get_dataelement",
		    [](DataSet& self, std::uint32_t packed) -> DataElement& {
			    const Tag tag(packed);
			    return *self.get_dataelement(tag);
		    },
		    py::arg("packed_tag"),
		    py::return_value_policy::reference_internal,
		    "Overload: pass packed 0xGGGEEEE integer; returns NullElement if missing")
		.def("get_dataelement",
		    [](DataSet& self, const std::string& tag_str) -> DataElement& {
			    return *self.get_dataelement(tag_str);
		    },
		    py::arg("tag_str"),
		    py::return_value_policy::reference_internal,
		    "Overload: parse tag path string.\n"
		    "Supported examples:\n"
		    "  - Hex tag with/without parens: '00100010', '(0010,0010)'\n"
		    "  - Keyword: 'PatientName'\n"
		    "  - Private creator: 'gggg,xxee,CREATOR' (odd group, xx block placeholder ok)\n"
		    "    e.g., '0009,xx1e,GEMS_GENIE_1'\n"
		    "  - Nested sequences: '00082112.0.00081190' or\n"
		    "    'RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose'\n"
		    "Returns a DataElement or NullElement (VR::None) if not found; malformed paths raise.")
		.def("__getitem__",
		    [](DataSet& self, py::object key) -> py::object {
			    Tag tag;
			    if (py::isinstance<Tag>(key)) {
				    tag = key.cast<Tag>();
			    } else if (py::isinstance<py::int_>(key)) {
				    tag = Tag(key.cast<std::uint32_t>());
			    } else if (py::isinstance<py::str>(key)) {
				    try {
					    tag = Tag(key.cast<std::string>());
				    } catch (const std::exception&) {
					    throw py::key_error("Invalid tag string");
				    }
			    } else {
				    throw py::type_error("DataSet indices must be Tag, int (0xGGGEEEE), or str");
			    }

			    DataElement* el = self.get_dataelement(tag);
			    if (!el || el == dicom::NullElement()) {
				    return py::none();
			    }
			    return dataelement_get_value_py(*el);
		    },
		    py::arg("key"),
		    "Index syntax: ds[tag|packed_int|tag_str] -> element.get_value(); returns None if missing")
		.def("__getattr__",
		    [](DataSet& self, const std::string& name) -> py::object {
			    // Allow keyword-style attribute access: ds.PatientName -> get_value("PatientName")
			    if (!name.empty() && name.size() >= 2 && name[0] != '_') {
				    try {
					    Tag tag(name);
					    DataElement* el = self.get_dataelement(tag);
					    if (el && el != dicom::NullElement()) {
						    return dataelement_get_value_py(*el);
					    }
				    } catch (const std::exception&) {
					    // fall through to AttributeError
				    }
			    }
			    throw py::attribute_error(("DataSet has no attribute '" + name + "'").c_str());
		    },
		    py::arg("name"),
		    "Attribute sugar: ds.PatientName -> ds.get_dataelement('PatientName').get_value(); "
		    "raises AttributeError if no such keyword/tag or element is missing.")
		.def("__dir__",
		    [](DataSet& self) {
			    py::object self_obj = py::cast(&self, py::return_value_policy::reference);
			    py::type t = py::type::of(self_obj);
			    py::list result = py::reinterpret_steal<py::list>(PyObject_Dir(t.ptr()));  // class attrs

			    std::unordered_set<std::string> seen;
			    for (auto& item : result) {
				    seen.insert(py::cast<std::string>(item));
			    }

			    for (auto& elem : self) {
				    const auto tag = elem.tag();
				    if (tag.element() == 0) {
					    continue;  // group length
				    }
				    if (tag.group() & 0x1u) {
					    continue;  // skip private tags
				    }
				    auto kw = dicom::lookup::tag_to_keyword(tag.value());
				    if (kw.empty()) {
					    continue;
				    }
				    std::string kw_str(kw.data(), kw.size());
				    if (seen.insert(kw_str).second) {
					    result.append(py::str(kw_str));
				    }
			    }
			    return result;
		    },
		    "dir() includes standard attributes plus public data element keywords (excludes group length/private).")
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
		.def("__str__", &Tag::to_string)
		.def("__repr__", &tag_repr)
		.def(py::self == py::self);

auto uid_cls = py::class_<Uid>(m, "Uid")
	.def(py::init<>())
	.def(py::init([](const std::string& text) {
		    return require_uid(dicom::uid::lookup(text), "Uid.__init__", text);
	    }),
	    py::arg("text"),
	    "Construct a UID from either a dotted value or keyword, raising ValueError if unknown.")
	.def_static("lookup",
	    [](const std::string& text) -> py::object {
		    return uid_or_none(dicom::uid::lookup(text));
	    },
	    py::arg("text"),
	    "Lookup a UID from value or keyword; returns None if missing.")
	.def_static("from_value",
	    [](const std::string& value) {
		    return require_uid(dicom::uid::from_value(value), "Uid.from_value", value);
	    },
	    py::arg("value"),
	    "Resolve a dotted UID value, raising ValueError if unknown.")
	.def_static("from_keyword",
	    [](const std::string& keyword) {
		    return require_uid(dicom::uid::from_keyword(keyword), "Uid.from_keyword", keyword);
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
		.def("uses_explicit_16bit_vl", &VR::uses_explicit_16bit_vl)
		.def("fixed_length", &VR::fixed_length)
		.def("str", [] (const VR& vr) { return std::string(vr_to_string_view(vr)); })
		.def("__str__", [] (const VR& vr) { return std::string(vr_to_string_view(vr)); })
		.def("__repr__", &vr_repr)
		.def(py::self == py::self);

	vr_cls.attr("None") = dicom::VR::None;
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

	py::class_<dicom::PixelFragment>(m, "PixelFragment")
		.def_readonly("offset", &dicom::PixelFragment::offset, "Fragment offset relative to pixel sequence base")
		.def_readonly("length", &dicom::PixelFragment::length, "Fragment length in bytes")
		.def("__repr__",
		    [](const dicom::PixelFragment& frag) {
			    std::ostringstream oss;
			    oss << "PixelFragment(offset=0x" << std::hex << frag.offset
			        << ", length=" << std::dec << frag.length << ")";
			    return oss.str();
		    });

	py::class_<dicom::PixelFrame>(m, "PixelFrame")
		.def_property_readonly("encoded_size", &dicom::PixelFrame::encoded_data_size,
		    "Size in bytes of materialized encoded data; 0 if not loaded")
		.def_property_readonly("fragments",
		    [](const dicom::PixelFrame& f) { return f.fragments(); },
		    "Fragments belonging to this frame")
		.def("encoded_bytes",
		    [](dicom::PixelFrame& f) {
			    auto span = f.encoded_data_view();
			    return py::bytes(reinterpret_cast<const char*>(span.data()), span.size());
		    },
		    "Return encoded pixel data as bytes (coalesced if needed)")
		.def("encoded_memoryview",
		    [](dicom::PixelFrame& f) {
			    auto span = f.encoded_data_view();
			    return py::memoryview::from_memory(
			        reinterpret_cast<const char*>(span.data()), span.size());
		    },
		    "Return a read-only memoryview over encoded pixel data (no copy); "
		    "invalidated if the frame's encoded data is cleared");

	py::class_<dicom::PixelSequence>(m, "PixelSequence")
		.def_property_readonly("number_of_frames", &dicom::PixelSequence::number_of_frames)
		.def_property_readonly("basic_offset_table_offset", &dicom::PixelSequence::basic_offset_table_offset)
		.def_property_readonly("basic_offset_table_count", &dicom::PixelSequence::basic_offset_table_count)
		.def("__len__", &dicom::PixelSequence::number_of_frames)
		.def("frame",
		    [](dicom::PixelSequence& self, std::size_t index) -> dicom::PixelFrame& {
			    dicom::PixelFrame* f = self.frame(index);
			    if (!f) throw py::index_error("PixelSequence index out of range");
			    return *f;
		    },
		    py::arg("index"),
		    py::return_value_policy::reference_internal)
		.def("frame_encoded_bytes",
		    [](dicom::PixelSequence& self, std::size_t index) {
			    auto span = self.frame_encoded_span(index);
			    return py::bytes(reinterpret_cast<const char*>(span.data()), span.size());
		    },
		    py::arg("index"),
		    "Return encoded pixel data for a frame (coalesces fragments if needed)")
		.def("frame_encoded_memoryview",
		    [](dicom::PixelSequence& self, std::size_t index) {
			    auto span = self.frame_encoded_span(index);
			    return py::memoryview::from_memory(
			        reinterpret_cast<const char*>(span.data()), span.size());
		    },
		    py::arg("index"),
		    "Return a read-only memoryview over encoded pixel data for a frame (no copy)")
		.def("__repr__",
		    [](dicom::PixelSequence& self) {
			    std::ostringstream oss;
			    oss << "PixelSequence(frames=" << self.number_of_frames() << ")";
			    return oss.str();
		    });

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

	m.def("tag_to_entry",
	    [] (const Tag& tag) -> py::object {
	        if (const auto* entry = dicom::lookup::tag_to_entry(tag.value())) {
	            return make_tag_entry_dict(*entry);
	        }
	        return py::none();
	    },
	    py::arg("tag"),
	    "Return registry details for the given Tag or None if missing.");

	m.def("tag_to_entry",
	    [] (std::uint32_t tag_value) -> py::object {
	        if (const auto* entry = dicom::lookup::tag_to_entry(tag_value)) {
	            return make_tag_entry_dict(*entry);
	        }
	        return py::none();
	    },
	    py::arg("tag_value"),
	    "Return registry details for a tag numeric value or None if missing.");

m.def("lookup_uid",
    [] (const std::string& text) -> py::object {
        return uid_or_none(dicom::uid::lookup(text));
    },
    py::arg("text"),
    "Lookup a UID by either dotted value or keyword; returns None if missing.");

m.def("uid_from_value",
    [] (const std::string& value) {
        return require_uid(dicom::uid::from_value(value), "uid_from_value", value);
    },
    py::arg("value"),
    "Resolve a dotted UID value, raising ValueError if unknown.");

m.def("uid_from_keyword",
    [] (const std::string& keyword) {
        return require_uid(dicom::uid::from_keyword(keyword), "uid_from_keyword", keyword);
    },
    py::arg("keyword"),
    "Resolve a UID keyword, raising ValueError if unknown.");

	m.attr("__all__") = py::make_tuple(
	    "LogLevel",
	    "Reporter",
	    "StderrReporter",
	    "FileReporter",
	    "BufferingReporter",
	    "log_info",
	    "log_warn",
	    "log_error",
	    "set_default_reporter",
	    "set_thread_reporter",
	    "set_log_level",
	    "DataSet",
	    "Tag",
	    "VR",
	    "Uid",
	    "read_file",
	    "read_bytes",
	    "keyword_to_tag_vr",
	    "tag_to_keyword",
	    "tag_to_entry",
	    "lookup_uid",
	    "uid_from_value",
	    "uid_from_keyword");
}
