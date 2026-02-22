#include <array>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <limits>
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
#include <dicom_endian.h>
#include <diagnostics.h>

namespace nb = nanobind;

using dicom::DataSet;
using dicom::DicomFile;
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

struct DecodedArrayOutput {
	nb::ndarray<nb::numpy> array;
	std::span<std::uint8_t> bytes;
};

struct DecodedArrayLayout {
	DecodedArraySpec spec{};
	dicom::pixel::DecodeOptions opt{};
	dicom::pixel::DecodeStrides dst_strides{};
	std::array<std::size_t, 4> shape{};
	std::array<std::int64_t, 4> strides{};
	std::size_t ndim{0};
	std::size_t frame_stride{0};
	std::size_t frames{0};
	std::size_t frame_index{0};
	std::size_t required_bytes{0};
	bool decode_all_frames{false};
};

DecodedArraySpec decoded_array_spec(const DicomFile::pixel_info_t& info, bool scaled) {
	if (scaled) {
		return DecodedArraySpec{nb::dtype<float>(), sizeof(float)};
	}

	switch (info.sv_dtype) {
	case dicom::pixel::DataType::u8:
		return DecodedArraySpec{nb::dtype<std::uint8_t>(), sizeof(std::uint8_t)};
	case dicom::pixel::DataType::s8:
		return DecodedArraySpec{nb::dtype<std::int8_t>(), sizeof(std::int8_t)};
	case dicom::pixel::DataType::u16:
		return DecodedArraySpec{nb::dtype<std::uint16_t>(), sizeof(std::uint16_t)};
	case dicom::pixel::DataType::s16:
		return DecodedArraySpec{nb::dtype<std::int16_t>(), sizeof(std::int16_t)};
	case dicom::pixel::DataType::u32:
		return DecodedArraySpec{nb::dtype<std::uint32_t>(), sizeof(std::uint32_t)};
	case dicom::pixel::DataType::s32:
		return DecodedArraySpec{nb::dtype<std::int32_t>(), sizeof(std::int32_t)};
	case dicom::pixel::DataType::f32:
		return DecodedArraySpec{nb::dtype<float>(), sizeof(float)};
	case dicom::pixel::DataType::f64:
		return DecodedArraySpec{nb::dtype<double>(), sizeof(double)};
	default:
		break;
	}

	throw nb::value_error("to_array requires a known pixel sample dtype");
}

DecodedArrayOutput make_writable_numpy_array(
    std::size_t ndim, const std::array<std::size_t, 4>& shape,
    const std::array<std::int64_t, 4>& strides, const nb::dlpack::dtype& dtype,
    std::size_t required_bytes) {
	auto storage = std::make_unique<std::uint8_t[]>(required_bytes);
	void* data_ptr = required_bytes == 0 ? nullptr : static_cast<void*>(storage.get());
	nb::capsule owner(data_ptr, [](void* ptr) noexcept {
		delete[] static_cast<std::uint8_t*>(ptr);
	});
	nb::ndarray<nb::numpy> array(
	    data_ptr, ndim, shape.data(), owner, strides.data(), dtype,
	    nb::device::cpu::value, 0, 'C');
	const auto bytes = std::span<std::uint8_t>(
	    static_cast<std::uint8_t*>(data_ptr), required_bytes);
	(void)storage.release();
	return DecodedArrayOutput{std::move(array), bytes};
}

std::string normalize_htj2k_decoder_name(std::string decoder) {
	std::string normalized{};
	normalized.reserve(decoder.size());
	for (const unsigned char ch : decoder) {
		if (ch == '_' || ch == '-' || ch == ' ' || ch == '\t') {
			continue;
		}
		normalized.push_back(static_cast<char>(std::tolower(ch)));
	}
	return normalized;
}

dicom::pixel::Htj2kDecoder parse_htj2k_decoder(std::string decoder) {
	const auto normalized = normalize_htj2k_decoder_name(std::move(decoder));
	if (normalized.empty() || normalized == "auto" || normalized == "autoselect") {
		return dicom::pixel::Htj2kDecoder::auto_select;
	}
	if (normalized == "openjph" || normalized == "ojph") {
		return dicom::pixel::Htj2kDecoder::openjph;
	}
	if (normalized == "openjpeg" || normalized == "openjp2") {
		return dicom::pixel::Htj2kDecoder::openjpeg;
	}
	throw nb::value_error("htj2k_decoder must be one of: 'auto', 'openjph', 'openjpeg'");
}

DecodedArrayLayout build_decode_layout(
    const DicomFile& self, long frame, bool scaled, int decoder_threads = -1,
    dicom::pixel::Htj2kDecoder htj2k_decoder_backend = dicom::pixel::Htj2kDecoder::auto_select) {
	if (frame < -1) {
		throw nb::value_error("frame must be >= -1");
	}
	if (decoder_threads < -1) {
		throw nb::value_error("threads must be -1, 0, or positive");
	}

	const auto& info = self.pixel_info();
	if (!info.has_pixel_data) {
		throw nb::value_error(
		    "to_array/decode_into requires PixelData, FloatPixelData, or DoubleFloatPixelData");
	}
	if (info.rows <= 0 || info.cols <= 0 || info.samples_per_pixel <= 0) {
		throw nb::value_error(
		    "to_array/decode_into requires positive Rows/Columns/SamplesPerPixel");
	}
	if (info.frames <= 0) {
		throw nb::value_error("to_array/decode_into requires NumberOfFrames >= 1");
	}

	DecodedArrayLayout layout{};
	const auto rows = static_cast<std::size_t>(info.rows);
	const auto cols = static_cast<std::size_t>(info.cols);
	const auto samples_per_pixel = static_cast<std::size_t>(info.samples_per_pixel);
	layout.frames = static_cast<std::size_t>(info.frames);

	layout.opt.planar_out = dicom::pixel::Planar::interleaved;
	layout.opt.alignment = 1;
	layout.opt.scaled = scaled;
	layout.opt.decoder_threads = decoder_threads;
	layout.opt.htj2k_decoder_backend = htj2k_decoder_backend;
	const bool effective_scaled = dicom::pixel::should_use_scaled_output(self, layout.opt);
	layout.opt.scaled = effective_scaled;

	layout.dst_strides = self.calc_decode_strides(layout.opt);
	layout.spec = decoded_array_spec(info, effective_scaled);

	const auto bytes_per_sample = layout.spec.bytes_per_sample;
	if (bytes_per_sample == 0) {
		throw nb::value_error("to_array/decode_into could not determine output sample size");
	}
	const auto row_stride = layout.dst_strides.row;
	layout.frame_stride = layout.dst_strides.frame;
	if ((row_stride % bytes_per_sample) != 0 || (layout.frame_stride % bytes_per_sample) != 0) {
		throw std::runtime_error(
		    "to_array/decode_into stride is not aligned to output sample size");
	}
	const auto row_stride_elems = row_stride / bytes_per_sample;
	const auto frame_stride_elems = layout.frame_stride / bytes_per_sample;
	const auto col_stride_elems = samples_per_pixel;

	if (layout.frames != 0 &&
	    layout.frame_stride > (std::numeric_limits<std::size_t>::max() / layout.frames)) {
		throw std::overflow_error("to_array/decode_into output buffer size overflow");
	}

	layout.decode_all_frames = (frame == -1) && (layout.frames > 1);
	if (!layout.decode_all_frames) {
		layout.frame_index = (frame < 0) ? std::size_t{0} : static_cast<std::size_t>(frame);
		if (layout.frame_index >= layout.frames) {
			throw nb::index_error("to_array/decode_into frame index out of range");
		}
		layout.required_bytes = layout.frame_stride;

		if (samples_per_pixel == 1) {
			layout.ndim = 2;
			layout.shape[0] = rows;
			layout.shape[1] = cols;
			layout.strides[0] = static_cast<std::int64_t>(row_stride_elems);
			layout.strides[1] = 1;
		} else {
			layout.ndim = 3;
			layout.shape[0] = rows;
			layout.shape[1] = cols;
			layout.shape[2] = samples_per_pixel;
			layout.strides[0] = static_cast<std::int64_t>(row_stride_elems);
			layout.strides[1] = static_cast<std::int64_t>(col_stride_elems);
			layout.strides[2] = 1;
		}
		return layout;
	}

	layout.required_bytes = layout.frame_stride * layout.frames;
	if (samples_per_pixel == 1) {
		layout.ndim = 3;
		layout.shape[0] = layout.frames;
		layout.shape[1] = rows;
		layout.shape[2] = cols;
		layout.strides[0] = static_cast<std::int64_t>(frame_stride_elems);
		layout.strides[1] = static_cast<std::int64_t>(row_stride_elems);
		layout.strides[2] = 1;
	} else {
		layout.ndim = 4;
		layout.shape[0] = layout.frames;
		layout.shape[1] = rows;
		layout.shape[2] = cols;
		layout.shape[3] = samples_per_pixel;
		layout.strides[0] = static_cast<std::int64_t>(frame_stride_elems);
		layout.strides[1] = static_cast<std::int64_t>(row_stride_elems);
		layout.strides[2] = static_cast<std::int64_t>(col_stride_elems);
		layout.strides[3] = 1;
	}
	return layout;
}

void decode_layout_into(const DicomFile& self, const DecodedArrayLayout& layout,
    std::span<std::uint8_t> out) {
	if (out.size() < layout.required_bytes) {
		throw nb::value_error("decode_into output buffer is smaller than required size");
	}
	if (!layout.decode_all_frames) {
		self.decode_into(layout.frame_index, out, layout.dst_strides, layout.opt);
		return;
	}

	for (std::size_t frame_index = 0; frame_index < layout.frames; ++frame_index) {
		auto frame_span = out.subspan(frame_index * layout.frame_stride, layout.frame_stride);
		self.decode_into(frame_index, frame_span, layout.dst_strides, layout.opt);
	}
}

const DataElement* raw_source_element(const DicomFile& self, dicom::pixel::DataType sv_dtype) {
	const auto& dataset = self.dataset();
	switch (sv_dtype) {
	case dicom::pixel::DataType::f32:
		return dataset.get_dataelement(Tag("FloatPixelData"));
	case dicom::pixel::DataType::f64:
		return dataset.get_dataelement(Tag("DoubleFloatPixelData"));
	default:
		return dataset.get_dataelement(Tag("PixelData"));
	}
}

nb::object dicomfile_to_array_view(const DicomFile& self, long frame) {
	const auto layout = build_decode_layout(self, frame, false, 0);
	const auto& info = self.pixel_info();
	const auto& dataset = self.dataset();

	if (!info.ts.is_uncompressed()) {
		throw nb::value_error("to_array_view requires an uncompressed transfer syntax");
	}
	if (info.samples_per_pixel > 1 &&
	    info.planar_configuration != dicom::pixel::Planar::interleaved) {
		throw nb::value_error(
		    "to_array_view requires PlanarConfiguration=interleaved when SamplesPerPixel > 1");
	}
	if (layout.spec.bytes_per_sample > 1 &&
	    (dataset.is_little_endian() != dicom::endian::host_is_little_endian())) {
		throw nb::value_error(
		    "to_array_view requires source endianness to match host endianness");
	}

	const auto* source = raw_source_element(self, info.sv_dtype);
	if (source->is_missing()) {
		throw nb::value_error("to_array_view requires source pixel data to be present");
	}
	if (source->vr().is_pixel_sequence()) {
		throw nb::value_error("to_array_view does not support encapsulated PixelData");
	}

	const auto src = source->value_span();
	const auto rows = static_cast<std::size_t>(info.rows);
	const auto cols = static_cast<std::size_t>(info.cols);
	const auto samples_per_pixel = static_cast<std::size_t>(info.samples_per_pixel);
	const auto src_row_components =
	    (info.planar_configuration == dicom::pixel::Planar::interleaved)
	        ? samples_per_pixel
	        : std::size_t{1};
	const auto src_row_bytes = cols * src_row_components * layout.spec.bytes_per_sample;
	std::size_t src_frame_bytes = src_row_bytes * rows;
	if (info.planar_configuration == dicom::pixel::Planar::planar) {
		src_frame_bytes *= samples_per_pixel;
	}
	if (layout.frames != 0 &&
	    src_frame_bytes > (std::numeric_limits<std::size_t>::max() / layout.frames)) {
		throw nb::value_error("to_array_view source frame size overflow");
	}
	const auto total_required_bytes = src_frame_bytes * layout.frames;
	if (src.size() < total_required_bytes) {
		throw nb::value_error("to_array_view source pixel data is smaller than expected");
	}
	if (src_frame_bytes < layout.required_bytes) {
		throw nb::value_error("to_array_view requires contiguous source frame layout");
	}

	const auto byte_offset = layout.decode_all_frames ? std::size_t{0} : layout.frame_index * src_frame_bytes;
	if (src.size() < byte_offset + layout.required_bytes) {
		throw nb::value_error("to_array_view requested frame is out of source bounds");
	}

	const auto* data_ptr = layout.required_bytes == 0 ? nullptr : (src.data() + byte_offset);
	nb::object owner = nb::cast(&self, nb::rv_policy::reference);
	return nb::cast(nb::ndarray<nb::numpy, const std::uint8_t>(
	    data_ptr, layout.ndim, layout.shape.data(), owner, layout.strides.data(), layout.spec.dtype,
	    nb::device::cpu::value, 0, 'C'));
}

nb::object dicomfile_to_array(
    const DicomFile& self, long frame, bool scaled, const std::string& htj2k_decoder) {
	const auto layout = build_decode_layout(
	    self, frame, scaled, -1, parse_htj2k_decoder(htj2k_decoder));
	auto out = make_writable_numpy_array(
	    layout.ndim, layout.shape, layout.strides, layout.spec.dtype, layout.required_bytes);
	decode_layout_into(self, layout, out.bytes);
	return nb::cast(std::move(out.array));
}

class PyWritableBufferView {
public:
	explicit PyWritableBufferView(nb::handle obj) {
		const int flags = PyBUF_WRITABLE | PyBUF_C_CONTIGUOUS | PyBUF_FORMAT | PyBUF_ND;
		if (PyObject_GetBuffer(obj.ptr(), &view_, flags) != 0) {
			throw nb::type_error(
			    "decode_into expects a writable C-contiguous buffer object");
		}
	}

	~PyWritableBufferView() { PyBuffer_Release(&view_); }

	PyWritableBufferView(const PyWritableBufferView&) = delete;
	PyWritableBufferView& operator=(const PyWritableBufferView&) = delete;

	const Py_buffer& view() const { return view_; }

private:
	Py_buffer view_{};
};

nb::object dicomfile_decode_into_array(const DicomFile& self, nb::handle out,
    long frame, bool scaled, int threads, const std::string& htj2k_decoder) {
	const auto layout = build_decode_layout(
	    self, frame, scaled, threads, parse_htj2k_decoder(htj2k_decoder));

	PyWritableBufferView writable(out);
	const auto& view = writable.view();
	if (view.itemsize <= 0) {
		throw nb::value_error("decode_into requires a valid output itemsize");
	}
	const auto actual_itemsize = static_cast<std::size_t>(view.itemsize);
	if (actual_itemsize != layout.spec.bytes_per_sample) {
		throw nb::value_error("decode_into output itemsize does not match decoded sample size");
	}
	if (view.len < 0) {
		throw nb::value_error("decode_into output buffer length is invalid");
	}
	const auto actual_bytes = static_cast<std::size_t>(view.len);
	if (actual_bytes != layout.required_bytes) {
		throw nb::value_error(
		    "decode_into output buffer size does not match expected decoded size");
	}
	if (layout.required_bytes > 0 && view.buf == nullptr) {
		throw nb::value_error("decode_into output buffer is null");
	}

	auto out_span = std::span<std::uint8_t>(
	    static_cast<std::uint8_t*>(view.buf), actual_bytes);
	decode_layout_into(self, layout, out_span);
	return nb::borrow<nb::object>(out);
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
	if (!element) {
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

std::string generated_uid_to_string(const dicom::uid::Generated& uid) {
	const auto value = uid.value();
	return std::string(value.data(), value.size());
}

dicom::WriteOptions make_write_options(
    bool include_preamble, bool write_file_meta, bool keep_existing_meta) {
	dicom::WriteOptions options;
	options.include_preamble = include_preamble;
	options.write_file_meta = write_file_meta;
	options.keep_existing_meta = keep_existing_meta;
	return options;
}

nb::bytes to_python_bytes(std::vector<std::uint8_t>&& bytes) {
	if (bytes.empty()) {
		return nb::bytes("", 0);
	}
	return nb::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

nb::object generated_uid_or_none(std::optional<dicom::uid::Generated> uid) {
	if (!uid) {
		return nb::none();
	}
	return nb::str(uid->value().data(), uid->value().size());
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
	m.attr("UID_PREFIX") = nb::str(dicom::uid::uid_prefix().data(), dicom::uid::uid_prefix().size());
	m.attr("IMPLEMENTATION_CLASS_UID") = nb::str(
	    dicom::uid::implementation_class_uid().data(), dicom::uid::implementation_class_uid().size());
	m.attr("IMPLEMENTATION_VERSION_NAME") = nb::str(
	    dicom::uid::implementation_version_name().data(), dicom::uid::implementation_version_name().size());

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
		.def("__bool__",
		    [](const DataElement& element) {
			    return static_cast<bool>(element);
		    },
		    "True when the element is present (VR != None); False for missing lookups.")
		.def("is_present", &DataElement::is_present,
		    "True when lookup resolved to a real element (not missing sentinel).")
		.def("is_missing", &DataElement::is_missing,
		    "True when lookup resolved to a missing element sentinel (VR.None).")
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
		        return nb::cast(element.to_tag().value_or(nb::cast<Tag>(default_value)));
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
	        return nb::cast(element.to_int().value_or(nb::cast<int>(default_value)));
	    },
	    nb::arg("default") = nb::none(),
	    "Return int or None; optional default fills on failure")
	.def("to_long",
	    [](const DataElement& element, nb::object default_value) -> nb::object {
	        if (default_value.is_none()) {
	            auto v = element.to_long();
	            return v ? nb::cast(*v) : nb::none();
			    }
			    return nb::cast(element.to_long().value_or(nb::cast<long>(default_value)));
		    },
		    nb::arg("default") = nb::none(),
		    "Return int or None; optional default fills on failure")
		.def("to_longlong",
		    [](const DataElement& element, nb::object default_value) -> nb::object {
			    if (default_value.is_none()) {
				    auto v = element.to_longlong();
				    return v ? nb::cast(*v) : nb::none();
			    }
			    return nb::cast(
			        element.to_longlong().value_or(nb::cast<long long>(default_value)));
		    },
		    nb::arg("default") = nb::none())
		.def("to_double",
		    [](const DataElement& element, nb::object default_value) -> nb::object {
			    if (default_value.is_none()) {
				    auto v = element.to_double();
				    return v ? nb::cast(*v) : nb::none();
	        }
	        return nb::cast(element.to_double().value_or(nb::cast<double>(default_value)));
	    },
	    nb::arg("default") = nb::none())
	.def("to_int_vector",
	    [](const DataElement& element, nb::object default_value) -> nb::object {
	        if (default_value.is_none()) {
	            auto v = element.to_int_vector();
	            return v ? nb::cast(*v) : nb::none();
	        }
	        return nb::cast(
	            element.to_int_vector().value_or(nb::cast<std::vector<int>>(default_value)));
	    },
	    nb::arg("default") = nb::none())
	.def("as_uint16_vector",
	    [](const DataElement& element, nb::object default_value) -> nb::object {
	        if (default_value.is_none()) {
	            auto v = element.as_uint16_vector();
	            return v ? nb::cast(*v) : nb::none();
	        }
	        return nb::cast(element.as_uint16_vector().value_or(
	            nb::cast<std::vector<std::uint16_t>>(default_value)));
	    },
	    nb::arg("default") = nb::none(),
	    "Interpret raw value bytes as uint16 list (honors dataset endianness)")
	.def("as_uint8_vector",
	    [](const DataElement& element, nb::object default_value) -> nb::object {
	        if (default_value.is_none()) {
	            auto v = element.as_uint8_vector();
	            return v ? nb::cast(*v) : nb::none();
	        }
	        return nb::cast(element.as_uint8_vector().value_or(
	            nb::cast<std::vector<std::uint8_t>>(default_value)));
	    },
	    nb::arg("default") = nb::none(),
	    "Interpret raw value bytes as uint8 list")
	.def("to_long_vector",
	    [](const DataElement& element, nb::object default_value) -> nb::object {
	        if (default_value.is_none()) {
	            auto v = element.to_long_vector();
	            return v ? nb::cast(*v) : nb::none();
			    }
			    return nb::cast(
			        element.to_long_vector().value_or(nb::cast<std::vector<long>>(default_value)));
		    },
		    nb::arg("default") = nb::none())
		.def("to_longlong_vector",
		    [](const DataElement& element, nb::object default_value) -> nb::object {
			    if (default_value.is_none()) {
				    auto v = element.to_longlong_vector();
				    return v ? nb::cast(*v) : nb::none();
			    }
			    return nb::cast(element.to_longlong_vector().value_or(
			        nb::cast<std::vector<long long>>(default_value)));
		    },
		    nb::arg("default") = nb::none())
		.def("to_double_vector",
		    [](const DataElement& element, nb::object default_value) -> nb::object {
			    if (default_value.is_none()) {
				    auto v = element.to_double_vector();
				    return v ? nb::cast(*v) : nb::none();
			    }
			    return nb::cast(element.to_double_vector().value_or(
			        nb::cast<std::vector<double>>(default_value)));
		    },
		    nb::arg("default") = nb::none())
		.def("get_value",
		    [](DataElement& element) -> nb::object {
			    return dataelement_get_value_py(element);
		    },
		    "Best-effort typed access: returns int/float/str or list based on VR/VM; "
		    "falls back to raw bytes (memoryview) for binary VRs; "
		    "returns None for missing elements or sequences/pixel sequences.")
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
	    "- Missing lookups return a falsey DataElement (VR::None)")
		.def(nb::init<>())
	.def_prop_ro("path", &DataSet::path, "Identifier of the attached stream (file path, provided name, or '<memory>')")
		.def("size", &DataSet::size,
		    "Number of active DataElements currently available in this DataSet")
		.def("add_dataelement",
		    [](DataSet& self, const Tag& tag, std::optional<VR> vr,
		        std::size_t offset, std::size_t length) {
		        const VR resolved = vr.value_or(VR::None);
		        DataElement* element = self.add_dataelement(tag, resolved, offset, length);
		        if (!element) {
			        throw std::runtime_error("Failed to add DataElement");
		        }
		        return element;
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
		.def("dump", &DataSet::dump,
		    nb::arg("max_print_chars") = static_cast<std::size_t>(80),
		    nb::arg("include_offset") = true,
		    "Return a human-readable DataSet dump as text")
		.def("get_dataelement",
		    [](DataSet& self, const Tag& tag) -> DataElement& {
		        return *self.get_dataelement(tag);
		    },
		    nb::arg("tag"), nb::rv_policy::reference_internal,
		    "Return the DataElement for a tag; missing lookups return a falsey DataElement (VR::None)")
		.def("get_dataelement",
		    [](DataSet& self, std::uint32_t packed) -> DataElement& {
			    const Tag tag(packed);
			    return *self.get_dataelement(tag);
		    },
		    nb::arg("packed_tag"),
		    nb::rv_policy::reference_internal,
		    "Overload: pass packed 0xGGGEEEE integer; missing lookups return a falsey DataElement")
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
		    "Returns a DataElement; missing lookups return a falsey DataElement (VR::None); malformed paths raise.")
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

			    if (el->is_missing()) {
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
					    if (el->is_present()) {
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

	nb::class_<DicomFile>(m, "DicomFile",
	    "DICOM file/session object that owns the root DataSet.")
		.def_prop_ro("path", &DicomFile::path,
		    "Identifier of the attached root stream (file path or provided memory name)")
		.def_prop_ro("has_error", &DicomFile::has_error,
		    "True if parsing recorded any error diagnostics or threw while reading.")
		.def_prop_ro("error_message",
		    [](const DicomFile& self) -> nb::object {
			    if (!self.has_error() || self.error_message().empty()) {
				    return nb::none();
			    }
			    return nb::str(self.error_message().c_str(), self.error_message().size());
		    },
		    "Latest captured parse error message, or None when no error was recorded.")
		.def_prop_ro("dataset",
		    [](DicomFile& self) -> DataSet& { return self.dataset(); },
		    nb::rv_policy::reference_internal,
		    "Root DataSet owned by this DicomFile")
		.def("__len__",
		    [](DicomFile& self) { return self.dataset().size(); },
		    "Number of active DataElements currently available in the root DataSet")
		.def("dump", &DicomFile::dump,
		    nb::arg("max_print_chars") = static_cast<std::size_t>(80),
		    nb::arg("include_offset") = true,
		    "Return a human-readable root DataSet dump as text")
		.def("rebuild_file_meta",
		    [](DicomFile& self) { self.rebuild_file_meta(); },
		    "Rebuild file meta group (0002,eeee) with standard minimum fields.")
		.def("write_file",
		    [](DicomFile& self, const std::string& path, bool include_preamble,
		        bool write_file_meta, bool keep_existing_meta) {
			    self.write_file(
			        path, make_write_options(include_preamble, write_file_meta, keep_existing_meta));
		    },
		    nb::arg("path"),
		    nb::kw_only(),
		    nb::arg("include_preamble") = true,
		    nb::arg("write_file_meta") = true,
		    nb::arg("keep_existing_meta") = true,
		    "Write this DicomFile to `path`.")
		.def("write_bytes",
		    [](DicomFile& self, bool include_preamble, bool write_file_meta,
		        bool keep_existing_meta) {
			    return to_python_bytes(self.write_bytes(
			        make_write_options(
			                  include_preamble, write_file_meta, keep_existing_meta)));
		    },
		    nb::kw_only(),
		    nb::arg("include_preamble") = true,
		    nb::arg("write_file_meta") = true,
		    nb::arg("keep_existing_meta") = true,
		    "Serialize this DicomFile to bytes.")
		.def("__iter__",
		    [](DicomFile& self) {
			    return PyDataElementIterator(self.dataset());
		    },
		    nb::keep_alive<0, 1>(),
		    "Iterate over DataElements from the root DataSet")
		.def("pixel_data",
		    [](const DicomFile& self, std::size_t frame_index) {
			    const auto decoded = self.pixel_data(frame_index);
			    if (decoded.empty()) {
				    return nb::bytes("", 0);
			    }
			    return nb::bytes(reinterpret_cast<const char*>(decoded.data()), decoded.size());
		    },
		    nb::arg("frame_index") = 0,
		    "Decode one frame with default options and return decoded bytes.")
		.def("to_array",
		    &dicomfile_to_array,
		    nb::arg("frame") = -1,
		    nb::arg("scaled") = false,
		    nb::arg("htj2k_decoder") = "auto",
		    "Decode pixel samples and return a NumPy array.\n"
		    "\n"
		    "Parameters\n"
		    "----------\n"
		    "frame : int, optional\n"
		    "    -1 decodes all frames (multi-frame only), otherwise decode the selected frame index.\n"
		    "scaled : bool, optional\n"
		    "    If True, apply Modality LUT/Rescale when available.\n"
		    "    Scaled output is ignored when SamplesPerPixel != 1, or when both\n"
		    "    Modality LUT Sequence and Rescale Slope/Intercept are absent.\n"
		    "htj2k_decoder : {'auto', 'openjph', 'openjpeg'}, optional\n"
		    "    HTJ2K backend selection. 'auto' prefers OpenJPH then falls back to OpenJPEG.\n"
		    "\n"
		    "Returns\n"
		    "-------\n"
		    "numpy.ndarray\n"
		    "    Shape is (rows, cols) or (rows, cols, samples) for a single frame,\n"
		    "    and (frames, rows, cols) or (frames, rows, cols, samples) when decoding all frames.")
		.def("to_array_view",
		    &dicomfile_to_array_view,
		    nb::arg("frame") = -1,
		    "Return a zero-copy read-only NumPy view over uncompressed source pixel data.\n"
		    "\n"
		    "This requires:\n"
		    "- uncompressed transfer syntax\n"
		    "- frame layout compatible with interleaved output\n"
		    "- source endianness matching host endianness for sample sizes > 1.")
		.def("decode_into",
		    &dicomfile_decode_into_array,
		    nb::arg("out"),
		    nb::arg("frame") = 0,
		    nb::arg("scaled") = false,
		    nb::arg("threads") = -1,
		    nb::arg("htj2k_decoder") = "auto",
		    "Decode pixel samples into an existing writable C-contiguous buffer.\n"
		    "\n"
		    "Parameters\n"
		    "----------\n"
		    "out : buffer-like\n"
		    "    Destination NumPy array or writable contiguous buffer. Size must\n"
		    "    exactly match the decoded output for the requested frame selection.\n"
		    "frame : int, optional\n"
		    "    -1 decodes all frames (multi-frame only), otherwise decode the selected frame index.\n"
		    "scaled : bool, optional\n"
		    "    If True, apply Modality LUT/Rescale when available.\n"
		    "threads : int, optional\n"
		    "    Decoder thread count hint.\n"
		    "    Default is -1 (use all CPUs).\n"
		    "    0 uses library default, -1 uses all CPUs, >0 sets explicit thread count.\n"
		    "    Currently applied to JPEG 2000; unsupported decoders may ignore it.\n"
		    "htj2k_decoder : {'auto', 'openjph', 'openjpeg'}, optional\n"
		    "    HTJ2K backend selection. 'auto' prefers OpenJPH then falls back to OpenJPEG.\n"
		    "\n"
		    "Returns\n"
		    "-------\n"
		    "Same object as `out` for call chaining.")
		.def("pixel_array",
		    &dicomfile_to_array,
		    nb::arg("frame") = -1,
		    nb::arg("scaled") = false,
		    nb::arg("htj2k_decoder") = "auto",
		    "Alias of to_array(frame=-1, scaled=False, htj2k_decoder='auto').")
		.def("__getitem__",
		    [](DicomFile& self, nb::object key) -> nb::object {
			    DataSet& dataset = self.dataset();
			    DataElement* el = nullptr;

			    if (nb::isinstance<Tag>(key)) {
				    el = dataset.get_dataelement(nb::cast<Tag>(key));
			    } else if (nb::isinstance<nb::int_>(key)) {
				    el = dataset.get_dataelement(Tag(nb::cast<std::uint32_t>(key)));
			    } else if (nb::isinstance<nb::str>(key)) {
				    el = dataset.get_dataelement(nb::cast<std::string>(key));
			    } else {
				    throw nb::type_error("DicomFile indices must be Tag, int (0xGGGEEEE), or str");
			    }

			    if (el->is_missing()) {
				    return nb::none();
			    }
			    return dataelement_get_value_py(*el, nb::cast(&dataset, nb::rv_policy::reference));
		    },
		    nb::arg("key"),
		    "Index syntax delegated to root DataSet")
		.def("__getattr__",
		    [](DicomFile& self, const std::string& name) -> nb::object {
			    nb::object owner = nb::cast(&self, nb::rv_policy::reference);
			    nb::object dataset_obj =
			        nb::cast(&self.dataset(), nb::rv_policy::reference_internal, owner);
			    return nb::getattr(dataset_obj, nb::str(name.c_str(), name.size()));
		    },
		    nb::arg("name"),
		    "Forward unknown attributes/methods to the root DataSet.")
		.def("__dir__",
		    [](DicomFile& self) {
			    nb::object self_obj = nb::cast(&self, nb::rv_policy::reference);
			    PyObject* type_obj = reinterpret_cast<PyObject*>(Py_TYPE(self_obj.ptr()));
			    nb::list result = nb::steal<nb::list>(PyObject_Dir(type_obj));  // class attrs

			    std::unordered_set<std::string> seen;
			    for (nb::handle item : result) {
				    seen.insert(nb::cast<std::string>(item));
			    }

			    nb::object dataset_obj =
			        nb::cast(&self.dataset(), nb::rv_policy::reference_internal, self_obj);
			    nb::list dataset_dir = nb::cast<nb::list>(nb::getattr(dataset_obj, "__dir__")());
			    for (nb::handle item : dataset_dir) {
				    std::string name = nb::cast<std::string>(item);
				    if (seen.insert(name).second) {
					    result.append(nb::str(name.c_str(), name.size()));
				    }
			    }
			    return result;
		    },
		    "dir() includes DicomFile attributes plus forwarded root DataSet attributes/keywords.")
		.def("__repr__",
		    [](DicomFile& self) {
			    std::ostringstream oss;
			    oss << "DicomFile(path='" << self.path() << "')";
			    return oss.str();
		    });

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
    "Read a DICOM file from disk and return a DicomFile.\n"
    "\n"
    "Parameters\n"
    "----------\n"
    "path : str\n"
    "    Filesystem path to the DICOM Part 10 file.\n"
    "load_until : Tag | None, optional\n"
    "    Stop after this tag is read (inclusive). Defaults to reading entire file.\n"
    "keep_on_error : bool | None, optional\n"
    "    When True, keep partially read data instead of raising on parse errors.\n"
    "    Inspect DicomFile.has_error and DicomFile.error_message after reading.\n");

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

        std::unique_ptr<dicom::DicomFile> file;
        if (copy || total == 0) {
	        std::vector<std::uint8_t> owned(total);
	        if (total > 0) {
		        std::memcpy(owned.data(), info.buf, total);
	        }
	        file = dicom::read_bytes(std::string{name}, std::move(owned), opts);
        } else {
	        if (elem_size != 1) {
		        throw std::invalid_argument("read_bytes(copy=False) requires a byte-oriented buffer");
	        }
	        file = dicom::read_bytes(std::string{name},
	            static_cast<const std::uint8_t*>(info.buf), total, opts);
        }

        nb::object py_file = nb::cast(std::move(file));
        if (!copy && total > 0) {
	        py_file.attr("_buffer_owner") = buffer;
        }
        return py_file;
    },
    nb::arg("data"),
    nb::arg("name") = std::string{"<memory>"},
    nb::arg("load_until") = nb::none(),
    nb::arg("keep_on_error") = nb::none(),
    nb::arg("copy") = true,
    "Read a DicomFile from a bytes-like object. Parsing is eager up to `load_until`.\n"
    "\n"
    "Parameters\n"
    "----------\n"
    "data : buffer\n"
    "    1-D bytes-like object containing the Part 10 stream (or raw stream).\n"
    "name : str, optional\n"
    "    Identifier reported by DicomFile.path() and diagnostics. Default '<memory>'.\n"
    "load_until : Tag | None, optional\n"
    "    Stop after this tag is read (inclusive). Defaults to reading entire buffer.\n"
    "keep_on_error : bool | None, optional\n"
    "    When True, keep partially read data instead of raising on parse errors.\n"
    "    Inspect DicomFile.has_error and DicomFile.error_message after reading.\n"
    "copy : bool, optional\n"
    "    When False, avoid copying and reference the caller's buffer; caller must keep\n"
    "    the buffer alive while the DicomFile exists.\n"
    "\n"
    "Warning\n"
    "-------\n"
    "When copy=False, the source buffer must remain alive for as long as the returned DicomFile;\n"
    "the binding keeps a Python reference, but mutating or freeing the underlying memory can\n"
    "still corrupt the dataset.");

m.def("load_root_elements_reserve_hint",
    []() {
	    return dicom::load_root_elements_reserve_hint();
    },
    "Return the current adaptive reserve hint used by root DataSet parsing.");

m.def("reset_root_elements_reserve_hint",
    []() {
	    dicom::reset_root_elements_reserve_hint();
    },
    "Reset adaptive root DataSet reserve hint to its initial value.");

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
			    try {
				    new (self) Uid(dicom::uid::lookup_or_throw(text));
			    } catch (const std::invalid_argument&) {
				    std::ostringstream oss;
				    oss << "Unknown DICOM UID from Uid.__init__: " << text;
				    const std::string message = oss.str();
				    throw nb::value_error(message.c_str());
			    }
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

m.def("uid_prefix",
    []() {
	    const auto value = dicom::uid::uid_prefix();
	    return std::string(value.data(), value.size());
    },
    "Return DICOMSDL UID root prefix.");

m.def("implementation_class_uid",
    []() {
	    const auto value = dicom::uid::implementation_class_uid();
	    return std::string(value.data(), value.size());
    },
    "Return default Implementation Class UID used by DICOMSDL.");

m.def("implementation_version_name",
    []() {
	    const auto value = dicom::uid::implementation_version_name();
	    return std::string(value.data(), value.size());
    },
    "Return default Implementation Version Name used by DICOMSDL.");

m.def("is_valid_uid_text_strict",
    &dicom::uid::is_valid_uid_text_strict,
    nb::arg("text"),
    "Return True if the UID string is valid under strict DICOM UID rules.");

m.def("make_uid_with_suffix",
    [] (std::uint64_t suffix, std::optional<std::string> root) -> nb::object {
	    if (root) {
		    return generated_uid_or_none(dicom::uid::make_uid_with_suffix(*root, suffix));
	    }
	    return generated_uid_or_none(dicom::uid::make_uid_with_suffix(suffix));
    },
    nb::arg("suffix"),
    nb::arg("root") = nb::none(),
    "Build '<root>.<suffix>' and return it when valid; returns None on failure.");

m.def("try_append_uid",
    [] (const std::string& base_uid, std::uint64_t component) -> nb::object {
	    auto generated = dicom::uid::make_generated(base_uid);
	    if (!generated) {
		    return nb::none();
	    }
	    return generated_uid_or_none(generated->try_append(component));
    },
    nb::arg("base_uid"),
    nb::arg("component"),
    "Try to append one UID component with fallback policy; returns None on failure.");

m.def("append_uid",
    [] (const std::string& base_uid, std::uint64_t component) -> std::string {
	    auto generated = dicom::uid::make_generated(base_uid);
	    if (!generated) {
		    throw nb::value_error("Invalid base UID");
	    }
	    return generated_uid_to_string(generated->append(component));
    },
    nb::arg("base_uid"),
    nb::arg("component"),
    "Append one UID component with fallback policy; raises ValueError/RuntimeError on failure.");

m.def("try_generate_uid",
    []() -> nb::object {
	    return generated_uid_or_none(dicom::uid::try_generate_uid());
    },
    "Generate a UID under DICOMSDL prefix; returns None on failure.");

m.def("generate_uid",
    []() {
	    return generated_uid_to_string(dicom::uid::generate_uid());
    },
    "Generate a UID under DICOMSDL prefix. Raises RuntimeError on failure.");

m.def("generate_sop_instance_uid",
    []() {
	    return generated_uid_to_string(dicom::uid::generate_sop_instance_uid());
    },
    "Generate a SOP Instance UID under DICOMSDL prefix.");

m.def("generate_series_instance_uid",
    []() {
	    return generated_uid_to_string(dicom::uid::generate_series_instance_uid());
    },
    "Generate a Series Instance UID under DICOMSDL prefix.");

m.def("generate_study_instance_uid",
    []() {
	    return generated_uid_to_string(dicom::uid::generate_study_instance_uid());
    },
    "Generate a Study Instance UID under DICOMSDL prefix.");

	m.attr("__all__") = nb::make_tuple(
	    "LogLevel",
	    "Reporter",
	    "StderrReporter",
	    "FileReporter",
	    "BufferingReporter",
	    "DICOM_STANDARD_VERSION",
	    "DICOMSDL_VERSION",
	    "__version__",
	    "UID_PREFIX",
	    "IMPLEMENTATION_CLASS_UID",
	    "IMPLEMENTATION_VERSION_NAME",
	    "log_info",
	    "log_warn",
	    "log_error",
	    "set_default_reporter",
	    "set_thread_reporter",
	    "set_log_level",
	    "DicomFile",
	    "DataSet",
	    "Tag",
	    "VR",
	    "Uid",
	    "read_file",
	    "read_bytes",
	    "load_root_elements_reserve_hint",
	    "reset_root_elements_reserve_hint",
	    "keyword_to_tag_vr",
	    "tag_to_keyword",
	    "tag_to_entry",
	    "lookup_uid",
	    "uid_from_value",
	    "uid_from_keyword",
	    "uid_prefix",
	    "implementation_class_uid",
	    "implementation_version_name",
	    "is_valid_uid_text_strict",
	    "make_uid_with_suffix",
	    "try_append_uid",
	    "append_uid",
	    "try_generate_uid",
	    "generate_uid",
	    "generate_sop_instance_uid",
	    "generate_series_instance_uid",
	    "generate_study_instance_uid");
}
