#include <array>
#include <cstring>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <nanobind/operators.h>
#include <nanobind/nanobind.h>
#include <nanobind/ndarray.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/stl/unique_ptr.h>
#include <nanobind/stl/vector.h>

#include <dicom.h>
#include <diagnostics.h>

namespace nb = nanobind;

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

nb::object readonly_memoryview_from_span(const void* data, std::size_t size) {
	char* ptr = size == 0
	                ? const_cast<char*>("")
	                : const_cast<char*>(reinterpret_cast<const char*>(data));
	return nb::steal<nb::object>(
	    PyMemoryView_FromMemory(ptr, static_cast<Py_ssize_t>(size), PyBUF_READ));
}

struct DecodedArraySpec {
	nb::dlpack::dtype dtype{};
	std::size_t bytes_per_sample{0};
};

DecodedArraySpec decoded_array_spec(const DataSet::pixel_info_t& info, bool scaled) {
	if (scaled) {
		return DecodedArraySpec{nb::dtype<float>(), sizeof(float)};
	}

	switch (info.sv_dtype) {
	case dicom::pixel::dtype::u8:
		return DecodedArraySpec{nb::dtype<std::uint8_t>(), sizeof(std::uint8_t)};
	case dicom::pixel::dtype::s8:
		return DecodedArraySpec{nb::dtype<std::int8_t>(), sizeof(std::int8_t)};
	case dicom::pixel::dtype::u16:
		return DecodedArraySpec{nb::dtype<std::uint16_t>(), sizeof(std::uint16_t)};
	case dicom::pixel::dtype::s16:
		return DecodedArraySpec{nb::dtype<std::int16_t>(), sizeof(std::int16_t)};
	case dicom::pixel::dtype::u32:
		return DecodedArraySpec{nb::dtype<std::uint32_t>(), sizeof(std::uint32_t)};
	case dicom::pixel::dtype::s32:
		return DecodedArraySpec{nb::dtype<std::int32_t>(), sizeof(std::int32_t)};
	case dicom::pixel::dtype::f32:
		return DecodedArraySpec{nb::dtype<float>(), sizeof(float)};
	case dicom::pixel::dtype::f64:
		return DecodedArraySpec{nb::dtype<double>(), sizeof(double)};
	default:
		break;
	}

	throw nb::value_error("pixel_array requires a known pixel sample dtype");
}

nb::object make_numpy_array_from_decoded(std::vector<std::uint8_t>&& decoded,
    std::size_t ndim, const std::array<std::size_t, 4>& shape,
    const std::array<std::int64_t, 4>& strides, const nb::dlpack::dtype& dtype) {
	auto* storage = new std::vector<std::uint8_t>(std::move(decoded));
	void* data_ptr = storage->empty() ? nullptr : static_cast<void*>(storage->data());
	nb::capsule owner(storage, [](void* ptr) noexcept {
		delete static_cast<std::vector<std::uint8_t>*>(ptr);
	});
	return nb::cast(nb::ndarray<nb::numpy>(
	    data_ptr, ndim, shape.data(), owner, strides.data(), dtype,
	    nb::device::cpu::value, 0, 'C'));
}

nb::object dataset_pixel_array(const DataSet& self, long frame, bool scaled) {
	if (frame < -1) {
		throw nb::value_error("frame must be >= -1");
	}

	const auto& info = self.pixel_info();
	if (!info.has_pixel_data) {
		throw nb::value_error(
		    "pixel_array requires PixelData, FloatPixelData, or DoubleFloatPixelData");
	}
	if (info.rows <= 0 || info.cols <= 0 || info.samples_per_pixel <= 0) {
		throw nb::value_error("pixel_array requires positive Rows/Columns/SamplesPerPixel");
	}
	if (info.frames <= 0) {
		throw nb::value_error("pixel_array requires NumberOfFrames >= 1");
	}

	const auto rows = static_cast<std::size_t>(info.rows);
	const auto cols = static_cast<std::size_t>(info.cols);
	const auto samples_per_pixel = static_cast<std::size_t>(info.samples_per_pixel);
	const auto frames = static_cast<std::size_t>(info.frames);

	dicom::pixel::decode_opts opt{};
	opt.planar_out = dicom::pixel::planar::interleaved;
	opt.alignment = 1;
	opt.scaled = scaled;

	const auto dst_strides = self.calc_strides(opt);
	const auto spec = decoded_array_spec(info, scaled);
	const auto row_stride = dst_strides.row;
	const auto frame_stride = dst_strides.frame;
	const auto bytes_per_sample = spec.bytes_per_sample;
	if (bytes_per_sample == 0) {
		throw nb::value_error("pixel_array could not determine output sample size");
	}
	if ((row_stride % bytes_per_sample) != 0 || (frame_stride % bytes_per_sample) != 0) {
		throw std::runtime_error("pixel_array stride is not aligned to output sample size");
	}
	const auto row_stride_elems = row_stride / bytes_per_sample;
	const auto frame_stride_elems = frame_stride / bytes_per_sample;
	const auto col_stride_elems = samples_per_pixel;

	const bool decode_all_frames = (frame == -1) && (frames > 1);
	if (!decode_all_frames) {
		const auto frame_index = (frame < 0) ? std::size_t{0} : static_cast<std::size_t>(frame);
		if (frame_index >= frames) {
			throw nb::index_error("pixel_array frame index out of range");
		}

		std::vector<std::uint8_t> decoded(frame_stride);
		self.decode_into(frame_index, std::span<std::uint8_t>(decoded), dst_strides, opt);

		std::array<std::size_t, 4> shape{};
		std::array<std::int64_t, 4> strides{};
		std::size_t ndim = 0;
		if (samples_per_pixel == 1) {
			ndim = 2;
			shape[0] = rows;
			shape[1] = cols;
			strides[0] = static_cast<std::int64_t>(row_stride_elems);
			strides[1] = 1;
		} else {
			ndim = 3;
			shape[0] = rows;
			shape[1] = cols;
			shape[2] = samples_per_pixel;
			strides[0] = static_cast<std::int64_t>(row_stride_elems);
			strides[1] = static_cast<std::int64_t>(col_stride_elems);
			strides[2] = 1;
		}

		return make_numpy_array_from_decoded(
		    std::move(decoded), ndim, shape, strides, spec.dtype);
	}

	std::vector<std::uint8_t> decoded(frame_stride * frames);
	for (std::size_t frame_index = 0; frame_index < frames; ++frame_index) {
		auto frame_span = std::span<std::uint8_t>(
		    decoded.data() + frame_index * frame_stride, frame_stride);
		self.decode_into(frame_index, frame_span, dst_strides, opt);
	}

	std::array<std::size_t, 4> shape{};
	std::array<std::int64_t, 4> strides{};
	std::size_t ndim = 0;
	if (samples_per_pixel == 1) {
		ndim = 3;
		shape[0] = frames;
		shape[1] = rows;
		shape[2] = cols;
		strides[0] = static_cast<std::int64_t>(frame_stride_elems);
		strides[1] = static_cast<std::int64_t>(row_stride_elems);
		strides[2] = 1;
	} else {
		ndim = 4;
		shape[0] = frames;
		shape[1] = rows;
		shape[2] = cols;
		shape[3] = samples_per_pixel;
		strides[0] = static_cast<std::int64_t>(frame_stride_elems);
		strides[1] = static_cast<std::int64_t>(row_stride_elems);
		strides[2] = static_cast<std::int64_t>(col_stride_elems);
		strides[3] = 1;
	}

	return make_numpy_array_from_decoded(
	    std::move(decoded), ndim, shape, strides, spec.dtype);
}

class PyBufferView {
public:
	explicit PyBufferView(nb::handle obj) {
		if (PyObject_GetBuffer(obj.ptr(), &view_, PyBUF_FULL_RO) != 0) {
			throw nb::type_error("read_bytes expects a bytes-like object");
		}
	}

	~PyBufferView() { PyBuffer_Release(&view_); }

	PyBufferView(const PyBufferView&) = delete;
	PyBufferView& operator=(const PyBufferView&) = delete;

	const Py_buffer& view() const { return view_; }

private:
	Py_buffer view_{};
};

nb::object dataelement_get_value_py(DataElement& element, nb::handle parent = nb::handle()) {
	if (&element == dicom::NullElement()) {
		return nb::none();
	}
	if (element.vr().is_sequence()) {
		auto* seq = element.as_sequence();
		if (!seq) return nb::none();
		nb::handle keep = parent.is_none() ? nb::cast(element.parent(), nb::rv_policy::reference) : parent;
		return nb::cast(seq, nb::rv_policy::reference_internal, keep);
	}
	if (element.vr().is_pixel_sequence()) {
		auto* pix = element.as_pixel_sequence();
		if (!pix) return nb::none();
		nb::handle keep = parent.is_none() ? nb::cast(element.parent(), nb::rv_policy::reference) : parent;
		return nb::cast(pix, nb::rv_policy::reference_internal, keep);
	}

	const int vm = element.vm();
	if (vm <= 1) {
		if (auto v = element.to_longlong()) {
			return nb::cast(*v);
		}
		if (auto v = element.to_double()) {
			return nb::cast(*v);
		}
		if (auto v = element.to_string_view()) {
			return nb::str(v->data(), v->size());
		}
	} else {
		if (auto v = element.to_longlong_vector()) {
			return nb::cast(*v);
		}
		if (auto v = element.to_double_vector()) {
			return nb::cast(*v);
		}
		if (auto v = element.to_string_views()) {
			nb::list out;
			for (const auto& sv : *v) {
				out.append(nb::str(sv.data(), sv.size()));
			}
			return out;
		}
	}

	auto span = element.value_span();
	return readonly_memoryview_from_span(span.data(), span.size());
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

nb::object uid_or_none(std::optional<WellKnown> uid) {
	if (!uid) {
		return nb::none();
	}
	return nb::cast(*uid);
}


WellKnown require_uid(std::optional<WellKnown> uid, const char* origin, const std::string& text) {
	if (!uid) {
		std::ostringstream oss;
		oss << "Unknown DICOM UID from " << origin << ": " << text;
		const std::string message = oss.str();
		throw nb::value_error(message.c_str());
	}
	return *uid;
}

nb::dict make_tag_entry_dict(const dicom::DataElementEntry& entry) {
	nb::dict info;
	info["tag"] = nb::str(entry.tag.data(), entry.tag.size());
	info["keyword"] = nb::str(entry.keyword.data(), entry.keyword.size());
	info["name"] = nb::str(entry.name.data(), entry.name.size());
	info["vr"] = nb::str(entry.vr.data(), entry.vr.size());
	info["vm"] = nb::str(entry.vm.data(), entry.vm.size());
	info["retired"] = nb::str(entry.retired.data(), entry.retired.size());
	info["tag_value"] = entry.tag_value;
	info["vr_value"] = entry.vr_value;
	return info;
}

struct PyDataElementIterator {
	explicit PyDataElementIterator(DataSet& data_set)
	    : data_set_(&data_set), current_(data_set.begin()), end_(data_set.end()) {}

	DataElement& next() {
		if (current_ == end_) {
			throw nb::stop_iteration();
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
			throw nb::stop_iteration();
		}
		DataSet* dataset = sequence_->get_dataset(index_++);
		if (!dataset) {
			throw nb::stop_iteration();
		}
		return *dataset;
	}

	Sequence* sequence_;
	std::size_t index_;
};

}  // namespace

NB_MODULE(_dicomsdl, m) {
	m.doc() = "nanobind bindings for DataSet";

	m.attr("DICOM_STANDARD_VERSION") = nb::str(DICOM_STANDARD_VERSION);
	m.attr("DICOMSDL_VERSION") = nb::str(DICOMSDL_VERSION);
	m.attr("__version__") = nb::str(DICOMSDL_VERSION);

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
	nb::enum_<diag::LogLevel>(m, "LogLevel")
	    .value("Info", diag::LogLevel::Info)
	    .value("Warning", diag::LogLevel::Warning)
	    .value("Error", diag::LogLevel::Error);

	nb::class_<diag::Reporter>(m, "Reporter");

	nb::class_<diag::StderrReporter, diag::Reporter>(
	    m, "StderrReporter")
			.def(nb::init<>(), "Reporter that writes to stderr");

	nb::class_<diag::FileReporter, diag::Reporter>(
	    m, "FileReporter")
			.def(nb::init<std::string>(), nb::arg("path"),
			    "Append log lines to the given file path");

	nb::class_<diag::BufferingReporter, diag::Reporter>(
	    m, "BufferingReporter")
			.def(nb::init<std::size_t>(), nb::arg("max_messages") = 0,
			    "Buffer messages in memory; 0 means unbounded, otherwise acts as a ring buffer")
			.def("take_messages", &diag::BufferingReporter::take_messages,
			    nb::arg("include_level") = true,
			    "Return buffered messages as strings and clear the buffer")
			.def("for_each",
			    [] (diag::BufferingReporter& self, nb::callable fn) {
				    self.for_each([&fn](diag::LogLevel sev, const std::string& msg) {
					    fn(sev, msg);
				    });
			    },
			    nb::arg("fn"),
			    "Iterate over buffered messages without clearing; fn(severity, message)");

	m.def("set_default_reporter", &diag::set_default_reporter, nb::arg("reporter").none(),
	    "Install a process-wide reporter (None resets to stderr)");
	m.def("set_thread_reporter", &diag::set_thread_reporter, nb::arg("reporter").none(),
	    "Install a reporter for the current thread (None clears it)");
	m.def("set_log_level", &diag::set_log_level, nb::arg("level"),
	    "Set process-wide log level; messages below this are dropped");

	nb::class_<DataElement>(m, "DataElement",
	    "Single DICOM element. Provides tag/VR/length/offset and typed value helpers.\n"
	    "For sequences and pixel data, holds nested Sequence or PixelSequence objects.")
		.def_prop_ro("tag", &DataElement::tag)
		.def_prop_ro("vr", &DataElement::vr)
		.def_prop_ro("length", &DataElement::length)
		.def_prop_ro("offset", &DataElement::offset)
		.def_prop_ro("vm", &DataElement::vm)
		.def_prop_ro("is_sequence",
	    [](const DataElement& element) { return element.vr().is_sequence(); })
		.def_prop_ro("is_pixel_sequence",
	    [](const DataElement& element) { return element.vr().is_pixel_sequence(); })
		.def_prop_ro("sequence",
	    [](DataElement& element) -> Sequence* {
		    return element.sequence();
	    },
	    nb::rv_policy::reference_internal,
	    "Return the nested Sequence if present; otherwise None.")
		.def_prop_ro("pixel_sequence",
	    [](DataElement& element) -> dicom::PixelSequence* {
		    return element.pixel_sequence();
	    },
	    nb::rv_policy::reference_internal,
	    "Return the nested PixelSequence if present; otherwise None.")
	.def("to_uid_string",
	    [](const DataElement& element) -> nb::object {
	        auto v = element.to_uid_string();
	        if (v) {
	            return nb::cast(*v);
	        }
	        return nb::none();
	    },
	    "Return the trimmed UI string value or None if unavailable.")
		.def("to_string_view",
	    [](const DataElement& element) -> nb::object {
	        auto v = element.to_string_view();
	        if (v) {
	            return nb::str(v->data(), v->size());
	        }
	        return nb::none();
	    },
	    "Return a trimmed raw string (no charset decoding) or None if VR is not textual.")
		.def("to_string_views",
	    [](const DataElement& element) -> nb::object {
	        auto values = element.to_string_views();
	        if (!values) {
	            return nb::none();
	        }
	        nb::list out;
	        for (const auto& item : *values) {
	            out.append(nb::str(item.data(), item.size()));
	        }
	        return out;
	    },
	    "Return a list of trimmed raw strings for multi-valued VRs, or None if unsupported.")
		.def("to_utf8_view",
	    [](const DataElement& element) -> nb::object {
	        auto v = element.to_utf8_view();
	        if (v) {
	            return nb::str(v->data(), v->size());
	        }
	        return nb::none();
	    },
	    "Return a charset-decoded UTF-8 string when available, else None.")
		.def("to_utf8_views",
	    [](const DataElement& element) -> nb::object {
	        auto values = element.to_utf8_views();
	        if (!values) {
	            return nb::none();
	        }
	        nb::list out;
	        for (const auto& item : *values) {
	            out.append(nb::str(item.data(), item.size()));
	        }
		return out;
	    },
	    "Return a list of UTF-8 strings for multi-valued VRs, or None if unsupported.")
	.def("to_transfer_syntax_uid",
	    [](const DataElement& element) -> nb::object {
	        auto uid = element.to_transfer_syntax_uid();
	        if (uid) {
	            return nb::cast(*uid);
	        }
	        return nb::none();
	    },
	    "Return a well-known transfer syntax UID if the element matches, else None.")
		.def("to_tag",
		    [](const DataElement& element, nb::object default_value) -> nb::object {
		        if (default_value.is_none()) {
		            auto v = element.to_tag();
		            return v ? nb::cast(*v) : nb::none();
		        }
		        return nb::cast(element.toTag(nb::cast<Tag>(default_value)));
		    },
		    nb::arg("default") = nb::none())
		.def("to_tag_vector",
		    [](const DataElement& element) -> nb::object {
		        auto v = element.to_tag_vector();
	        return v ? nb::cast(*v) : nb::none();
	    })
	.def("to_int",
	    [](const DataElement& element, nb::object default_value) -> nb::object {
	        if (default_value.is_none()) {
	            auto v = element.to_int();
	            return v ? nb::cast(*v) : nb::none();
	        }
	        return nb::cast(element.toInt(nb::cast<int>(default_value)));
	    },
	    nb::arg("default") = nb::none(),
	    "Return int or None; optional default fills on failure")
	.def("to_long",
	    [](const DataElement& element, nb::object default_value) -> nb::object {
	        if (default_value.is_none()) {
	            auto v = element.to_long();
	            return v ? nb::cast(*v) : nb::none();
			    }
			    return nb::cast(element.toLong(nb::cast<long>(default_value)));
		    },
		    nb::arg("default") = nb::none(),
		    "Return int or None; optional default fills on failure")
		.def("to_longlong",
		    [](const DataElement& element, nb::object default_value) -> nb::object {
			    if (default_value.is_none()) {
				    auto v = element.to_longlong();
				    return v ? nb::cast(*v) : nb::none();
			    }
			    return nb::cast(element.toLongLong(nb::cast<long long>(default_value)));
		    },
		    nb::arg("default") = nb::none())
		.def("to_double",
		    [](const DataElement& element, nb::object default_value) -> nb::object {
			    if (default_value.is_none()) {
				    auto v = element.to_double();
				    return v ? nb::cast(*v) : nb::none();
	        }
	        return nb::cast(element.toDouble(nb::cast<double>(default_value)));
	    },
	    nb::arg("default") = nb::none())
	.def("to_int_vector",
	    [](const DataElement& element, nb::object default_value) -> nb::object {
	        if (default_value.is_none()) {
	            auto v = element.to_int_vector();
	            return v ? nb::cast(*v) : nb::none();
	        }
	        return nb::cast(element.toIntVector(nb::cast<std::vector<int>>(default_value)));
	    },
	    nb::arg("default") = nb::none())
	.def("as_uint16_vector",
	    [](const DataElement& element, nb::object default_value) -> nb::object {
	        if (default_value.is_none()) {
	            auto v = element.as_uint16_vector();
	            return v ? nb::cast(*v) : nb::none();
	        }
	        return nb::cast(element.asUint16Vector(nb::cast<std::vector<std::uint16_t>>(default_value)));
	    },
	    nb::arg("default") = nb::none(),
	    "Interpret raw value bytes as uint16 list (honors dataset endianness)")
	.def("as_uint8_vector",
	    [](const DataElement& element, nb::object default_value) -> nb::object {
	        if (default_value.is_none()) {
	            auto v = element.as_uint8_vector();
	            return v ? nb::cast(*v) : nb::none();
	        }
	        return nb::cast(element.asUint8Vector(nb::cast<std::vector<std::uint8_t>>(default_value)));
	    },
	    nb::arg("default") = nb::none(),
	    "Interpret raw value bytes as uint8 list")
	.def("to_long_vector",
	    [](const DataElement& element, nb::object default_value) -> nb::object {
	        if (default_value.is_none()) {
	            auto v = element.to_long_vector();
	            return v ? nb::cast(*v) : nb::none();
			    }
			    return nb::cast(element.toLongVector(nb::cast<std::vector<long>>(default_value)));
		    },
		    nb::arg("default") = nb::none())
		.def("to_longlong_vector",
		    [](const DataElement& element, nb::object default_value) -> nb::object {
			    if (default_value.is_none()) {
				    auto v = element.to_longlong_vector();
				    return v ? nb::cast(*v) : nb::none();
			    }
			    return nb::cast(element.toLongLongVector(nb::cast<std::vector<long long>>(default_value)));
		    },
		    nb::arg("default") = nb::none())
		.def("to_double_vector",
		    [](const DataElement& element, nb::object default_value) -> nb::object {
			    if (default_value.is_none()) {
				    auto v = element.to_double_vector();
				    return v ? nb::cast(*v) : nb::none();
			    }
			    return nb::cast(element.toDoubleVector(nb::cast<std::vector<double>>(default_value)));
		    },
		    nb::arg("default") = nb::none())
		.def("get_value",
		    [](DataElement& element) -> nb::object {
			    return dataelement_get_value_py(element);
		    },
		    "Best-effort typed access: returns int/float/str or list based on VR/VM; "
		    "falls back to raw bytes (memoryview) for binary VRs; "
		    "returns None for NullElement or sequences/pixel sequences.")
		.def("value_span",
		    [](const DataElement& element) {
			    auto span = element.value_span();
			    return readonly_memoryview_from_span(span.data(), span.size());
		    },
		    "Return the raw value bytes as a read-only memoryview")
		.def("__repr__", &dataelement_repr);

	nb::class_<PySequenceIterator>(m, "SequenceIterator")
		.def("__iter__", [](PySequenceIterator& self) -> PySequenceIterator& { return self; })
		.def("__next__",
		    [](PySequenceIterator& self) -> DataSet& { return self.next(); },
		    nb::rv_policy::reference_internal);

	nb::class_<Sequence>(m, "Sequence")
		.def("__len__", &Sequence::size)
		.def("__getitem__",
		    [](Sequence& self, std::size_t index) -> DataSet& {
			    DataSet* ds = self.get_dataset(index);
			    if (!ds) {
				    throw nb::index_error("Sequence index out of range");
			    }
			    return *ds;
		    },
		    nb::arg("index"),
		    nb::rv_policy::reference_internal)
		.def("__iter__",
		    [](Sequence& self) {
			    return PySequenceIterator(self);
		    },
		    nb::keep_alive<0, 1>(),
		    "Iterate over child DataSets in insertion order")
		.def("add_dataset",
		    [](Sequence& self) -> DataSet& {
			    DataSet* ds = self.add_dataset();
			    if (!ds) {
				    throw std::runtime_error("Failed to append DataSet to sequence");
			    }
			    return *ds;
		    },
		    nb::rv_policy::reference_internal,
		    "Append a new DataSet to the sequence and return it")
		.def("__repr__",
		    [](Sequence& self) {
			    std::ostringstream oss;
			    oss << "Sequence(len=" << self.size() << ")";
			    return oss.str();
		    });

	nb::class_<PyDataElementIterator>(m, "DataElementIterator")
		.def("__iter__", [](PyDataElementIterator& self) -> PyDataElementIterator& { return self; })
		.def("__next__",
		    [](PyDataElementIterator& self) -> DataElement& { return self.next(); },
		    nb::rv_policy::reference_internal);

	nb::class_<DataSet>(m, "DataSet",
	    "In-memory DICOM dataset. Created via read_file/read_bytes or directly.\n"
	    "\n"
	    "Features\n"
	    "--------\n"
	    "- Iterable over DataElements in tag order\n"
	    "- Indexing by Tag, packed int, or tag-path string\n"
	    "- Attribute access by keyword (e.g., ds.PatientName)\n"
	    "- Missing lookups return a NullElement sentinel (VR::None)")
		.def(nb::init<>())
	.def_prop_ro("path", &DataSet::path, "Identifier of the attached stream (file path, provided name, or '<memory>')")
		.def("add_dataelement",
		    [](DataSet& self, const Tag& tag, std::optional<VR> vr,
		        std::size_t offset, std::size_t length) {
		        const VR resolved = vr.value_or(VR::None);
		        DataElement* element = self.add_dataelement(tag, resolved, offset, length);
		        return element ? element : dicom::NullElement();
		    },
		    nb::arg("tag"), nb::arg("vr") = nb::none(),
		    nb::arg("offset") = 0, nb::arg("length") = 0,
		    nb::rv_policy::reference_internal,
		    "Add or update a DataElement and return a reference to it")
		.def("remove_dataelement",
		    [](DataSet& self, const Tag& tag) {
		        self.remove_dataelement(tag);
		    },
		    nb::arg("tag"),
		    "Remove a DataElement by tag if it exists")
		.def("dump_elements", &DataSet::dump_elements,
		    "Print internal element storage for debugging")
		.def("pixel_data",
		    [](const DataSet& self, std::size_t frame_index) {
			    const auto decoded = self.pixel_data(frame_index);
			    if (decoded.empty()) {
				    return nb::bytes("", 0);
			    }
			    return nb::bytes(reinterpret_cast<const char*>(decoded.data()), decoded.size());
		    },
		    nb::arg("frame_index") = 0,
		    "Decode one frame with default options and return decoded bytes.")
		.def("pixel_array",
		    &dataset_pixel_array,
		    nb::arg("frame") = -1,
		    nb::arg("scaled") = false,
		    "Decode pixel samples and return a NumPy array.\n"
		    "\n"
		    "Parameters\n"
		    "----------\n"
		    "frame : int, optional\n"
		    "    -1 decodes all frames (multi-frame only), otherwise decode the selected frame index.\n"
		    "scaled : bool, optional\n"
		    "    If True, apply Modality LUT/Rescale and return float32 output.\n"
		    "\n"
		    "Returns\n"
		    "-------\n"
		    "numpy.ndarray\n"
		    "    Shape is (rows, cols) or (rows, cols, samples) for a single frame,\n"
		    "    and (frames, rows, cols) or (frames, rows, cols, samples) when decoding all frames.")
		.def("get_dataelement",
		    [](DataSet& self, const Tag& tag) -> DataElement& {
		        return *self.get_dataelement(tag);
		    },
		    nb::arg("tag"), nb::rv_policy::reference_internal,
		    "Return the DataElement for a tag or a VR::None NullElement sentinel if missing")
		.def("get_dataelement",
		    [](DataSet& self, std::uint32_t packed) -> DataElement& {
			    const Tag tag(packed);
			    return *self.get_dataelement(tag);
		    },
		    nb::arg("packed_tag"),
		    nb::rv_policy::reference_internal,
		    "Overload: pass packed 0xGGGEEEE integer; returns NullElement if missing")
		.def("get_dataelement",
		    [](DataSet& self, const std::string& tag_str) -> DataElement& {
			    return *self.get_dataelement(tag_str);
		    },
		    nb::arg("tag_str"),
		    nb::rv_policy::reference_internal,
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
		    [](DataSet& self, nb::object key) -> nb::object {
			    DataElement* el = nullptr;

			    if (nb::isinstance<Tag>(key)) {
				    el = self.get_dataelement(nb::cast<Tag>(key));
			    } else if (nb::isinstance<nb::int_>(key)) {
				    el = self.get_dataelement(Tag(nb::cast<std::uint32_t>(key)));
			    } else if (nb::isinstance<nb::str>(key)) {
				    // Allow full tag-path strings (including sequences)
				    el = self.get_dataelement(nb::cast<std::string>(key));
			    } else {
				    throw nb::type_error("DataSet indices must be Tag, int (0xGGGEEEE), or str");
			    }

			    if (!el || el == dicom::NullElement()) {
				    return nb::none();
			    }
			    return dataelement_get_value_py(*el, nb::cast(&self, nb::rv_policy::reference));
		    },
		    nb::arg("key"),
		    "Index syntax: ds[tag|packed_int|tag_str] -> element.get_value(); returns None if missing")
		.def("__getattr__",
		    [](DataSet& self, const std::string& name) -> nb::object {
			    // Allow keyword-style attribute access: ds.PatientName -> get_value("PatientName")
			    if (!name.empty() && name.size() >= 2 && name[0] != '_') {
				    try {
					    Tag tag(name);
					    DataElement* el = self.get_dataelement(tag);
					    if (el && el != dicom::NullElement()) {
						    return dataelement_get_value_py(*el, nb::cast(&self, nb::rv_policy::reference));
					    }
				    } catch (const std::exception&) {
					    // fall through to AttributeError
				    }
			    }
			    throw nb::attribute_error(("DataSet has no attribute '" + name + "'").c_str());
		    },
		    nb::arg("name"),
		    "Attribute sugar: ds.PatientName -> ds.get_dataelement('PatientName').get_value(); "
		    "raises AttributeError if no such keyword/tag or element is missing.")
		.def("__dir__",
		    [](DataSet& self) {
			    nb::object self_obj = nb::cast(&self, nb::rv_policy::reference);
			    PyObject* type_obj = reinterpret_cast<PyObject*>(Py_TYPE(self_obj.ptr()));
			    nb::list result = nb::steal<nb::list>(PyObject_Dir(type_obj));  // class attrs

			    std::unordered_set<std::string> seen;
			    for (nb::handle item : result) {
				    seen.insert(nb::cast<std::string>(item));
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
					    result.append(nb::str(kw_str.c_str(), kw_str.size()));
				    }
			    }
			    return result;
		    },
		    "dir() includes standard attributes plus public data element keywords (excludes group length/private).")
		.def("__iter__",
		    [](DataSet& self) {
			    return PyDataElementIterator(self);
		    },
		    nb::keep_alive<0, 1>(), "Iterate over DataElements in tag order");

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
    nb::arg("path"),
    nb::arg("load_until") = nb::none(),
    nb::arg("keep_on_error") = nb::none(),
    "Read a DICOM file from disk and return a DataSet.\n"
    "\n"
    "Parameters\n"
    "----------\n"
    "path : str\n"
    "    Filesystem path to the DICOM Part 10 file.\n"
    "load_until : Tag | None, optional\n"
    "    Stop after this tag is read (inclusive). Defaults to reading entire file.\n"
    "keep_on_error : bool | None, optional\n"
    "    When True, keep partially read data instead of raising on parse errors.\n");

m.def("read_bytes",
    [] (nb::object buffer, const std::string& name, std::optional<Tag> load_until,
        std::optional<bool> keep_on_error, bool copy) {
        PyBufferView view(buffer);
        const Py_buffer& info = view.view();
        if (info.ndim != 1) {
            throw std::invalid_argument("read_bytes expects a 1-D bytes-like object");
        }
        if (!PyBuffer_IsContiguous(&info, 'C')) {
            throw std::invalid_argument("read_bytes expects a contiguous bytes-like object");
        }

        const std::size_t elem_size = static_cast<std::size_t>(info.itemsize <= 0 ? 1 : info.itemsize);
        const std::size_t total = static_cast<std::size_t>(info.len);

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
		        std::memcpy(owned.data(), info.buf, total);
	        }
	        dataset = dicom::read_bytes(std::string{name}, std::move(owned), opts);
        } else {
	        if (elem_size != 1) {
		        throw std::invalid_argument("read_bytes(copy=False) requires a byte-oriented buffer");
	        }
	        dataset = dicom::read_bytes(std::string{name},
	            static_cast<const std::uint8_t*>(info.buf), total, opts);
        }

        nb::object py_dataset = nb::cast(std::move(dataset));
        if (!copy && total > 0) {
	        py_dataset.attr("_buffer_owner") = buffer;
        }
        return py_dataset;
    },
    nb::arg("data"),
    nb::arg("name") = std::string{"<memory>"},
    nb::arg("load_until") = nb::none(),
    nb::arg("keep_on_error") = nb::none(),
    nb::arg("copy") = true,
    "Read a DataSet from a bytes-like object. Parsing is eager up to `load_until`.\n"
    "\n"
    "Parameters\n"
    "----------\n"
    "data : buffer\n"
    "    1-D bytes-like object containing the Part 10 stream (or raw stream).\n"
    "name : str, optional\n"
    "    Identifier reported by DataSet.path() and diagnostics. Default '<memory>'.\n"
    "load_until : Tag | None, optional\n"
    "    Stop after this tag is read (inclusive). Defaults to reading entire buffer.\n"
    "keep_on_error : bool | None, optional\n"
    "    When True, keep partially read data instead of raising on parse errors.\n"
    "copy : bool, optional\n"
    "    When False, avoid copying and reference the caller's buffer; caller must keep\n"
    "    the buffer alive while the DataSet exists.\n"
    "\n"
    "Warning\n"
    "-------\n"
    "When copy=False, the source buffer must remain alive for as long as the returned DataSet;\n"
    "the binding keeps a Python reference, but mutating or freeing the underlying memory can\n"
    "still corrupt the dataset.");

	nb::class_<Tag>(m, "Tag")
		.def(nb::init<>())
		.def(nb::init<std::uint16_t, std::uint16_t>(), nb::arg("group"), nb::arg("element"))
		.def(nb::init<const std::string&>(), nb::arg("keyword"))
		.def_static("from_value", &Tag::from_value, nb::arg("value"))
		.def_prop_ro("group", &Tag::group)
		.def_prop_ro("element", &Tag::element)
		.def_prop_ro("value", &Tag::value)
		.def("is_private", &Tag::is_private)
		.def("__int__", &Tag::value)
		.def("__bool__", [](const Tag& tag) { return static_cast<bool>(tag); })
		.def("__str__", &Tag::to_string)
		.def("__repr__", &tag_repr)
		.def(nb::self == nb::self);

auto uid_cls = nb::class_<Uid>(m, "Uid")
	.def(nb::init<>())
	.def("__init__",
	    [](Uid* self, const std::string& text) {
		    new (self) Uid(require_uid(dicom::uid::lookup(text), "Uid.__init__", text));
	    },
	    nb::arg("text"),
	    "Construct a UID from either a dotted value or keyword, raising ValueError if unknown.")
	.def_static("lookup",
	    [](const std::string& text) -> nb::object {
		    return uid_or_none(dicom::uid::lookup(text));
	    },
	    nb::arg("text"),
	    "Lookup a UID from value or keyword; returns None if missing.")
	.def_static("from_value",
	    [](const std::string& value) {
		    return require_uid(dicom::uid::from_value(value), "Uid.from_value", value);
	    },
	    nb::arg("value"),
	    "Resolve a dotted UID value, raising ValueError if unknown.")
	.def_static("from_keyword",
	    [](const std::string& keyword) {
		    return require_uid(dicom::uid::from_keyword(keyword), "Uid.from_keyword", keyword);
	    },
	    nb::arg("keyword"),
	    "Resolve a UID keyword, raising ValueError if unknown.")
	.def_prop_ro("value",
	    [](const Uid& uid) { return std::string(uid.value()); },
	    "Return the dotted UID value or empty string if invalid.")
	.def_prop_ro("keyword",
	    [](const Uid& uid) -> nb::object {
		    if (uid.keyword().empty()) {
			    return nb::none();
		    }
		    return nb::str(uid.keyword().data(), uid.keyword().size());
	    },
	    "Return the UID keyword or None if missing.")
	.def_prop_ro("name",
	    [](const Uid& uid) { return std::string(uid.name()); },
	    "Return the descriptive UID name or empty string if invalid.")
	.def_prop_ro("type",
	    [](const Uid& uid) { return std::string(uid.type()); },
	    "Return the UID type (Transfer Syntax, SOP Class, ...).")
	.def_prop_ro("raw_index", &Uid::raw_index, "Return the registry index.")
	.def_prop_ro("is_valid", &Uid::valid)
	.def("__bool__", [](const Uid& uid) { return uid.valid(); })
	.def("__repr__", &uid_repr)
	.def(nb::self == nb::self);

	auto vr_cls = nb::class_<VR>(m, "VR")
		.def(nb::init<>())
		.def(nb::init<std::uint16_t>(), nb::arg("value"))
		.def_static("from_string", &VR::from_string, nb::arg("value"))
		.def_static("from_chars", [](char a, char b) { return VR::from_chars(a, b); },
		         nb::arg("first"), nb::arg("second"))
		.def_prop_ro("value", [] (const VR& vr) { return static_cast<std::uint16_t>(vr); })
		.def_prop_ro("is_known", &VR::is_known)
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
		.def(nb::self == nb::self)
		.def_prop_ro_static("None", [](nb::handle) { return dicom::VR::None; })
		.def_prop_ro_static("AE", [](nb::handle) { return dicom::VR::AE; })
		.def_prop_ro_static("AS", [](nb::handle) { return dicom::VR::AS; })
		.def_prop_ro_static("AT", [](nb::handle) { return dicom::VR::AT; })
		.def_prop_ro_static("CS", [](nb::handle) { return dicom::VR::CS; })
		.def_prop_ro_static("DA", [](nb::handle) { return dicom::VR::DA; })
		.def_prop_ro_static("DS", [](nb::handle) { return dicom::VR::DS; })
		.def_prop_ro_static("DT", [](nb::handle) { return dicom::VR::DT; })
		.def_prop_ro_static("FD", [](nb::handle) { return dicom::VR::FD; })
		.def_prop_ro_static("FL", [](nb::handle) { return dicom::VR::FL; })
		.def_prop_ro_static("IS", [](nb::handle) { return dicom::VR::IS; })
		.def_prop_ro_static("LO", [](nb::handle) { return dicom::VR::LO; })
		.def_prop_ro_static("LT", [](nb::handle) { return dicom::VR::LT; })
		.def_prop_ro_static("OB", [](nb::handle) { return dicom::VR::OB; })
		.def_prop_ro_static("OD", [](nb::handle) { return dicom::VR::OD; })
		.def_prop_ro_static("OF", [](nb::handle) { return dicom::VR::OF; })
		.def_prop_ro_static("OV", [](nb::handle) { return dicom::VR::OV; })
		.def_prop_ro_static("OL", [](nb::handle) { return dicom::VR::OL; })
		.def_prop_ro_static("OW", [](nb::handle) { return dicom::VR::OW; })
		.def_prop_ro_static("PN", [](nb::handle) { return dicom::VR::PN; })
		.def_prop_ro_static("SH", [](nb::handle) { return dicom::VR::SH; })
		.def_prop_ro_static("SL", [](nb::handle) { return dicom::VR::SL; })
		.def_prop_ro_static("SQ", [](nb::handle) { return dicom::VR::SQ; })
		.def_prop_ro_static("SS", [](nb::handle) { return dicom::VR::SS; })
		.def_prop_ro_static("ST", [](nb::handle) { return dicom::VR::ST; })
		.def_prop_ro_static("SV", [](nb::handle) { return dicom::VR::SV; })
		.def_prop_ro_static("TM", [](nb::handle) { return dicom::VR::TM; })
		.def_prop_ro_static("UC", [](nb::handle) { return dicom::VR::UC; })
		.def_prop_ro_static("UI", [](nb::handle) { return dicom::VR::UI; })
		.def_prop_ro_static("UL", [](nb::handle) { return dicom::VR::UL; })
		.def_prop_ro_static("UN", [](nb::handle) { return dicom::VR::UN; })
		.def_prop_ro_static("UR", [](nb::handle) { return dicom::VR::UR; })
		.def_prop_ro_static("US", [](nb::handle) { return dicom::VR::US; })
		.def_prop_ro_static("UT", [](nb::handle) { return dicom::VR::UT; })
		.def_prop_ro_static("UV", [](nb::handle) { return dicom::VR::UV; })
		.def_prop_ro_static("PX", [](nb::handle) { return dicom::VR::PX; });

	nb::class_<dicom::PixelFragment>(m, "PixelFragment")
		.def_ro("offset", &dicom::PixelFragment::offset, "Fragment offset relative to pixel sequence base")
		.def_ro("length", &dicom::PixelFragment::length, "Fragment length in bytes")
		.def("__repr__",
		    [](const dicom::PixelFragment& frag) {
			    std::ostringstream oss;
			    oss << "PixelFragment(offset=0x" << std::hex << frag.offset
			        << ", length=" << std::dec << frag.length << ")";
			    return oss.str();
		    });

	nb::class_<dicom::PixelFrame>(m, "PixelFrame")
		.def_prop_ro("encoded_size", &dicom::PixelFrame::encoded_data_size,
		    "Size in bytes of materialized encoded data; 0 if not loaded")
		.def_prop_ro("fragments",
		    [](const dicom::PixelFrame& f) { return f.fragments(); },
		    "Fragments belonging to this frame")
		.def("encoded_bytes",
		    [](dicom::PixelFrame& f) {
			    auto span = f.encoded_data_view();
			    return nb::bytes(reinterpret_cast<const char*>(span.data()), span.size());
		    },
		    "Return encoded pixel data as bytes (coalesced if needed)")
		.def("encoded_memoryview",
		    [](dicom::PixelFrame& f) {
			    auto span = f.encoded_data_view();
			    return readonly_memoryview_from_span(span.data(), span.size());
		    },
		    "Return a read-only memoryview over encoded pixel data (no copy); "
		    "invalidated if the frame's encoded data is cleared");

	nb::class_<dicom::PixelSequence>(m, "PixelSequence")
		.def_prop_ro("number_of_frames", &dicom::PixelSequence::number_of_frames)
		.def_prop_ro("basic_offset_table_offset", &dicom::PixelSequence::basic_offset_table_offset)
		.def_prop_ro("basic_offset_table_count", &dicom::PixelSequence::basic_offset_table_count)
		.def("__len__", &dicom::PixelSequence::number_of_frames)
		.def("frame",
		    [](dicom::PixelSequence& self, std::size_t index) -> dicom::PixelFrame& {
			    dicom::PixelFrame* f = self.frame(index);
			    if (!f) throw nb::index_error("PixelSequence index out of range");
			    return *f;
		    },
		    nb::arg("index"),
		    nb::rv_policy::reference_internal)
		.def("frame_encoded_bytes",
		    [](dicom::PixelSequence& self, std::size_t index) {
			    auto span = self.frame_encoded_span(index);
			    return nb::bytes(reinterpret_cast<const char*>(span.data()), span.size());
		    },
		    nb::arg("index"),
		    "Return encoded pixel data for a frame (coalesces fragments if needed)")
		.def("frame_encoded_memoryview",
		    [](dicom::PixelSequence& self, std::size_t index) {
			    auto span = self.frame_encoded_span(index);
			    return readonly_memoryview_from_span(span.data(), span.size());
		    },
		    nb::arg("index"),
		    "Return a read-only memoryview over encoded pixel data for a frame (no copy)")
	.def("__repr__",
	    [](dicom::PixelSequence& self) {
		    std::ostringstream oss;
		    oss << "PixelSequence(frames=" << self.number_of_frames() << ")";
		    return oss.str();
	    });

	m.def("keyword_to_tag_vr",
	    [] (const std::string& keyword) -> nb::object {
	        auto [tag, vr] = dicom::lookup::keyword_to_tag_vr(keyword);
	        if (!static_cast<bool>(tag)) {
	            return nb::none();
	        }
	        return nb::make_tuple(tag, vr);
	    },
	    nb::arg("keyword"),
	    "Return (Tag, VR) for the provided DICOM keyword or None if missing.");

	m.def("tag_to_keyword",
	    [] (const Tag& tag) -> nb::object {
	        const auto keyword = dicom::lookup::tag_to_keyword(tag.value());
	        if (keyword.empty()) {
	            return nb::none();
	        }
	        return nb::str(keyword.data(), keyword.size());
	    },
	    nb::arg("tag"),
	    "Return the DICOM keyword for this Tag or None if missing.");

	m.def("tag_to_keyword",
	    [] (std::uint32_t tag_value) -> nb::object {
	        const auto keyword = dicom::lookup::tag_to_keyword(tag_value);
	        if (keyword.empty()) {
	            return nb::none();
	        }
	        return nb::str(keyword.data(), keyword.size());
	    },
	    nb::arg("tag_value"),
	    "Return the DICOM keyword for a 32-bit tag value or None if missing.");

	m.def("tag_to_entry",
	    [] (const Tag& tag) -> nb::object {
	        if (const auto* entry = dicom::lookup::tag_to_entry(tag.value())) {
	            return make_tag_entry_dict(*entry);
	        }
	        return nb::none();
	    },
	    nb::arg("tag"),
	    "Return registry details for the given Tag or None if missing.");

	m.def("tag_to_entry",
	    [] (std::uint32_t tag_value) -> nb::object {
	        if (const auto* entry = dicom::lookup::tag_to_entry(tag_value)) {
	            return make_tag_entry_dict(*entry);
	        }
	        return nb::none();
	    },
	    nb::arg("tag_value"),
	    "Return registry details for a tag numeric value or None if missing.");

m.def("lookup_uid",
    [] (const std::string& text) -> nb::object {
        return uid_or_none(dicom::uid::lookup(text));
    },
    nb::arg("text"),
    "Lookup a UID by either dotted value or keyword; returns None if missing.");

m.def("uid_from_value",
    [] (const std::string& value) {
        return require_uid(dicom::uid::from_value(value), "uid_from_value", value);
    },
    nb::arg("value"),
    "Resolve a dotted UID value, raising ValueError if unknown.");

m.def("uid_from_keyword",
    [] (const std::string& keyword) {
        return require_uid(dicom::uid::from_keyword(keyword), "uid_from_keyword", keyword);
    },
    nb::arg("keyword"),
    "Resolve a UID keyword, raising ValueError if unknown.");

	m.attr("__all__") = nb::make_tuple(
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
