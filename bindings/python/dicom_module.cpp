#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <memory>
#include <new>
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
#include <photometric_text_detail.hpp>
#include "pixel/host/adapter/host_adapter.hpp"
#include "pixel/host/support/dicom_pixel_support.hpp"
#include "pixel/runtime/runtime_registry.hpp"

namespace nb = nanobind;

using dicom::DataSet;
using dicom::DicomFile;
using dicom::DataElement;
using dicom::Sequence;
using dicom::Tag;
using dicom::VR;
using dicom::uid::WellKnown;
using Uid = dicom::uid::WellKnown;
using EncoderContext = dicom::pixel::EncoderContext;
namespace diag = dicom::diag;

namespace {

std::string_view vr_to_string_view(const VR& vr);
nb::object readonly_memoryview_from_span(
    const void* data, std::size_t size, nb::handle owner = nb::handle());
std::vector<std::uint8_t> pybuffer_to_bytes(nb::handle value);

PyObject* new_reference_or_null(PyObject* obj) noexcept {
	if (obj == nullptr) {
		return nullptr;
	}
#if PY_VERSION_HEX >= 0x030A0000
	return Py_NewRef(obj);
#else
	Py_INCREF(obj);
	return obj;
#endif
}

std::filesystem::path python_path_to_filesystem_path(nb::handle value, const char* arg_name) {
	PyObject* fs_path = PyOS_FSPath(value.ptr());
	if (fs_path == nullptr) {
		throw nb::python_error();
	}

	nb::object path_obj = nb::steal<nb::object>(fs_path);
	if (PyUnicode_Check(path_obj.ptr())) {
#if defined(_WIN32)
		Py_ssize_t size = 0;
		wchar_t* data = PyUnicode_AsWideCharString(path_obj.ptr(), &size);
		if (data == nullptr) {
			throw nb::python_error();
		}
		std::filesystem::path path(std::wstring(data, static_cast<std::size_t>(size)));
		PyMem_Free(data);
		return path;
#else
		return std::filesystem::path(nb::cast<std::string>(path_obj));
#endif
	}

	if (PyBytes_Check(path_obj.ptr())) {
		char* data = nullptr;
		Py_ssize_t size = 0;
		if (PyBytes_AsStringAndSize(path_obj.ptr(), &data, &size) != 0) {
			throw nb::python_error();
		}
		return std::filesystem::path(std::string(data, static_cast<std::size_t>(size)));
	}

	std::string message = arg_name;
	message += " must be str, bytes, or os.PathLike";
	throw nb::type_error(message.c_str());
}

bool is_selection_tag_like_py(nb::handle value) {
	return nb::isinstance<Tag>(value) || nb::isinstance<nb::int_>(value) ||
	       nb::isinstance<nb::str>(value);
}

bool is_non_string_sequence_like_py(nb::handle value) {
	if (PyUnicode_Check(value.ptr()) || PyBytes_Check(value.ptr()) ||
	    PyByteArray_Check(value.ptr()) || PyMemoryView_Check(value.ptr())) {
		return false;
	}
	return PySequence_Check(value.ptr()) != 0;
}

Tag selection_tag_from_py(nb::handle value, const char* context) {
	if (nb::isinstance<Tag>(value)) {
		return nb::cast<Tag>(value);
	}
	if (nb::isinstance<nb::int_>(value)) {
		return Tag(nb::cast<std::uint32_t>(value));
	}
	if (nb::isinstance<nb::str>(value)) {
		return Tag(nb::cast<std::string>(value));
	}
	std::string message = context;
	message += " tags must be Tag, int (0xGGGGEEEE), or str";
	throw nb::type_error(message.c_str());
}

dicom::DataSetSelectionNode selection_node_from_py(nb::handle value, const char* context);

std::vector<dicom::DataSetSelectionNode> selection_nodes_from_py(
    nb::handle value, const char* context) {
	if (!is_non_string_sequence_like_py(value)) {
		std::string message = context;
		message += " must be a sequence of selection nodes";
		throw nb::type_error(message.c_str());
	}

	nb::sequence seq = nb::borrow<nb::sequence>(value);
	const std::size_t size = static_cast<std::size_t>(PySequence_Size(seq.ptr()));
	std::vector<dicom::DataSetSelectionNode> nodes;
	nodes.reserve(size);
	for (std::size_t index = 0; index < size; ++index) {
		nodes.push_back(selection_node_from_py(seq[index], context));
	}
	return nodes;
}

dicom::DataSetSelectionNode selection_node_from_py(nb::handle value, const char* context) {
	if (is_selection_tag_like_py(value)) {
		return dicom::DataSetSelectionNode(selection_tag_from_py(value, context));
	}

	if (!is_non_string_sequence_like_py(value)) {
		std::string message = context;
		message += " nodes must be Tag/int/str leaves or (tag, children) pairs";
		throw nb::type_error(message.c_str());
	}

	nb::sequence seq = nb::borrow<nb::sequence>(value);
	const auto size = static_cast<std::size_t>(PySequence_Size(seq.ptr()));
	if (size != 2) {
		std::string message = context;
		message += " nested nodes must be 2-item sequences: (tag, children)";
		throw nb::type_error(message.c_str());
	}

	nb::handle tag_value = seq[0];
	nb::handle children_value = seq[1];
	if (!is_selection_tag_like_py(tag_value)) {
		std::string message = context;
		message += " nested node tag must be Tag, int (0xGGGGEEEE), or str";
		throw nb::type_error(message.c_str());
	}
	if (!is_non_string_sequence_like_py(children_value)) {
		std::string message = context;
		message += " nested node children must be a sequence of selection nodes";
		throw nb::type_error(message.c_str());
	}

	return dicom::DataSetSelectionNode(
	    selection_tag_from_py(tag_value, context),
	    selection_nodes_from_py(children_value, context));
}

dicom::DataSetSelection selection_from_py(nb::handle value, const char* context) {
	if (nb::isinstance<dicom::DataSetSelection>(value)) {
		return nb::cast<const dicom::DataSetSelection&>(value);
	}
	return dicom::DataSetSelection(selection_nodes_from_py(value, context));
}

bool transfer_syntax_has_runtime_encode_support(Uid uid) noexcept {
	if (!uid.valid() || uid.uid_type() != dicom::UidType::TransferSyntax) {
		return false;
	}

	uint32_t codec_profile_code = PIXEL_CODEC_PROFILE_UNKNOWN;
	if (!::pixel::runtime::codec_profile_code_from_transfer_syntax(
	        uid, &codec_profile_code)) {
		return false;
	}

#if defined(DICOMSDL_PIXEL_RUNTIME_ENABLED)
	const auto* registry = ::pixel::runtime::current_registry();
	if (registry == nullptr) {
		return false;
	}
	const auto* binding = registry->find_encoder_binding(codec_profile_code);
	return binding != nullptr &&
	       binding->binding_kind != ::pixel::runtime::EncoderBindingKind::kNone;
#else
	return codec_profile_code == PIXEL_CODEC_PROFILE_NATIVE_UNCOMPRESSED;
#endif
}

nb::object empty_py_list() {
	return nb::steal<nb::object>(PyList_New(0));
}

template <typename T>
nb::object vector_to_py_list(const std::vector<T>& values) {
	nb::object out = empty_py_list();
	for (const auto& value : values) {
		nb::object py_value = nb::cast(value);
		if (PyList_Append(out.ptr(), py_value.ptr()) != 0) {
			throw nb::python_error();
		}
	}
	return out;
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
	dicom::pixel::DecodePlan plan{};
	std::array<std::size_t, 4> shape{};
	std::array<std::int64_t, 4> strides{};
	std::size_t ndim{0};
	std::size_t frame_stride{0};
	std::size_t frames{0};
	std::size_t frame_index{0};
	std::size_t required_bytes{0};
	bool decode_all_frames{false};
};

struct PixelArrayLayout {
	DecodedArraySpec spec{};
	std::array<std::size_t, 4> shape{};
	std::array<std::int64_t, 4> strides{};
	std::size_t ndim{0};
	std::size_t required_bytes{0};
};

struct DirectRawArrayAccess {
	DecodedArrayLayout layout{};
	std::span<const std::uint8_t> source_bytes{};
	std::size_t byte_offset{0};
};

DecodedArraySpec decoded_array_spec(dicom::pixel::DataType data_type) {
	switch (data_type) {
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

DecodedArraySpec decoded_array_spec(const dicom::pixel::PixelLayout& layout) {
	return decoded_array_spec(layout.data_type);
}

DecodedArraySpec decoded_array_spec(const dicom::pixel::DecodePlan& plan) {
	return decoded_array_spec(plan.output_layout);
}

nb::object numpy_dtype_object(dicom::pixel::DataType data_type) {
	const auto numpy = nb::module_::import_("numpy");

	switch (data_type) {
	case dicom::pixel::DataType::u8:
		return numpy.attr("dtype")("uint8");
	case dicom::pixel::DataType::s8:
		return numpy.attr("dtype")("int8");
	case dicom::pixel::DataType::u16:
		return numpy.attr("dtype")("uint16");
	case dicom::pixel::DataType::s16:
		return numpy.attr("dtype")("int16");
	case dicom::pixel::DataType::u32:
		return numpy.attr("dtype")("uint32");
	case dicom::pixel::DataType::s32:
		return numpy.attr("dtype")("int32");
	case dicom::pixel::DataType::f32:
		return numpy.attr("dtype")("float32");
	case dicom::pixel::DataType::f64:
		return numpy.attr("dtype")("float64");
	default:
		break;
	}

	throw nb::value_error("decode plan does not describe a supported NumPy dtype");
}

nb::object numpy_dtype_object(const dicom::pixel::DecodePlan& plan) {
	return numpy_dtype_object(plan.output_layout.data_type);
}

std::string_view encoded_lossy_state_name(
    dicom::pixel::EncodedLossyState state) noexcept {
	switch (state) {
	case dicom::pixel::EncodedLossyState::lossless:
		return "lossless";
	case dicom::pixel::EncodedLossyState::lossy:
		return "lossy";
	case dicom::pixel::EncodedLossyState::near_lossless:
		return "near_lossless";
	case dicom::pixel::EncodedLossyState::unknown:
	default:
		return "unknown";
	}
}

std::string_view photometric_name(dicom::pixel::Photometric photometric) noexcept {
	switch (photometric) {
	case dicom::pixel::Photometric::monochrome1:
		return "monochrome1";
	case dicom::pixel::Photometric::monochrome2:
		return "monochrome2";
	case dicom::pixel::Photometric::palette_color:
		return "palette_color";
	case dicom::pixel::Photometric::rgb:
		return "rgb";
	case dicom::pixel::Photometric::ybr_full:
		return "ybr_full";
	case dicom::pixel::Photometric::ybr_full_422:
		return "ybr_full_422";
	case dicom::pixel::Photometric::ybr_rct:
		return "ybr_rct";
	case dicom::pixel::Photometric::ybr_ict:
		return "ybr_ict";
	case dicom::pixel::Photometric::ybr_partial_420:
		return "ybr_partial_420";
	case dicom::pixel::Photometric::xyb:
		return "xyb";
	case dicom::pixel::Photometric::hsv:
		return "hsv";
	case dicom::pixel::Photometric::argb:
		return "argb";
	case dicom::pixel::Photometric::cmyk:
		return "cmyk";
	case dicom::pixel::Photometric::ybr_partial_422:
		return "ybr_partial_422";
	default:
		return "unknown";
	}
}

std::string_view data_type_name(dicom::pixel::DataType data_type) noexcept {
	switch (data_type) {
	case dicom::pixel::DataType::u8:
		return "uint8";
	case dicom::pixel::DataType::s8:
		return "int8";
	case dicom::pixel::DataType::u16:
		return "uint16";
	case dicom::pixel::DataType::s16:
		return "int16";
	case dicom::pixel::DataType::u32:
		return "uint32";
	case dicom::pixel::DataType::s32:
		return "int32";
	case dicom::pixel::DataType::f32:
		return "float32";
	case dicom::pixel::DataType::f64:
		return "float64";
	case dicom::pixel::DataType::unknown:
	default:
		return "unknown";
	}
}

nb::object uid_to_object(Uid uid) {
	if (!uid.valid()) {
		return nb::none();
	}
	return nb::cast(uid);
}

nb::tuple shape_tuple_from_layout(const DecodedArrayLayout& layout) {
	nb::tuple shape = nb::steal<nb::tuple>(PyTuple_New(static_cast<Py_ssize_t>(layout.ndim)));
	for (std::size_t i = 0; i < layout.ndim; ++i) {
		PyObject* item = PyLong_FromSize_t(layout.shape[i]);
		if (item == nullptr) {
			throw nb::python_error();
		}
		if (PyTuple_SetItem(shape.ptr(), static_cast<Py_ssize_t>(i), item) < 0) {
			Py_DECREF(item);
			throw nb::python_error();
		}
	}
	return shape;
}

struct NormalizedArrayGeometry {
	std::array<std::size_t, 4> shape{};
	std::array<std::int64_t, 4> strides{};
	std::size_t ndim{0};
};

NormalizedArrayGeometry build_normalized_array_geometry_or_throw(
    const dicom::pixel::PixelLayout& pixel_layout, std::size_t bytes_per_sample,
    bool decode_all_frames) {
	NormalizedArrayGeometry geometry{};
	const auto rows = static_cast<std::size_t>(pixel_layout.rows);
	const auto cols = static_cast<std::size_t>(pixel_layout.cols);
	const auto frames = static_cast<std::size_t>(pixel_layout.frames);
	const auto samples_per_pixel =
	    static_cast<std::size_t>(pixel_layout.samples_per_pixel);
	const auto row_stride_elems = pixel_layout.row_stride / bytes_per_sample;
	const auto frame_stride_elems = pixel_layout.frame_stride / bytes_per_sample;
	const auto planar_multisample =
	    pixel_layout.planar == dicom::pixel::Planar::planar && samples_per_pixel > 1;

	std::size_t plane_stride_elems = 0;
	if (planar_multisample) {
		if ((frame_stride_elems % samples_per_pixel) != 0) {
			throw nb::value_error(
			    "pixel layout planar frame stride is not divisible by samples_per_pixel");
		}
		plane_stride_elems = frame_stride_elems / samples_per_pixel;
	}

	if (!decode_all_frames) {
		if (samples_per_pixel == 1) {
			geometry.ndim = 2;
			geometry.shape[0] = rows;
			geometry.shape[1] = cols;
			geometry.strides[0] = static_cast<std::int64_t>(row_stride_elems);
			geometry.strides[1] = 1;
		} else if (planar_multisample) {
			// Expose planar decode output as plane-first so Python callers can
			// use a standard C-contiguous array for decode_into(plan=...).
			geometry.ndim = 3;
			geometry.shape[0] = samples_per_pixel;
			geometry.shape[1] = rows;
			geometry.shape[2] = cols;
			geometry.strides[0] = static_cast<std::int64_t>(plane_stride_elems);
			geometry.strides[1] = static_cast<std::int64_t>(row_stride_elems);
			geometry.strides[2] = 1;
		} else {
			geometry.ndim = 3;
			geometry.shape[0] = rows;
			geometry.shape[1] = cols;
			geometry.shape[2] = samples_per_pixel;
			geometry.strides[0] = static_cast<std::int64_t>(row_stride_elems);
			geometry.strides[1] = static_cast<std::int64_t>(samples_per_pixel);
			geometry.strides[2] = 1;
		}
		return geometry;
	}

	if (samples_per_pixel == 1) {
		geometry.ndim = 3;
		geometry.shape[0] = frames;
		geometry.shape[1] = rows;
		geometry.shape[2] = cols;
		geometry.strides[0] = static_cast<std::int64_t>(frame_stride_elems);
		geometry.strides[1] = static_cast<std::int64_t>(row_stride_elems);
		geometry.strides[2] = 1;
	} else if (planar_multisample) {
		geometry.ndim = 4;
		geometry.shape[0] = frames;
		geometry.shape[1] = samples_per_pixel;
		geometry.shape[2] = rows;
		geometry.shape[3] = cols;
		geometry.strides[0] = static_cast<std::int64_t>(frame_stride_elems);
		geometry.strides[1] = static_cast<std::int64_t>(plane_stride_elems);
		geometry.strides[2] = static_cast<std::int64_t>(row_stride_elems);
		geometry.strides[3] = 1;
	} else {
		geometry.ndim = 4;
		geometry.shape[0] = frames;
		geometry.shape[1] = rows;
		geometry.shape[2] = cols;
		geometry.shape[3] = samples_per_pixel;
		geometry.strides[0] = static_cast<std::int64_t>(frame_stride_elems);
		geometry.strides[1] = static_cast<std::int64_t>(row_stride_elems);
		geometry.strides[2] = static_cast<std::int64_t>(samples_per_pixel);
		geometry.strides[3] = 1;
	}
	return geometry;
}

PixelArrayLayout build_array_layout_from_pixel_layout(
    const dicom::pixel::PixelLayout& pixel_layout) {
	PixelArrayLayout layout{};
	if (pixel_layout.empty()) {
		throw nb::value_error("pixel transform output layout is empty");
	}
	if (pixel_layout.rows == 0 || pixel_layout.cols == 0 ||
	    pixel_layout.samples_per_pixel == 0 || pixel_layout.frames == 0) {
		throw nb::value_error("pixel transform output layout has invalid geometry");
	}

	layout.spec = decoded_array_spec(pixel_layout);
	const auto bytes_per_sample = layout.spec.bytes_per_sample;
	if (bytes_per_sample == 0) {
		throw nb::value_error("pixel transform output layout has invalid sample size");
	}
	if ((pixel_layout.row_stride % bytes_per_sample) != 0 ||
	    (pixel_layout.frame_stride % bytes_per_sample) != 0) {
		throw nb::value_error("pixel transform output strides are not aligned to sample size");
	}

	const auto frames = static_cast<std::size_t>(pixel_layout.frames);
	if (frames != 0 &&
	    pixel_layout.frame_stride > (std::numeric_limits<std::size_t>::max() / frames)) {
		throw std::overflow_error("pixel transform output size overflow");
	}
	layout.required_bytes = pixel_layout.frame_stride * frames;
	const auto geometry =
	    build_normalized_array_geometry_or_throw(pixel_layout, bytes_per_sample, frames > 1);
	layout.ndim = geometry.ndim;
	layout.shape = geometry.shape;
	layout.strides = geometry.strides;
	return layout;
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

nb::object numpy_array_from_owned_pixel_buffer(dicom::pixel::PixelBuffer&& buffer) {
	auto* owner = new dicom::pixel::PixelBuffer(std::move(buffer));
	const auto layout = build_array_layout_from_pixel_layout(owner->layout);
	void* data_ptr = owner->bytes.empty() ? nullptr : static_cast<void*>(owner->bytes.data());
	nb::capsule owner_capsule(owner, [](void* ptr) noexcept {
		delete static_cast<dicom::pixel::PixelBuffer*>(ptr);
	});
	return nb::cast(nb::ndarray<nb::numpy>(
	    data_ptr, layout.ndim, layout.shape.data(), owner_capsule,
	    layout.strides.data(), layout.spec.dtype, nb::device::cpu::value, 0, 'C'));
}

std::string normalize_encoder_option_name(std::string option) {
	std::string normalized{};
	normalized.reserve(option.size());
	for (const unsigned char ch : option) {
		if (ch == '_' || ch == '-' || ch == ' ' || ch == '\t') {
			continue;
		}
		normalized.push_back(static_cast<char>(std::tolower(ch)));
	}
	return normalized;
}

dicom::pixel::Htj2kDecoderBackend parse_htj2k_decoder_backend(
    std::string_view text) {
	std::string normalized{};
	normalized.reserve(text.size());
	for (const unsigned char ch : text) {
		if (ch == '_' || ch == '-' || ch == ' ' || ch == '\t') {
			continue;
		}
		normalized.push_back(static_cast<char>(std::tolower(ch)));
	}

	if (normalized.empty() || normalized == "auto") {
		return dicom::pixel::Htj2kDecoderBackend::auto_select;
	}
	if (normalized == "openjph") {
		return dicom::pixel::Htj2kDecoderBackend::openjph;
	}
	if (normalized == "openjpeg") {
		return dicom::pixel::Htj2kDecoderBackend::openjpeg;
	}

	throw nb::value_error(
	    "htj2k decoder backend must be one of: auto, openjph, openjpeg");
}

std::string_view htj2k_decoder_backend_name(
    dicom::pixel::Htj2kDecoderBackend backend) {
	switch (backend) {
	case dicom::pixel::Htj2kDecoderBackend::openjph:
		return "openjph";
	case dicom::pixel::Htj2kDecoderBackend::openjpeg:
		return "openjpeg";
	case dicom::pixel::Htj2kDecoderBackend::auto_select:
	default:
		return "auto";
	}
}

struct CodecOptionTextStorage {
	bool auto_mode{false};
	struct Entry {
		std::string key;
		std::string value;
	};
	std::vector<Entry> entries{};
	std::vector<dicom::pixel::CodecOptionTextKv> items{};

	void add(std::string_view key, std::string value) {
		entries.push_back(Entry{std::string(key), std::move(value)});
	}

	void finalize() {
		items.clear();
		items.reserve(entries.size());
		for (const auto& entry : entries) {
			items.push_back(dicom::pixel::CodecOptionTextKv{
			    .key = entry.key,
			    .value = entry.value,
			});
		}
	}

	[[nodiscard]] std::span<const dicom::pixel::CodecOptionTextKv> span() const {
		return std::span<const dicom::pixel::CodecOptionTextKv>(
		    items.data(), items.size());
	}
};

class PyReadOnlyBufferView {
public:
	explicit PyReadOnlyBufferView(nb::handle obj) {
		const int flags = PyBUF_C_CONTIGUOUS | PyBUF_FORMAT | PyBUF_ND;
		if (PyObject_GetBuffer(obj.ptr(), &view_, flags) != 0) {
			throw nb::type_error(
			    "set_pixel_data expects a C-contiguous buffer object");
		}
	}

	~PyReadOnlyBufferView() { PyBuffer_Release(&view_); }

	PyReadOnlyBufferView(const PyReadOnlyBufferView&) = delete;
	PyReadOnlyBufferView& operator=(const PyReadOnlyBufferView&) = delete;

	[[nodiscard]] const Py_buffer& view() const noexcept { return view_; }

private:
	Py_buffer view_{};
};

[[nodiscard]] std::string_view strip_pep3118_endianness_prefix(
    std::string_view format) noexcept {
	if (format.empty()) {
		return format;
	}
	switch (format.front()) {
	case '@':
	case '=':
	case '<':
	case '>':
	case '!':
		return format.substr(1);
	default:
		return format;
	}
}

[[nodiscard]] dicom::pixel::DataType parse_pixel_source_data_type_or_throw(
    std::string_view format, std::size_t itemsize) {
	const auto core = strip_pep3118_endianness_prefix(format);
	if (core.size() != 1) {
		throw nb::value_error(
		    "set_pixel_data supports only primitive numeric source dtypes");
	}

	const auto ensure_itemsize = [itemsize](std::size_t expected) {
		if (itemsize != expected) {
			throw nb::value_error(
			    "set_pixel_data source itemsize does not match source dtype format");
		}
	};

	switch (core.front()) {
	case 'B':
		ensure_itemsize(1);
		return dicom::pixel::DataType::u8;
	case 'b':
		ensure_itemsize(1);
		return dicom::pixel::DataType::s8;
	case 'H':
		ensure_itemsize(2);
		return dicom::pixel::DataType::u16;
	case 'h':
		ensure_itemsize(2);
		return dicom::pixel::DataType::s16;
	case 'I':
		ensure_itemsize(4);
		return dicom::pixel::DataType::u32;
	case 'i':
		ensure_itemsize(4);
		return dicom::pixel::DataType::s32;
	case 'f':
		ensure_itemsize(4);
		return dicom::pixel::DataType::f32;
	case 'd':
		ensure_itemsize(8);
		return dicom::pixel::DataType::f64;
	default:
		break;
	}

	throw nb::value_error("set_pixel_data unsupported source dtype format");
}

[[nodiscard]] dicom::pixel::ConstPixelSpan build_pixel_span_or_throw(
    const Py_buffer& view) {
	const auto throw_source_error = [](std::string_view detail) -> void {
		throw nb::value_error((std::string("set_pixel_data ") + std::string(detail)).c_str());
	};

	if (view.ndim < 2 || view.ndim > 4) {
		throw_source_error("source must have ndim 2, 3, or 4");
	}
	if (view.itemsize <= 0) {
		throw_source_error("source must have a positive itemsize");
	}
	if (view.len < 0) {
		throw_source_error("source buffer length is invalid");
	}
	if (view.len > 0 && view.buf == nullptr) {
		throw_source_error("source buffer is null");
	}
	if (view.shape == nullptr) {
		throw_source_error("source must expose shape metadata");
	}
	if (view.format == nullptr || view.format[0] == '\0') {
		throw_source_error("source must expose dtype format metadata");
	}

	const auto format = std::string_view(view.format);
	if (!format.empty() && format.front() == '>') {
		throw_source_error("does not support big-endian source dtype");
	}

	const auto bytes_per_sample = static_cast<std::size_t>(view.itemsize);
	const auto data_type =
	    parse_pixel_source_data_type_or_throw(format, bytes_per_sample);

	const auto read_dim = [&](int axis) -> std::size_t {
		const auto value = view.shape[axis];
		if (value <= 0) {
			throw_source_error("source shape values must be positive");
		}
		return static_cast<std::size_t>(value);
	};

	std::size_t frames = 1;
	std::size_t rows = 0;
	std::size_t cols = 0;
	std::size_t samples_per_pixel = 1;

	if (view.ndim == 2) {
		rows = read_dim(0);
		cols = read_dim(1);
	} else if (view.ndim == 3) {
		const auto d0 = read_dim(0);
		const auto d1 = read_dim(1);
		const auto d2 = read_dim(2);
		if (d2 == 1 || d2 == 3) {
			rows = d0;
			cols = d1;
			samples_per_pixel = d2;
		} else {
			frames = d0;
			rows = d1;
			cols = d2;
		}
	} else {
		frames = read_dim(0);
		rows = read_dim(1);
		cols = read_dim(2);
		samples_per_pixel = read_dim(3);
	}

	if (samples_per_pixel != 1 && samples_per_pixel != 3) {
		throw_source_error("currently supports samples_per_pixel 1 or 3");
	}

	if (rows > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
	    cols > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
	    frames > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
	    samples_per_pixel > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
		throw_source_error("source dimensions exceed int range");
	}

	const auto max_size = std::numeric_limits<std::size_t>::max();
	if (samples_per_pixel != 0 &&
	    cols > (max_size / samples_per_pixel)) {
		throw_source_error("row size overflow");
	}
	const auto row_components = cols * samples_per_pixel;
	if (bytes_per_sample != 0 &&
	    row_components > (max_size / bytes_per_sample)) {
		throw_source_error("row stride overflow");
	}
	const auto row_stride = row_components * bytes_per_sample;
	if (rows != 0 && row_stride > (max_size / rows)) {
		throw_source_error("frame stride overflow");
	}
	const auto frame_stride = row_stride * rows;
	if (frames != 0 && frame_stride > (max_size / frames)) {
		throw_source_error("total byte size overflow");
	}
	const auto required_bytes = frame_stride * frames;
	const auto actual_bytes = static_cast<std::size_t>(view.len);
	if (actual_bytes != required_bytes) {
		throw_source_error("source buffer size does not match inferred shape and dtype");
	}

	// Build the normalized layout once so Python callers share the same encode path
	// as native C++ callers.
	dicom::pixel::PixelLayout layout{
	    .data_type = data_type,
	    .photometric = samples_per_pixel == 1
	                       ? dicom::pixel::Photometric::monochrome2
	                       : dicom::pixel::Photometric::rgb,
	    .planar = dicom::pixel::Planar::interleaved,
	    .reserved = 0,
	    .rows = static_cast<std::uint32_t>(rows),
	    .cols = static_cast<std::uint32_t>(cols),
	    .frames = static_cast<std::uint32_t>(frames),
	    .samples_per_pixel = static_cast<std::uint16_t>(samples_per_pixel),
	    .bits_stored = dicom::pixel::normalized_bits_stored_of(data_type),
	    .row_stride = row_stride,
	    .frame_stride = frame_stride,
	};
	return dicom::pixel::ConstPixelSpan{
	    .layout = layout,
	    .bytes = std::span<const std::uint8_t>(
	        reinterpret_cast<const std::uint8_t*>(view.buf), actual_bytes),
	};
}

[[nodiscard]] dicom::pixel::ConstPixelSpan build_transform_pixel_span_or_throw(
    const Py_buffer& view) {
	const auto throw_source_error = [](std::string_view detail) -> void {
		throw nb::value_error((std::string("pixel transform ") + std::string(detail)).c_str());
	};

	if (view.ndim < 2 || view.ndim > 4) {
		throw_source_error("source must have ndim 2, 3, or 4");
	}
	if (view.itemsize <= 0) {
		throw_source_error("source must have a positive itemsize");
	}
	if (view.len < 0) {
		throw_source_error("source buffer length is invalid");
	}
	if (view.len > 0 && view.buf == nullptr) {
		throw_source_error("source buffer is null");
	}
	if (view.shape == nullptr) {
		throw_source_error("source must expose shape metadata");
	}
	if (view.format == nullptr || view.format[0] == '\0') {
		throw_source_error("source must expose dtype format metadata");
	}

	const auto format = std::string_view(view.format);
	if (!format.empty() && format.front() == '>') {
		throw_source_error("does not support big-endian source dtype");
	}

	const auto bytes_per_sample = static_cast<std::size_t>(view.itemsize);
	const auto data_type =
	    parse_pixel_source_data_type_or_throw(format, bytes_per_sample);

	const auto read_dim = [&](int axis) -> std::size_t {
		const auto value = view.shape[axis];
		if (value <= 0) {
			throw_source_error("source shape values must be positive");
		}
		return static_cast<std::size_t>(value);
	};

	std::size_t frames = 1;
	std::size_t rows = 0;
	std::size_t cols = 0;
	std::size_t samples_per_pixel = 1;

	if (view.ndim == 2) {
		rows = read_dim(0);
		cols = read_dim(1);
	} else if (view.ndim == 3) {
		const auto d0 = read_dim(0);
		const auto d1 = read_dim(1);
		const auto d2 = read_dim(2);
		if (d2 == 1 || d2 == 3) {
			throw_source_error(
			    "source shape is ambiguous when ndim == 3 and the trailing dimension is 1 or 3; "
			    "reshape to (rows, cols), (frames, rows, cols), or (frames, rows, cols, 1) explicitly");
		}
		frames = d0;
		rows = d1;
		cols = d2;
	} else {
		frames = read_dim(0);
		rows = read_dim(1);
		cols = read_dim(2);
		samples_per_pixel = read_dim(3);
	}

	if (samples_per_pixel != 1) {
		throw_source_error("currently supports only monochrome/indexed samples_per_pixel 1");
	}

	if (rows > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
	    cols > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
	    frames > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
		throw_source_error("source dimensions exceed int range");
	}

	const auto max_size = std::numeric_limits<std::size_t>::max();
	if (samples_per_pixel != 0 &&
	    cols > (max_size / samples_per_pixel)) {
		throw_source_error("row size overflow");
	}
	const auto row_components = cols * samples_per_pixel;
	if (bytes_per_sample != 0 &&
	    row_components > (max_size / bytes_per_sample)) {
		throw_source_error("row stride overflow");
	}
	const auto row_stride = row_components * bytes_per_sample;
	if (rows != 0 && row_stride > (max_size / rows)) {
		throw_source_error("frame stride overflow");
	}
	const auto frame_stride = row_stride * rows;
	if (frames != 0 && frame_stride > (max_size / frames)) {
		throw_source_error("total byte size overflow");
	}
	const auto required_bytes = frame_stride * frames;
	const auto actual_bytes = static_cast<std::size_t>(view.len);
	if (actual_bytes != required_bytes) {
		throw_source_error("source buffer size does not match inferred shape and dtype");
	}

	dicom::pixel::PixelLayout layout{
	    .data_type = data_type,
	    .photometric = dicom::pixel::Photometric::monochrome2,
	    .planar = dicom::pixel::Planar::interleaved,
	    .reserved = 0,
	    .rows = static_cast<std::uint32_t>(rows),
	    .cols = static_cast<std::uint32_t>(cols),
	    .frames = static_cast<std::uint32_t>(frames),
	    .samples_per_pixel = static_cast<std::uint16_t>(samples_per_pixel),
	    .bits_stored = dicom::pixel::normalized_bits_stored_of(data_type),
	    .row_stride = row_stride,
	    .frame_stride = frame_stride,
	};
	return dicom::pixel::ConstPixelSpan{
	    .layout = layout,
	    .bytes = std::span<const std::uint8_t>(
	        reinterpret_cast<const std::uint8_t*>(view.buf), actual_bytes),
	};
}

nb::object apply_rescale_to_numpy_array(
    nb::handle source, float slope, float intercept) {
	PyReadOnlyBufferView source_view(source);
	const auto pixel_span = build_transform_pixel_span_or_throw(source_view.view());
	return numpy_array_from_owned_pixel_buffer(
	    dicom::pixel::apply_rescale(pixel_span, slope, intercept));
}

nb::object apply_rescale_frames_to_numpy_array(
    nb::handle source, const std::vector<float>& slopes,
    const std::vector<float>& intercepts) {
	PyReadOnlyBufferView source_view(source);
	const auto pixel_span = build_transform_pixel_span_or_throw(source_view.view());
	auto output = dicom::pixel::PixelBuffer::allocate(
	    dicom::pixel::make_rescale_output_layout(pixel_span.layout));
	dicom::pixel::apply_rescale_frames_into(
	    pixel_span, output.span(), slopes, intercepts);
	return numpy_array_from_owned_pixel_buffer(std::move(output));
}

nb::object apply_modality_lut_to_numpy_array(
    nb::handle source, const dicom::pixel::ModalityLut& lut) {
	PyReadOnlyBufferView source_view(source);
	const auto pixel_span = build_transform_pixel_span_or_throw(source_view.view());
	return numpy_array_from_owned_pixel_buffer(
	    dicom::pixel::apply_modality_lut(pixel_span, lut));
}

nb::object apply_palette_lut_to_numpy_array(
    nb::handle source, const dicom::pixel::PaletteLut& lut) {
	PyReadOnlyBufferView source_view(source);
	const auto pixel_span = build_transform_pixel_span_or_throw(source_view.view());
	return numpy_array_from_owned_pixel_buffer(
	    dicom::pixel::apply_palette_lut(pixel_span, lut));
}

nb::object apply_voi_lut_to_numpy_array(
    nb::handle source, const dicom::pixel::VoiLut& lut) {
	PyReadOnlyBufferView source_view(source);
	const auto pixel_span = build_transform_pixel_span_or_throw(source_view.view());
	return numpy_array_from_owned_pixel_buffer(
	    dicom::pixel::apply_voi_lut(pixel_span, lut));
}

nb::object apply_window_to_numpy_array(
    nb::handle source, const dicom::pixel::WindowTransform& window) {
	PyReadOnlyBufferView source_view(source);
	const auto pixel_span = build_transform_pixel_span_or_throw(source_view.view());
	return numpy_array_from_owned_pixel_buffer(
	    dicom::pixel::apply_window(pixel_span, window));
}

[[nodiscard]] std::string format_encoder_option_double(double value) {
	std::ostringstream stream;
	stream << std::setprecision(17) << value;
	return stream.str();
}

[[nodiscard]] CodecOptionTextStorage parse_encoder_options_to_text_storage(
    nb::handle options_obj,
    std::optional<dicom::uid::WellKnown> transfer_syntax = std::nullopt) {
	using namespace dicom::literals;
	CodecOptionTextStorage storage{};
	if (!options_obj || options_obj.is_none()) {
		storage.auto_mode = true;
		return storage;
	}

	struct CodecOptionFields {
		bool has_type{false};
		std::string type_name{"auto"};
		bool has_target_bpp{false};
		bool has_target_psnr{false};
		bool has_threads{false};
		bool has_color_transform{false};
		bool has_distance{false};
		bool has_effort{false};
		bool has_quality{false};
		bool has_near_lossless_error{false};
		double target_bpp{0.0};
		double target_psnr{0.0};
		int threads{-1};
		bool color_transform{true};
		double distance{1.0};
		int effort{7};
		int quality{90};
		int near_lossless_error{0};
	};

	const auto throw_incompatible = [&](std::string_view codec_name) {
		if (!transfer_syntax) {
			return;
		}
		throw nb::value_error(
		    (std::string("options type '") + std::string(codec_name) +
		        "' is incompatible with transfer syntax '" +
		        std::string(transfer_syntax->value()) + "'")
		        .c_str());
	};

	const auto parse_from_name = [&](std::string option_name,
	                                 const CodecOptionFields& fields) -> CodecOptionTextStorage {
		CodecOptionTextStorage out{};
		const auto normalized = normalize_encoder_option_name(std::move(option_name));
		const bool has_j2k_fields =
		    fields.has_target_bpp || fields.has_target_psnr || fields.has_color_transform;
		const bool has_thread_field = fields.has_threads;
		const bool has_jpegxl_fields = fields.has_distance || fields.has_effort;
		const bool has_jpeg_fields = fields.has_quality;
		const bool has_jpegls_fields = fields.has_near_lossless_error;

		const auto ensure_no_fields = [&](std::string_view codec_name) {
			if (has_j2k_fields || has_thread_field || has_jpegxl_fields ||
			    has_jpeg_fields || has_jpegls_fields) {
				throw nb::value_error(
				    (std::string(codec_name) +
				        " options does not accept extra option fields").c_str());
			}
		};

		if (normalized.empty() || normalized == "auto") {
			ensure_no_fields("auto");
			out.auto_mode = true;
			return out;
		}
		if (normalized == "none" || normalized == "nocompression" ||
		    normalized == "native" || normalized == "uncompressed") {
			ensure_no_fields("none");
			if (transfer_syntax && !transfer_syntax->is_uncompressed()) {
				throw_incompatible("none");
			}
			out.finalize();
			return out;
		}
		if (normalized == "rle" || normalized == "rlelossless") {
			ensure_no_fields("rle");
			if (transfer_syntax && !transfer_syntax->is_rle()) {
				throw_incompatible("rle");
			}
			out.finalize();
			return out;
		}
			if (normalized == "j2k" || normalized == "jpeg2000" || normalized == "j2koptions") {
				if (has_jpeg_fields || has_jpegls_fields || has_jpegxl_fields) {
					throw nb::value_error(
					    "j2k options do not accept quality/near_lossless_error/distance/effort");
				}
			if (fields.target_bpp < 0.0 || fields.target_psnr < 0.0) {
				throw nb::value_error("options target_bpp/target_psnr must be >= 0");
			}
			if (fields.threads < -1) {
				throw nb::value_error("options threads must be -1, 0, or positive");
			}
			if (transfer_syntax && !transfer_syntax->is_jpeg2000()) {
				throw_incompatible("j2k");
			}
			out.entries.reserve(4);
			out.add("target_bpp", format_encoder_option_double(fields.target_bpp));
			out.add("target_psnr", format_encoder_option_double(fields.target_psnr));
			out.add("threads", std::to_string(fields.threads));
			out.add("color_transform", fields.color_transform ? "true" : "false");
			out.finalize();
			return out;
		}
			if (normalized == "htj2k" || normalized == "htj2koptions") {
				if (has_jpeg_fields || has_jpegls_fields || has_jpegxl_fields) {
					throw nb::value_error(
					    "htj2k options do not accept quality/near_lossless_error/distance/effort");
				}
			if (fields.target_bpp < 0.0 || fields.target_psnr < 0.0) {
				throw nb::value_error("options target_bpp/target_psnr must be >= 0");
			}
			if (fields.threads < -1) {
				throw nb::value_error("options threads must be -1, 0, or positive");
			}
			if (transfer_syntax && !transfer_syntax->is_htj2k()) {
				throw_incompatible("htj2k");
			}
			out.entries.reserve(4);
			out.add("target_bpp", format_encoder_option_double(fields.target_bpp));
			out.add("target_psnr", format_encoder_option_double(fields.target_psnr));
			out.add("threads", std::to_string(fields.threads));
			out.add("color_transform", fields.color_transform ? "true" : "false");
			out.finalize();
			return out;
		}
			if (normalized == "jpegls" || normalized == "jls" ||
			    normalized == "jpeglsoptions") {
				if (has_j2k_fields || has_thread_field || has_jpeg_fields || has_jpegxl_fields) {
					throw nb::value_error(
					    "jpegls options do not accept target_bpp/target_psnr/threads/color_transform/quality/distance/effort");
				}
			if (fields.near_lossless_error < 0 || fields.near_lossless_error > 255) {
				throw nb::value_error(
				    "options near_lossless_error must be in [0, 255]");
			}
			if (transfer_syntax && !transfer_syntax->is_jpegls()) {
				throw_incompatible("jpegls");
			}
			if (transfer_syntax && transfer_syntax->is_lossless() &&
			    fields.near_lossless_error != 0) {
				throw nb::value_error(
				    "JPEG-LS lossless transfer syntax requires options near_lossless_error=0");
			}
			if (transfer_syntax && transfer_syntax->is_lossy() &&
			    fields.near_lossless_error <= 0) {
				throw nb::value_error(
				    "JPEG-LS lossy transfer syntax requires options near_lossless_error>0");
			}
			out.entries.reserve(1);
			out.add("near_lossless_error", std::to_string(fields.near_lossless_error));
			out.finalize();
			return out;
		}
			if (normalized == "jpeg" || normalized == "jpegbaseline" ||
			    normalized == "jpeglossless" || normalized == "jpegoptions") {
				if (has_j2k_fields || has_thread_field || has_jpegls_fields || has_jpegxl_fields) {
					throw nb::value_error(
					    "jpeg options do not accept target_bpp/target_psnr/threads/color_transform/near_lossless_error/distance/effort");
				}
			if (fields.quality < 1 || fields.quality > 100) {
				throw nb::value_error("options quality must be in [1, 100]");
			}
			if (transfer_syntax && !transfer_syntax->is_jpeg_family()) {
				throw_incompatible("jpeg");
			}
			out.entries.reserve(1);
			out.add("quality", std::to_string(fields.quality));
			out.finalize();
			return out;
		}
			if (normalized == "jpegxl" || normalized == "jxl" ||
			    normalized == "jpegxloptions") {
				if (has_j2k_fields || has_jpeg_fields || has_jpegls_fields) {
					throw nb::value_error(
					    "jpegxl options do not accept target_bpp/target_psnr/color_transform/quality/near_lossless_error");
				}
			if (fields.threads < -1) {
				throw nb::value_error("options threads must be -1, 0, or positive");
			}
			if (fields.effort < 1 || fields.effort > 10) {
				throw nb::value_error("options effort must be in [1, 10]");
			}
			if (!std::isfinite(fields.distance) || fields.distance < 0.0 ||
			    fields.distance > 25.0) {
				throw nb::value_error("options distance must be in [0, 25]");
			}
			if (transfer_syntax && !transfer_syntax->is_jpegxl()) {
				throw_incompatible("jpegxl");
			}
			double distance = fields.distance;
			if (!fields.has_distance && transfer_syntax &&
			    *transfer_syntax == "JPEGXLLossless"_uid) {
				distance = 0.0;
			}
			if (transfer_syntax &&
			    *transfer_syntax == "JPEGXLJPEGRecompression"_uid) {
				throw nb::value_error(
				    "JPEGXLJPEGRecompression transfer syntax is decode-only and not supported for encoding");
			}
			if (transfer_syntax &&
			    *transfer_syntax == "JPEGXLLossless"_uid &&
			    distance != 0.0) {
				throw nb::value_error(
				    "JPEGXLLossless transfer syntax requires options distance=0");
			}
			if (transfer_syntax &&
			    *transfer_syntax == "JPEGXL"_uid &&
			    distance <= 0.0) {
				throw nb::value_error(
				    "JPEGXL transfer syntax requires options distance > 0");
			}

			out.entries.reserve(3);
			out.add("distance", format_encoder_option_double(distance));
			out.add("effort", std::to_string(fields.effort));
			out.add("threads", std::to_string(fields.threads));
			out.finalize();
			return out;
		}

		throw nb::value_error(
		    "options must be one of: auto, none, rle, j2k, htj2k, jpegls, jpeg, jpegxl");
	};

	if (nb::isinstance<nb::str>(options_obj)) {
		const CodecOptionFields fields{};
		return parse_from_name(nb::cast<std::string>(options_obj), fields);
	}

	if (!PyDict_Check(options_obj.ptr())) {
		throw nb::type_error("options must be None, str, or dict");
	}

	nb::dict codec_dict = nb::borrow<nb::dict>(options_obj);
	static const std::unordered_set<std::string> allowed_keys{
	    "type", "target_bpp", "target_psnr", "threads", "color_transform", "use_mct",
	    "mct", "quality", "near_lossless_error", "distance", "effort"};
	PyObject* key_obj = nullptr;
	PyObject* value_obj = nullptr;
	Py_ssize_t pos = 0;
	while (PyDict_Next(codec_dict.ptr(), &pos, &key_obj, &value_obj)) {
		if (!PyUnicode_Check(key_obj)) {
			throw nb::type_error("options dict keys must be str");
		}
		const auto key = nb::cast<std::string>(nb::handle(key_obj));
		if (allowed_keys.find(key) == allowed_keys.end()) {
			throw nb::value_error(("options has unknown key: " + key).c_str());
		}
	}

	CodecOptionFields fields{};
	if (PyObject* type_obj = PyDict_GetItemString(codec_dict.ptr(), "type")) {
		fields.has_type = true;
		fields.type_name = nb::cast<std::string>(nb::handle(type_obj));
	}
	if (PyObject* value = PyDict_GetItemString(codec_dict.ptr(), "target_bpp")) {
		fields.has_target_bpp = true;
		fields.target_bpp = nb::cast<double>(nb::handle(value));
	}
	if (PyObject* value = PyDict_GetItemString(codec_dict.ptr(), "target_psnr")) {
		fields.has_target_psnr = true;
		fields.target_psnr = nb::cast<double>(nb::handle(value));
	}
	if (PyObject* value = PyDict_GetItemString(codec_dict.ptr(), "threads")) {
		fields.has_threads = true;
		fields.threads = nb::cast<int>(nb::handle(value));
	}
	if (PyObject* value = PyDict_GetItemString(codec_dict.ptr(), "color_transform")) {
		fields.has_color_transform = true;
		fields.color_transform = nb::cast<bool>(nb::handle(value));
	}
	if (PyObject* value = PyDict_GetItemString(codec_dict.ptr(), "use_mct")) {
		const bool parsed = nb::cast<bool>(nb::handle(value));
		if (fields.has_color_transform && fields.color_transform != parsed) {
			throw nb::value_error(
			    "options color_transform and use_mct must match when both are provided");
		}
		fields.has_color_transform = true;
		fields.color_transform = parsed;
	}
	if (PyObject* value = PyDict_GetItemString(codec_dict.ptr(), "mct")) {
		const bool parsed = nb::cast<bool>(nb::handle(value));
		if (fields.has_color_transform && fields.color_transform != parsed) {
			throw nb::value_error(
			    "options color_transform and mct must match when both are provided");
		}
		fields.has_color_transform = true;
		fields.color_transform = parsed;
	}
	if (PyObject* value = PyDict_GetItemString(codec_dict.ptr(), "quality")) {
		fields.has_quality = true;
		fields.quality = nb::cast<int>(nb::handle(value));
	}
	if (PyObject* value = PyDict_GetItemString(codec_dict.ptr(), "distance")) {
		fields.has_distance = true;
		fields.distance = nb::cast<double>(nb::handle(value));
	}
	if (PyObject* value = PyDict_GetItemString(codec_dict.ptr(), "effort")) {
		fields.has_effort = true;
		fields.effort = nb::cast<int>(nb::handle(value));
	}
	if (PyObject* value = PyDict_GetItemString(codec_dict.ptr(), "near_lossless_error")) {
		fields.has_near_lossless_error = true;
		fields.near_lossless_error = nb::cast<int>(nb::handle(value));
	}

	if (!fields.has_type) {
		if (fields.has_target_bpp || fields.has_target_psnr || fields.has_threads) {
			fields.type_name = "j2k";
		} else if (fields.has_distance || fields.has_effort) {
			fields.type_name = "jpegxl";
		} else if (fields.has_quality) {
			fields.type_name = "jpeg";
		} else if (fields.has_near_lossless_error) {
			fields.type_name = "jpegls";
		} else if (fields.has_color_transform) {
			throw nb::value_error(
			    "options with color_transform/use_mct requires explicit type ('j2k' or 'htj2k')");
		}
	}

	return parse_from_name(fields.type_name, fields);
}

[[nodiscard]] Uid parse_transfer_syntax_text_or_throw(
    const std::string& transfer_syntax_text) {
	const auto uid = dicom::uid::lookup(transfer_syntax_text);
	if (!uid) {
		throw nb::value_error(("Unknown DICOM UID: " + transfer_syntax_text).c_str());
	}
	if (uid->uid_type() != dicom::UidType::TransferSyntax) {
		throw nb::value_error(
		    ("UID is not a Transfer Syntax UID: " + transfer_syntax_text).c_str());
	}
	return *uid;
}

[[nodiscard]] Uid require_transfer_syntax_uid_or_throw(Uid uid) {
	if (!uid.valid()) {
		throw nb::value_error("uid must be a valid Transfer Syntax UID");
	}
	if (uid.uid_type() != dicom::UidType::TransferSyntax) {
		const std::string message =
		    "UID is not a Transfer Syntax UID: " + std::string(uid.value());
		throw nb::value_error(message.c_str());
	}
	return uid;
}

void set_transfer_syntax_with_options(
    DicomFile& self, Uid transfer_syntax, nb::handle options) {
	const auto text_options =
	    parse_encoder_options_to_text_storage(options, transfer_syntax);
	if (text_options.auto_mode) {
		self.set_transfer_syntax(transfer_syntax);
		return;
	}
	self.set_transfer_syntax(transfer_syntax, text_options.span());
}

void write_with_transfer_syntax_with_options(DicomFile& self, nb::handle path,
    Uid transfer_syntax, nb::handle options, bool include_preamble,
    bool write_file_meta, bool keep_existing_meta) {
	dicom::WriteOptions write_options;
	write_options.include_preamble = include_preamble;
	write_options.write_file_meta = write_file_meta;
	write_options.keep_existing_meta = keep_existing_meta;

	const auto output_path = python_path_to_filesystem_path(path, "path");
	const auto text_options =
	    parse_encoder_options_to_text_storage(options, transfer_syntax);
	if (text_options.auto_mode) {
		self.write_with_transfer_syntax(output_path, transfer_syntax, write_options);
		return;
	}
	self.write_with_transfer_syntax(
	    output_path, transfer_syntax, text_options.span(), write_options);
}

void write_with_transfer_syntax_with_encoder_context(DicomFile& self, nb::handle path,
    Uid transfer_syntax, const EncoderContext& encoder_context, bool include_preamble,
    bool write_file_meta, bool keep_existing_meta) {
	dicom::WriteOptions write_options;
	write_options.include_preamble = include_preamble;
	write_options.write_file_meta = write_file_meta;
	write_options.keep_existing_meta = keep_existing_meta;
	self.write_with_transfer_syntax(python_path_to_filesystem_path(path, "path"),
	    transfer_syntax, encoder_context, write_options);
}

void set_pixel_data_with_options(DicomFile& self, Uid transfer_syntax,
    nb::handle source_obj, std::optional<std::size_t> frame_index,
    nb::handle options) {
	PyReadOnlyBufferView source_view(source_obj);
	const auto source = build_pixel_span_or_throw(source_view.view());
	const auto text_options =
	    parse_encoder_options_to_text_storage(options, transfer_syntax);
	if (text_options.auto_mode) {
		if (frame_index.has_value()) {
			self.set_pixel_data(transfer_syntax, source, *frame_index);
		} else {
			self.set_pixel_data(transfer_syntax, source);
		}
		return;
	}
	if (frame_index.has_value()) {
		self.set_pixel_data(transfer_syntax, source, *frame_index,
		    text_options.span());
	} else {
		self.set_pixel_data(transfer_syntax, source, text_options.span());
	}
}

void set_pixel_data_with_encoder_context(DicomFile& self, Uid transfer_syntax,
    nb::handle source_obj, std::optional<std::size_t> frame_index,
    const EncoderContext& encoder_context) {
	PyReadOnlyBufferView source_view(source_obj);
	const auto source = build_pixel_span_or_throw(source_view.view());
	if (frame_index.has_value()) {
		self.set_pixel_data(transfer_syntax, source, *frame_index,
		    encoder_context);
	} else {
		self.set_pixel_data(transfer_syntax, source, encoder_context);
	}
}

[[nodiscard]] EncoderContext create_encoder_context_with_options(
    Uid transfer_syntax, nb::handle options) {
	const auto text_options =
	    parse_encoder_options_to_text_storage(options, transfer_syntax);
	if (text_options.auto_mode) {
		return dicom::pixel::create_encoder_context(transfer_syntax);
	}
	return dicom::pixel::create_encoder_context(
	    transfer_syntax, text_options.span());
}

void configure_encoder_context_with_options(EncoderContext& context,
    Uid transfer_syntax, nb::handle options) {
	const auto text_options =
	    parse_encoder_options_to_text_storage(options, transfer_syntax);
	if (text_options.auto_mode) {
		context.configure(transfer_syntax);
		return;
	}
	context.configure(transfer_syntax, text_options.span());
}

DecodedArrayLayout build_decode_layout_from_plan(
    const dicom::pixel::DecodePlan& plan, long frame) {
	if (frame < -1) {
		throw nb::value_error("frame must be >= -1");
	}

	DecodedArrayLayout layout{};
	layout.plan = plan;

	// The Python array contract follows the normalized decode output layout directly.
	const auto& output_layout = layout.plan.output_layout;
	if (output_layout.empty()) {
		throw nb::value_error(
		    "to_array/decode_into requires PixelData, FloatPixelData, or DoubleFloatPixelData");
	}
	if (output_layout.rows == 0 || output_layout.cols == 0 ||
	    output_layout.samples_per_pixel == 0) {
		throw nb::value_error(
		    "to_array/decode_into requires positive Rows/Columns/SamplesPerPixel");
	}
	if (output_layout.frames == 0) {
		throw nb::value_error("to_array/decode_into requires NumberOfFrames >= 1");
	}

	layout.frames = static_cast<std::size_t>(output_layout.frames);
	layout.spec = decoded_array_spec(output_layout);

	// NumPy strides are expressed in element counts, so validate byte strides first.
	const auto bytes_per_sample = layout.spec.bytes_per_sample;
	if (bytes_per_sample == 0) {
		throw nb::value_error("to_array/decode_into could not determine output sample size");
	}
	const auto row_stride = output_layout.row_stride;
	layout.frame_stride = output_layout.frame_stride;
	if ((row_stride % bytes_per_sample) != 0 || (layout.frame_stride % bytes_per_sample) != 0) {
		throw std::runtime_error(
		    "to_array/decode_into stride is not aligned to output sample size");
	}

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
		const auto geometry =
		    build_normalized_array_geometry_or_throw(output_layout, bytes_per_sample, false);
		layout.ndim = geometry.ndim;
		layout.shape = geometry.shape;
		layout.strides = geometry.strides;
		return layout;
	}

	layout.required_bytes = layout.frame_stride * layout.frames;
	const auto geometry =
	    build_normalized_array_geometry_or_throw(output_layout, bytes_per_sample, true);
	layout.ndim = geometry.ndim;
	layout.shape = geometry.shape;
	layout.strides = geometry.strides;
	return layout;
}

DecodedArrayLayout build_decode_layout(const DicomFile& self, long frame,
    int worker_threads = -1, int codec_threads = -1, bool decode_mct = true) {
	if (worker_threads < -1) {
		throw nb::value_error("worker_threads must be -1, 0, or positive");
	}
	if (codec_threads < -1) {
		throw nb::value_error("codec_threads must be -1, 0, or positive");
	}

	// The convenience array helpers always request tightly packed interleaved output.
	dicom::pixel::DecodeOptions opt{};
	opt.alignment = 1;
	opt.planar_out = dicom::pixel::Planar::interleaved;
	opt.decode_mct = decode_mct;
	opt.worker_threads = worker_threads;
	opt.codec_threads = codec_threads;
	return build_decode_layout_from_plan(self.create_decode_plan(opt), frame);
}

void validate_decode_thread_options_or_throw(int worker_threads, int codec_threads) {
	if (worker_threads < -1) {
		throw nb::value_error("worker_threads must be -1, 0, or positive");
	}
	if (codec_threads < -1) {
		throw nb::value_error("codec_threads must be -1, 0, or positive");
	}
}

void decode_layout_into(const DicomFile& self, const DecodedArrayLayout& layout,
    std::span<std::uint8_t> out) {
	if (out.size() < layout.required_bytes) {
		throw nb::value_error("decode_into output buffer is smaller than required size");
	}
	if (layout.decode_all_frames) {
		self.decode_all_frames_into(out, layout.plan);
		return;
	}
	self.decode_into(layout.frame_index, out, layout.plan);
}

void decode_layout_into_unchecked(const DicomFile& self, const DecodedArrayLayout& layout,
    std::span<std::uint8_t> out) {
	if (layout.decode_all_frames) {
		self.decode_all_frames_into(out, layout.plan);
		return;
	}
	self.decode_into(layout.frame_index, out, layout.plan);
}

void decode_layout_into_with_info_unchecked(const DicomFile& self,
    const DecodedArrayLayout& layout, std::span<std::uint8_t> out,
    dicom::pixel::DecodeInfo& decode_info) {
	if (layout.decode_all_frames) {
		self.decode_all_frames_into(out, layout.plan, decode_info);
		return;
	}
	self.decode_into(layout.frame_index, out, layout.plan, decode_info);
}

DecodedArrayLayout resolve_decode_layout_or_throw(const DicomFile& self, long frame,
    bool decode_mct, int worker_threads, int codec_threads, nb::handle plan_obj) {
	if (!plan_obj.is_none()) {
		// A reusable plan already fixes dtype, strides, and required byte counts.
		return build_decode_layout_from_plan(
		    nb::cast<dicom::pixel::DecodePlan>(plan_obj), frame);
	}

	validate_decode_thread_options_or_throw(worker_threads, codec_threads);
	return build_decode_layout(self, frame, worker_threads, codec_threads, decode_mct);
}

void copy_direct_raw_array_access_unchecked(const DirectRawArrayAccess& access,
    std::span<std::uint8_t> dst) {
	if (access.layout.required_bytes == 0) {
		return;
	}
	std::memcpy(dst.data(), access.source_bytes.data() + access.byte_offset,
	    access.layout.required_bytes);
}

DirectRawArrayAccess build_direct_raw_array_access_or_throw(
    const DicomFile& self, long frame) {
	if (frame < -1) {
		throw nb::value_error("frame must be >= -1");
	}

	const auto source_layout =
	    dicom::pixel::support_detail::compute_decode_source_layout(self);
	if (source_layout.empty()) {
		throw nb::value_error(
		    "to_array/decode_into requires PixelData, FloatPixelData, or DoubleFloatPixelData");
	}
	if (!self.transfer_syntax_uid().is_uncompressed()) {
		throw nb::value_error("to_array_view requires an uncompressed transfer syntax");
	}

	// Direct views are only possible when native uncompressed storage already matches
	// the exported NumPy layout.
	const auto source_view =
	    dicom::pixel::support_detail::compute_native_decode_source_view_or_throw(
	        self, source_layout);
	if (source_view.planar_source && source_view.samples_per_pixel > std::size_t{1}) {
		throw nb::value_error(
		    "to_array_view requires PlanarConfiguration=interleaved when SamplesPerPixel > 1");
	}

	DirectRawArrayAccess access{};
	auto& layout = access.layout;
	layout.spec = decoded_array_spec(source_layout);
	layout.frames = source_view.frames;
	layout.decode_all_frames = (frame == -1) && (layout.frames > 1);
	layout.frame_index =
	    layout.decode_all_frames ? 0u : ((frame < 0) ? 0u : static_cast<std::size_t>(frame));
	if (layout.frame_index >= layout.frames) {
		throw nb::index_error("to_array/decode_into frame index out of range");
	}

	layout.frame_stride = source_view.frame_bytes;
	layout.required_bytes =
	    layout.decode_all_frames ? (source_view.frame_bytes * source_view.frames)
	                             : source_view.frame_bytes;

	const auto row_stride_elems =
	    source_view.row_payload_bytes / layout.spec.bytes_per_sample;
	const auto frame_stride_elems =
	    source_view.frame_bytes / layout.spec.bytes_per_sample;
	const auto col_stride_elems = source_view.samples_per_pixel;
	if (!layout.decode_all_frames) {
		if (source_view.samples_per_pixel == 1) {
			layout.ndim = 2;
			layout.shape[0] = source_view.rows;
			layout.shape[1] = source_view.cols;
			layout.strides[0] = static_cast<std::int64_t>(row_stride_elems);
			layout.strides[1] = 1;
		} else {
			layout.ndim = 3;
			layout.shape[0] = source_view.rows;
			layout.shape[1] = source_view.cols;
			layout.shape[2] = source_view.samples_per_pixel;
			layout.strides[0] = static_cast<std::int64_t>(row_stride_elems);
			layout.strides[1] = static_cast<std::int64_t>(col_stride_elems);
			layout.strides[2] = 1;
		}
	} else {
		if (source_view.samples_per_pixel == 1) {
			layout.ndim = 3;
			layout.shape[0] = layout.frames;
			layout.shape[1] = source_view.rows;
			layout.shape[2] = source_view.cols;
			layout.strides[0] = static_cast<std::int64_t>(frame_stride_elems);
			layout.strides[1] = static_cast<std::int64_t>(row_stride_elems);
			layout.strides[2] = 1;
		} else {
			layout.ndim = 4;
			layout.shape[0] = layout.frames;
			layout.shape[1] = source_view.rows;
			layout.shape[2] = source_view.cols;
			layout.shape[3] = source_view.samples_per_pixel;
			layout.strides[0] = static_cast<std::int64_t>(frame_stride_elems);
			layout.strides[1] = static_cast<std::int64_t>(row_stride_elems);
			layout.strides[2] = static_cast<std::int64_t>(col_stride_elems);
			layout.strides[3] = 1;
		}
	}

	access.source_bytes = source_view.source_bytes;
	access.byte_offset =
	    layout.decode_all_frames ? 0u : (layout.frame_index * source_view.frame_bytes);
	return access;
}

std::optional<DirectRawArrayAccess> try_build_direct_raw_array_access(
    const DicomFile& self, long frame) {
	try {
		return build_direct_raw_array_access_or_throw(self, frame);
	} catch (...) {
		return std::nullopt;
	}
}

void copy_direct_raw_array_access(const DirectRawArrayAccess& access,
    std::span<std::uint8_t> dst) {
	if (dst.size() != access.layout.required_bytes) {
		throw nb::value_error("direct raw copy size does not match expected decoded size");
	}
	copy_direct_raw_array_access_unchecked(access, dst);
}

nb::object dicomfile_to_array_view(const DicomFile& self, long frame) {
	const auto direct = build_direct_raw_array_access_or_throw(self, frame);
	const auto* data_ptr =
	    direct.layout.required_bytes == 0 ? nullptr
	                                      : (direct.source_bytes.data() + direct.byte_offset);
	nb::object owner = nb::cast(&self, nb::rv_policy::reference);
	return nb::cast(nb::ndarray<nb::numpy, const std::uint8_t>(
	    data_ptr, direct.layout.ndim, direct.layout.shape.data(), owner,
	    direct.layout.strides.data(), direct.layout.spec.dtype,
	    nb::device::cpu::value, 0, 'C'));
}

nb::object dicomfile_to_array(
    const DicomFile& self, long frame, bool decode_mct, int worker_threads,
    int codec_threads, nb::handle plan_obj) {
	if (!plan_obj.is_none()) {
		// A reusable plan already fixes dtype, strides, and required byte counts.
		const auto plan = nb::cast<dicom::pixel::DecodePlan>(plan_obj);
		const auto layout = build_decode_layout_from_plan(plan, frame);
		auto out = make_writable_numpy_array(
		    layout.ndim, layout.shape, layout.strides, layout.spec.dtype, layout.required_bytes);
		{
			nb::gil_scoped_release release;
			decode_layout_into_unchecked(self, layout, out.bytes);
		}
		return nb::cast(std::move(out.array));
	}

	validate_decode_thread_options_or_throw(worker_threads, codec_threads);
	if (auto direct = try_build_direct_raw_array_access(self, frame)) {
		// Prefer a raw copy path when the source bytes already match the target array layout.
		auto out = make_writable_numpy_array(direct->layout.ndim, direct->layout.shape,
		    direct->layout.strides, direct->layout.spec.dtype,
		    direct->layout.required_bytes);
		{
			nb::gil_scoped_release release;
			copy_direct_raw_array_access_unchecked(*direct, out.bytes);
		}
		return nb::cast(std::move(out.array));
	}
	const auto layout =
	    build_decode_layout(self, frame, worker_threads, codec_threads, decode_mct);
	auto out = make_writable_numpy_array(
	    layout.ndim, layout.shape, layout.strides, layout.spec.dtype, layout.required_bytes);
	{
		nb::gil_scoped_release release;
		decode_layout_into_unchecked(self, layout, out.bytes);
	}
	return nb::cast(std::move(out.array));
}

nb::tuple dicomfile_to_array_with_info(
    const DicomFile& self, long frame, bool decode_mct, int worker_threads,
    int codec_threads, nb::handle plan_obj) {
	const auto layout = resolve_decode_layout_or_throw(
	    self, frame, decode_mct, worker_threads, codec_threads, plan_obj);
	auto out = make_writable_numpy_array(
	    layout.ndim, layout.shape, layout.strides, layout.spec.dtype, layout.required_bytes);
	dicom::pixel::DecodeInfo decode_info{};
	{
		nb::gil_scoped_release release;
		decode_layout_into_with_info_unchecked(self, layout, out.bytes, decode_info);
	}
	return nb::make_tuple(nb::cast(std::move(out.array)), std::move(decode_info));
}

nb::object dicomfile_to_array_maybe_with_info(const DicomFile& self, long frame,
    bool decode_mct, int worker_threads, int codec_threads, nb::handle plan_obj,
    bool with_info) {
	if (with_info) {
		return nb::cast(dicomfile_to_array_with_info(
		    self, frame, decode_mct, worker_threads, codec_threads, plan_obj));
	}
	return dicomfile_to_array(
	    self, frame, decode_mct, worker_threads, codec_threads, plan_obj);
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
    long frame, bool decode_mct, int worker_threads, int codec_threads,
    nb::handle plan_obj) {
	std::optional<DirectRawArrayAccess> direct{};
	DecodedArrayLayout layout{};
	if (!plan_obj.is_none()) {
		// Reuse the caller-supplied plan instead of recomputing layout metadata.
		layout = build_decode_layout_from_plan(
		    nb::cast<dicom::pixel::DecodePlan>(plan_obj), frame);
	} else {
		validate_decode_thread_options_or_throw(worker_threads, codec_threads);
		direct = try_build_direct_raw_array_access(self, frame);
		layout = direct ? direct->layout
		                : build_decode_layout(
		                      self, frame, worker_threads, codec_threads, decode_mct);
	}

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
	if (direct) {
		nb::gil_scoped_release release;
		copy_direct_raw_array_access_unchecked(*direct, out_span);
	} else {
		nb::gil_scoped_release release;
		decode_layout_into_unchecked(self, layout, out_span);
	}
	return nb::borrow<nb::object>(out);
}

dicom::pixel::DecodeInfo dicomfile_decode_into_info(const DicomFile& self, nb::handle out,
    long frame, bool decode_mct, int worker_threads, int codec_threads,
    nb::handle plan_obj) {
	const auto layout = resolve_decode_layout_or_throw(
	    self, frame, decode_mct, worker_threads, codec_threads, plan_obj);

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
	dicom::pixel::DecodeInfo decode_info{};
	{
		nb::gil_scoped_release release;
		decode_layout_into_with_info_unchecked(self, layout, out_span, decode_info);
	}
	return decode_info;
}

nb::object dicomfile_decode_into_maybe_with_info(const DicomFile& self, nb::handle out,
    long frame, bool decode_mct, int worker_threads, int codec_threads,
    nb::handle plan_obj, bool with_info) {
	if (with_info) {
		return nb::cast(dicomfile_decode_into_info(
		    self, out, frame, decode_mct, worker_threads, codec_threads, plan_obj));
	}
	return dicomfile_decode_into_array(
	    self, out, frame, decode_mct, worker_threads, codec_threads, plan_obj);
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
	const auto raw_bytes = [&element]() -> nb::object {
		auto span = element.value_span();
		return nb::bytes(reinterpret_cast<const char*>(span.data()), span.size());
	};
	const auto raw_memoryview = [&element, parent]() -> nb::object {
		nb::object owner;
		if (parent.is_valid() && !parent.is_none()) {
			owner = nb::borrow<nb::object>(parent);
		} else {
			owner = nb::cast(&element, nb::rv_policy::reference);
		}
		auto span = element.value_span();
		return readonly_memoryview_from_span(span.data(), span.size(), owner);
	};
	const auto raw_string_scalar = [&element, &raw_bytes]() -> nb::object {
		if (auto v = element.to_string_view()) {
			return nb::str(v->data(), v->size());
		}
		return raw_bytes();
	};
	const auto raw_string_multi = [&element, &raw_bytes]() -> nb::object {
		if (auto v = element.to_string_views()) {
			nb::list out;
			for (const auto& sv : *v) {
				out.append(nb::str(sv.data(), sv.size()));
			}
			return out;
		}
		return raw_bytes();
	};
	const int vm = element.vm();
	switch (element.vr().value) {
	case dicom::VR::None_val:
		return nb::none();
	case dicom::VR::SQ_val: {
		auto* seq = element.as_sequence();
		if (!seq) {
			return nb::none();
		}
		nb::handle keep =
		    parent.is_none() ? nb::cast(element.parent(), nb::rv_policy::reference) : parent;
		return nb::cast(seq, nb::rv_policy::reference_internal, keep);
	}
	case dicom::VR::PX_val: {
		auto* pix = element.as_pixel_sequence();
		if (!pix) {
			return nb::none();
		}
		nb::handle keep =
		    parent.is_none() ? nb::cast(element.parent(), nb::rv_policy::reference) : parent;
		return nb::cast(pix, nb::rv_policy::reference_internal, keep);
	}
	case dicom::VR::PN_val:
		if (vm <= 1) {
			if (auto v = element.to_person_name()) {
				return nb::cast(*v);
			}
			if (auto v = element.to_utf8_string()) {
				return nb::cast(*v);
			}
			return raw_bytes();
		}
		if (auto v = element.to_person_names()) {
			return nb::cast(*v);
		}
		if (auto v = element.to_utf8_strings()) {
			return nb::cast(*v);
		}
		return raw_bytes();
	case dicom::VR::LO_val:
	case dicom::VR::LT_val:
	case dicom::VR::SH_val:
	case dicom::VR::ST_val:
	case dicom::VR::UC_val:
	case dicom::VR::UT_val:
		if (vm <= 1) {
			if (auto v = element.to_utf8_string()) {
				return nb::cast(*v);
			}
			return raw_bytes();
		}
		if (auto v = element.to_utf8_strings()) {
			return nb::cast(*v);
		}
		return raw_bytes();
	case dicom::VR::AE_val:
	case dicom::VR::AS_val:
	case dicom::VR::CS_val:
	case dicom::VR::DA_val:
	case dicom::VR::DT_val:
	case dicom::VR::TM_val:
	case dicom::VR::UI_val:
	case dicom::VR::UR_val:
		return vm <= 1 ? raw_string_scalar() : raw_string_multi();
	case dicom::VR::IS_val:
		if (vm == 0) {
			return empty_py_list();
		}
		if (vm == 1) {
			if (auto value = element.to_longlong()) {
				return nb::cast(*value);
			}
			return raw_string_scalar();
		}
		if (auto values = element.to_longlong_vector()) {
			return vector_to_py_list(*values);
		}
		return raw_string_multi();
	case dicom::VR::DS_val:
		if (vm == 0) {
			return empty_py_list();
		}
		if (vm == 1) {
			if (auto value = element.to_double()) {
				return nb::cast(*value);
			}
			return raw_string_scalar();
		}
		if (auto values = element.to_double_vector()) {
			return vector_to_py_list(*values);
		}
		return raw_string_multi();
	case dicom::VR::AT_val:
		if (vm == 0) {
			return empty_py_list();
		}
		if (vm == 1) {
			if (auto value = element.to_tag()) {
				return nb::cast(*value);
			}
			return raw_memoryview();
		}
		if (auto values = element.to_tag_vector()) {
			return vector_to_py_list(*values);
		}
		return raw_memoryview();
	case dicom::VR::FD_val:
	case dicom::VR::FL_val:
		if (vm == 0) {
			return empty_py_list();
		}
		if (vm == 1) {
			if (auto value = element.to_double()) {
				return nb::cast(*value);
			}
			return raw_memoryview();
		}
		if (auto values = element.to_double_vector()) {
			return vector_to_py_list(*values);
		}
		return raw_memoryview();
	case dicom::VR::SL_val:
	case dicom::VR::SS_val:
	case dicom::VR::SV_val:
	case dicom::VR::UL_val:
	case dicom::VR::US_val:
	case dicom::VR::UV_val:
		if (vm == 0) {
			return empty_py_list();
		}
		if (vm == 1) {
			if (auto value = element.to_longlong()) {
				return nb::cast(*value);
			}
			return raw_memoryview();
		}
		if (auto values = element.to_longlong_vector()) {
			return vector_to_py_list(*values);
		}
		return raw_memoryview();
	case dicom::VR::OB_val:
	case dicom::VR::OD_val:
	case dicom::VR::OF_val:
	case dicom::VR::OL_val:
	case dicom::VR::OW_val:
	case dicom::VR::OV_val:
	case dicom::VR::UN_val:
		return raw_memoryview();
	default:
		if (element.vr().is_string()) {
			return vm <= 1 ? raw_string_scalar() : raw_string_multi();
		}
		return raw_memoryview();
	}
}

std::vector<std::uint8_t> pybuffer_to_bytes(nb::handle value) {
	Py_buffer view{};
	if (PyObject_GetBuffer(value.ptr(), &view, PyBUF_CONTIG_RO) != 0) {
		throw nb::python_error();
	}
	std::vector<std::uint8_t> bytes;
	bytes.resize(static_cast<std::size_t>(view.len));
	if (view.len > 0) {
		std::memcpy(bytes.data(), view.buf, static_cast<std::size_t>(view.len));
	}
	PyBuffer_Release(&view);
	return bytes;
}

bool try_adopt_typed_binary_buffer(DataElement& element, nb::handle value) {
	if (value.is_none() || PyUnicode_Check(value.ptr()) || !PyObject_CheckBuffer(value.ptr())) {
		return false;
	}

	Py_buffer view{};
	if (PyObject_GetBuffer(value.ptr(), &view, PyBUF_C_CONTIGUOUS | PyBUF_FORMAT) != 0) {
		PyErr_Clear();
		return false;
	}

	const auto release = [&]() { PyBuffer_Release(&view); };
	if (view.len < 0 || view.itemsize <= 0 || (view.len > 0 && view.buf == nullptr) ||
	    view.format == nullptr || view.format[0] == '\0') {
		release();
		return false;
	}

	const auto format = strip_pep3118_endianness_prefix(std::string_view(view.format));
	const auto matches = [&](std::size_t itemsize, std::initializer_list<char> codes) {
		if (static_cast<std::size_t>(view.itemsize) != itemsize || format.size() != 1) {
			return false;
		}
		for (char code : codes) {
			if (format.front() == code) {
				return true;
			}
		}
		return false;
	};

	bool ok = false;
	switch (element.vr().value) {
	case VR::OB_val:
		ok = matches(1, {'B', 'b'});
		break;
	case VR::OF_val:
		ok = matches(4, {'f'});
		break;
	case VR::OD_val:
		ok = matches(8, {'d'});
		break;
	case VR::OW_val:
		ok = matches(2, {'H', 'h'});
		break;
	case VR::OL_val:
		ok = matches(4, {'I', 'i', 'L', 'l'});
		break;
	case VR::OV_val:
		ok = matches(8, {'Q', 'q', 'L', 'l'});
		break;
	default:
		break;
	}

	if (!ok) {
		release();
		return false;
	}

	std::vector<std::uint8_t> bytes(static_cast<std::size_t>(view.len));
	if (view.len > 0) {
		std::memcpy(bytes.data(), view.buf, static_cast<std::size_t>(view.len));
	}
	release();
	element.adopt_value_bytes(std::move(bytes));
	return true;
}

bool dataelement_set_value_py(DataElement& element, nb::handle value) {
	if (element.is_missing()) {
		throw nb::value_error("Cannot assign to a missing DataElement");
	}
	if (element.vr().is_sequence() || element.vr().is_pixel_sequence()) {
		throw nb::type_error("Sequence and pixel-sequence elements do not support direct scalar assignment");
	}
	if (value.is_none()) {
		switch (element.vr().value) {
		case VR::PN_val: {
			std::vector<std::string_view> empty;
			return element.from_utf8_views(empty);
		}
		case VR::LO_val:
		case VR::LT_val:
		case VR::SH_val:
		case VR::ST_val:
		case VR::UC_val:
		case VR::UT_val:
		case VR::AE_val:
		case VR::AS_val:
		case VR::CS_val:
		case VR::DA_val:
		case VR::DT_val:
		case VR::TM_val:
		case VR::UI_val:
		case VR::UR_val: {
			std::vector<std::string_view> empty;
			return element.vr().uses_specific_character_set() ? element.from_utf8_views(empty)
			                                                  : element.from_string_views(empty);
		}
		case VR::FD_val:
		case VR::FL_val:
		case VR::DS_val:
			return element.from_double_vector({});
		case VR::IS_val:
		case VR::SL_val:
		case VR::SS_val:
		case VR::SV_val:
		case VR::UL_val:
		case VR::US_val:
		case VR::UV_val:
			return element.from_longlong_vector({});
		case VR::AT_val:
			return element.from_tag_vector({});
		case VR::OB_val:
		case VR::OD_val:
		case VR::OF_val:
		case VR::OL_val:
		case VR::OW_val:
		case VR::OV_val:
		case VR::UN_val:
			element.adopt_value_bytes({});
			return true;
		case VR::SQ_val:
		case VR::PX_val:
		case VR::None_val:
		default:
			throw nb::type_error("None is not supported for this DICOM VR");
		}
	}

	PyObject* obj = value.ptr();
	PyTypeObject* tp = Py_TYPE(obj);

	const auto is_bytes_like = [&]() -> bool {
		return !value.is_none() && !PyUnicode_Check(obj) && PyObject_CheckBuffer(obj);
	};
	const auto adopt_bytes = [&]() -> bool {
		auto bytes = pybuffer_to_bytes(value);
		element.adopt_value_bytes(std::move(bytes));
		return true;
	};
	const auto is_sequence_like = [&]() -> bool {
		return PyList_Check(obj) || PyTuple_Check(obj);
	};
	const auto get_sequence = [&]() -> std::pair<nb::sequence, std::size_t> {
		nb::sequence seq = nb::borrow<nb::sequence>(value);
		const Py_ssize_t size_ssize = PySequence_Size(obj);
		if (size_ssize < 0) {
			throw nb::python_error();
		}
		return {seq, static_cast<std::size_t>(size_ssize)};
	};
	const auto from_string_list = [&](nb::sequence seq, std::size_t size) -> bool {
		std::vector<std::string> storage;
		std::vector<std::string_view> views;
		storage.reserve(size);
		views.reserve(size);
		for (std::size_t i = 0; i < size; ++i) {
			storage.push_back(nb::cast<std::string>(seq[i]));
			views.push_back(storage.back());
		}
		return element.vr().uses_specific_character_set() ? element.from_utf8_views(views)
		                                                  : element.from_string_views(views);
	};
	const auto from_person_name_list = [&](nb::sequence seq, std::size_t size) -> bool {
		std::vector<dicom::PersonName> values;
		values.reserve(size);
		for (std::size_t i = 0; i < size; ++i) {
			values.push_back(nb::cast<dicom::PersonName>(seq[i]));
		}
		return element.from_person_names(values);
	};
	const auto from_tag_list = [&](nb::sequence seq, std::size_t size) -> bool {
		std::vector<Tag> values;
		values.reserve(size);
		for (std::size_t i = 0; i < size; ++i) {
			values.push_back(nb::cast<Tag>(seq[i]));
		}
		return element.from_tag_vector(values);
	};
	const auto from_packed_tag_list = [&](nb::sequence seq, std::size_t size) -> bool {
		std::vector<Tag> values;
		values.reserve(size);
		for (std::size_t i = 0; i < size; ++i) {
			values.emplace_back(nb::cast<std::uint32_t>(seq[i]));
		}
		return element.from_tag_vector(values);
	};
	const auto from_double_list = [&](nb::sequence seq, std::size_t size) -> bool {
		std::vector<double> values;
		values.reserve(size);
		for (std::size_t i = 0; i < size; ++i) {
			values.push_back(nb::cast<double>(seq[i]));
		}
		return element.from_double_vector(values);
	};
	const auto from_long_list = [&](nb::sequence seq, std::size_t size) -> bool {
		std::vector<long long> values;
		values.reserve(size);
		for (std::size_t i = 0; i < size; ++i) {
			values.push_back(nb::cast<long long>(seq[i]));
		}
		return element.from_longlong_vector(values);
	};

	switch (element.vr().value) {
	case VR::PN_val:
		if (value.type().is(nb::type<dicom::PersonName>())) {
			return element.from_person_name(nb::cast<dicom::PersonName>(value));
		}
		if (PyUnicode_Check(obj)) {
			return element.from_utf8_view(nb::cast<std::string>(value));
		}
		if (is_sequence_like()) {
			auto [seq, size] = get_sequence();
			if (size == 0) {
				std::vector<std::string_view> empty;
				return element.from_utf8_views(empty);
			}
			nb::handle first = seq[0];
			if (first.type().is(nb::type<dicom::PersonName>())) {
				return from_person_name_list(seq, size);
			}
			if (PyUnicode_Check(first.ptr())) {
				return from_string_list(seq, size);
			}
			throw nb::type_error("PN assignment expects PersonName, str, or a sequence of those");
		}
		if (is_bytes_like()) {
			return adopt_bytes();
		}
		throw nb::type_error("PN assignment expects PersonName, str, or bytes-like");

	case VR::LO_val:
	case VR::LT_val:
	case VR::SH_val:
	case VR::ST_val:
	case VR::UC_val:
	case VR::UT_val:
	case VR::AE_val:
	case VR::AS_val:
	case VR::CS_val:
	case VR::DA_val:
	case VR::DT_val:
	case VR::TM_val:
	case VR::UI_val:
	case VR::UR_val:
		if (PyUnicode_Check(obj)) {
			return element.vr().uses_specific_character_set() ? element.from_utf8_view(nb::cast<std::string>(value))
			                                                  : element.from_string_view(nb::cast<std::string>(value));
		}
		if (is_sequence_like()) {
			auto [seq, size] = get_sequence();
			if (size == 0) {
				std::vector<std::string_view> empty;
				return element.vr().uses_specific_character_set() ? element.from_utf8_views(empty)
				                                                  : element.from_string_views(empty);
			}
			if (!PyUnicode_Check(seq[0].ptr())) {
				throw nb::type_error("String VR assignment expects str or a sequence of str");
			}
			return from_string_list(seq, size);
		}
		if (is_bytes_like()) {
			return adopt_bytes();
		}
		throw nb::type_error("String VR assignment expects str or bytes-like");

	case VR::FD_val:
	case VR::FL_val:
		if (tp == &PyFloat_Type || tp == &PyLong_Type || PyFloat_Check(obj) || PyLong_Check(obj)) {
			return element.from_double(nb::cast<double>(value));
		}
		if (PyUnicode_Check(obj)) {
			return element.from_string_view(nb::cast<std::string>(value));
		}
		if (is_sequence_like()) {
			auto [seq, size] = get_sequence();
			if (size == 0) {
				return element.from_double_vector({});
			}
			if (PyUnicode_Check(seq[0].ptr())) {
				return from_string_list(seq, size);
			}
			return from_double_list(seq, size);
		}
		if (is_bytes_like()) {
			return adopt_bytes();
		}
		throw nb::type_error("Floating-point VR assignment expects float, str, or bytes-like");

	case VR::DS_val:
		if (tp == &PyFloat_Type || tp == &PyLong_Type || PyFloat_Check(obj) || PyLong_Check(obj)) {
			return element.from_double(nb::cast<double>(value));
		}
		if (PyUnicode_Check(obj)) {
			return element.from_string_view(nb::cast<std::string>(value));
		}
		if (is_sequence_like()) {
			auto [seq, size] = get_sequence();
			if (size == 0) {
				return element.from_double_vector({});
			}
			if (PyUnicode_Check(seq[0].ptr())) {
				return from_string_list(seq, size);
			}
			return from_double_list(seq, size);
		}
		if (is_bytes_like()) {
			return adopt_bytes();
		}
		throw nb::type_error("DS assignment expects float, str, or bytes-like");

	case VR::IS_val:
	case VR::SL_val:
	case VR::SS_val:
	case VR::SV_val:
	case VR::UL_val:
	case VR::US_val:
	case VR::UV_val:
		if (tp == &PyLong_Type || PyLong_Check(obj)) {
			return element.from_longlong(nb::cast<long long>(value));
		}
		if (element.vr() == VR::IS && PyUnicode_Check(obj)) {
			return element.from_string_view(nb::cast<std::string>(value));
		}
		if (is_sequence_like()) {
			auto [seq, size] = get_sequence();
			if (size == 0) {
				return element.from_longlong_vector({});
			}
			if (element.vr() == VR::IS && PyUnicode_Check(seq[0].ptr())) {
				return from_string_list(seq, size);
			}
			return from_long_list(seq, size);
		}
		if (is_bytes_like()) {
			return adopt_bytes();
		}
		throw nb::type_error("Integer VR assignment expects int, str for IS, or bytes-like");

	case VR::AT_val:
		if (value.type().is(nb::type<Tag>())) {
			return element.from_tag(nb::cast<Tag>(value));
		}
		if (tp == &PyLong_Type || PyLong_Check(obj)) {
			return element.from_tag(Tag(nb::cast<std::uint32_t>(value)));
		}
		if (is_sequence_like()) {
			auto [seq, size] = get_sequence();
			if (size == 0) {
				return element.from_tag_vector({});
			}
			nb::handle first = seq[0];
			if (first.type().is(nb::type<Tag>())) {
				return from_tag_list(seq, size);
			}
			if (PyLong_Check(first.ptr())) {
				return from_packed_tag_list(seq, size);
			}
			throw nb::type_error("AT assignment expects Tag, packed int, or a sequence of those");
		}
		if (is_bytes_like()) {
			return adopt_bytes();
		}
		throw nb::type_error("AT assignment expects Tag, packed int, or bytes-like");

	case VR::OB_val:
	case VR::OD_val:
	case VR::OF_val:
	case VR::OL_val:
	case VR::OW_val:
	case VR::OV_val:
		if (try_adopt_typed_binary_buffer(element, value)) {
			return true;
		}
		if (is_bytes_like()) {
			return adopt_bytes();
		}
		throw nb::type_error(
		    "Binary VR assignment expects a bytes-like object or a matching typed array");
	case VR::UN_val:
		if (is_bytes_like()) {
			return adopt_bytes();
		}
		throw nb::type_error("Binary VR assignment expects a bytes-like object");

	case VR::SQ_val:
	case VR::PX_val:
	case VR::None_val:
	default:
		if (is_bytes_like()) {
			return adopt_bytes();
		}
		throw nb::type_error("Unsupported Python value type for DICOM DataElement assignment");
	}
}

void dataelement_set_value_or_throw_py(DataElement& element, nb::handle value) {
	const bool ok = dataelement_set_value_py(element, value);
	if (!ok) {
		throw nb::value_error(
		    ("Failed to assign value to " + element.tag().to_string() + " (" +
		        std::string(element.vr().str()) + ")")
		        .c_str());
	}
}

nb::object dataelement_to_tag_vector_py(const DataElement& element) {
	if (element.vm() == 0) {
		return empty_py_list();
	}
	auto values = element.to_tag_vector();
	if (!values) {
		return nb::none();
	}
	return vector_to_py_list(*values);
}

nb::object dataelement_to_int_vector_py(const DataElement& element, nb::object default_value) {
	if (default_value.is_none() && element.vm() == 0) {
		return empty_py_list();
	}
	if (default_value.is_none()) {
		auto values = element.to_int_vector();
		if (!values) {
			return nb::none();
		}
		return vector_to_py_list(*values);
	}
	return vector_to_py_list(
	    element.to_int_vector().value_or(nb::cast<std::vector<int>>(default_value)));
}

nb::object dataelement_to_long_vector_py(const DataElement& element, nb::object default_value) {
	if (default_value.is_none() && element.vm() == 0) {
		return empty_py_list();
	}
	if (default_value.is_none()) {
		auto values = element.to_long_vector();
		if (!values) {
			return nb::none();
		}
		return vector_to_py_list(*values);
	}
	return vector_to_py_list(
	    element.to_long_vector().value_or(nb::cast<std::vector<long>>(default_value)));
}

nb::object dataelement_to_longlong_vector_py(
    const DataElement& element, nb::object default_value) {
	if (default_value.is_none() && element.vm() == 0) {
		return empty_py_list();
	}
	if (default_value.is_none()) {
		auto values = element.to_longlong_vector();
		if (!values) {
			return nb::none();
		}
		return vector_to_py_list(*values);
	}
	return vector_to_py_list(
	    element.to_longlong_vector().value_or(nb::cast<std::vector<long long>>(default_value)));
}

nb::object dataelement_to_double_vector_py(
    const DataElement& element, nb::object default_value) {
	if (default_value.is_none() && element.vm() == 0) {
		return empty_py_list();
	}
	if (default_value.is_none()) {
		auto values = element.to_double_vector();
		if (!values) {
			return nb::none();
		}
		return vector_to_py_list(*values);
	}
	return vector_to_py_list(
	    element.to_double_vector().value_or(nb::cast<std::vector<double>>(default_value)));
}

DataElement& dataset_lookup_dataelement_py(
    DataSet& self, nb::handle key, const char* key_error_message) {
	if (nb::isinstance<Tag>(key)) {
		return self.get_dataelement(nb::cast<Tag>(key));
	}
	if (nb::isinstance<nb::int_>(key)) {
		return self.get_dataelement(Tag(nb::cast<std::uint32_t>(key)));
	}
	if (nb::isinstance<nb::str>(key)) {
		return self.get_dataelement(nb::cast<std::string>(key));
	}
	throw nb::type_error(key_error_message);
}

bool dataset_contains_py(DataSet& self, nb::handle key, const char* key_error_message) {
	if (nb::isinstance<Tag>(key)) {
		return self.get_dataelement(nb::cast<Tag>(key)).is_present();
	}
	if (nb::isinstance<nb::int_>(key)) {
		return self.get_dataelement(Tag(nb::cast<std::uint32_t>(key))).is_present();
	}
	if (nb::isinstance<nb::str>(key)) {
		try {
			return self.get_dataelement(nb::cast<std::string>(key)).is_present();
		} catch (const std::exception&) {
			return false;
		}
	}
	throw nb::type_error(key_error_message);
}

Tag dataset_assignment_key_to_tag(nb::handle key);
std::optional<std::string> dataset_assignment_key_to_text(nb::handle key);

bool dataset_try_set_value_py(DataSet& self, nb::handle key, nb::handle value) {
	if (auto text = dataset_assignment_key_to_text(key)) {
		return dataelement_set_value_py(self.ensure_dataelement(*text), value);
	}
	const Tag tag = dataset_assignment_key_to_tag(key);
	return dataelement_set_value_py(self.ensure_dataelement(tag), value);
}

bool dataset_try_set_value_with_vr_py(
    DataSet& self, nb::handle key, VR vr, nb::handle value) {
	if (auto text = dataset_assignment_key_to_text(key)) {
		DataElement& target = self.ensure_dataelement(*text, vr);
		if (target.vr().is_sequence() || target.vr().is_pixel_sequence()) {
			return false;
		}
		return dataelement_set_value_py(target, value);
	}
	const Tag tag = dataset_assignment_key_to_tag(key);
	if (vr == VR::None) {
		return dataset_try_set_value_py(self, key, value);
	}
	DataElement& target = self.ensure_dataelement(tag, vr);
	if (target.vr().is_sequence() || target.vr().is_pixel_sequence()) {
		return false;
	}
	return dataelement_set_value_py(target, value);
}

Tag dataset_assignment_key_to_tag(nb::handle key) {
	if (nb::isinstance<Tag>(key)) {
		return nb::cast<Tag>(key);
	}
	if (nb::isinstance<nb::int_>(key)) {
		return Tag(nb::cast<std::uint32_t>(key));
	}
	if (nb::isinstance<nb::str>(key)) {
		const std::string text = nb::cast<std::string>(key);
		if (text.find('.') != std::string::npos) {
			throw nb::type_error("Assignment does not support nested tag-path strings");
		}
		return Tag(text);
	}
	throw nb::type_error("DataSet assignment keys must be Tag, int (0xGGGEEEE), or str");
}

std::optional<std::string> dataset_assignment_key_to_text(nb::handle key) {
	if (!nb::isinstance<nb::str>(key)) {
		return std::nullopt;
	}
	return nb::cast<std::string>(key);
}

void dataset_set_value_py(DataSet& self, nb::handle key, nb::handle value) {
	if (!dataset_try_set_value_py(self, key, value)) {
		std::string target_name;
		const DataElement* element = nullptr;
		if (auto text = dataset_assignment_key_to_text(key)) {
			target_name = *text;
			element = &self.get_dataelement(*text);
		} else {
			const Tag tag = dataset_assignment_key_to_tag(key);
			target_name = tag.to_string();
			element = &self.get_dataelement(tag);
		}
		const VR vr = element->is_present() ? element->vr() : VR::None;
		throw nb::value_error(
		    ("Failed to assign value to " + target_name + " (" + std::string(vr.str()) + ")")
		        .c_str());
	}
}

bool is_dicom_assignment_attr_name(std::string_view attr_name) {
	const auto [tag, _vr] = dicom::lookup::keyword_to_tag_vr(attr_name);
	return static_cast<bool>(tag);
}

int dataset_setattro(PyObject* obj, PyObject* name, PyObject* value) {
	if (!name || !PyUnicode_Check(name)) {
		return PyObject_GenericSetAttr(obj, name, value);
	}
	try {
		std::string attr_name = nb::cast<std::string>(nb::handle(name));
		if (attr_name.empty() || attr_name[0] == '_' || value == nullptr ||
		    !is_dicom_assignment_attr_name(attr_name)) {
			return PyObject_GenericSetAttr(obj, name, value);
		}
		DataSet* self = nb::inst_ptr<DataSet>(nb::handle(obj));
		dataset_set_value_py(*self, nb::handle(name), nb::handle(value));
		return 0;
	} catch (const nb::python_error&) {
		return -1;
	} catch (const std::exception& ex) {
		PyErr_SetString(PyExc_RuntimeError, ex.what());
		return -1;
	}
}

int dicomfile_setattro(PyObject* obj, PyObject* name, PyObject* value) {
	if (!name || !PyUnicode_Check(name)) {
		return PyObject_GenericSetAttr(obj, name, value);
	}
	try {
		std::string attr_name = nb::cast<std::string>(nb::handle(name));
		if (attr_name.empty() || attr_name[0] == '_' || value == nullptr ||
		    !is_dicom_assignment_attr_name(attr_name)) {
			return PyObject_GenericSetAttr(obj, name, value);
		}
		DicomFile* self = nb::inst_ptr<DicomFile>(nb::handle(obj));
		dataset_set_value_py(self->dataset(), nb::handle(name), nb::handle(value));
		return 0;
	} catch (const nb::python_error&) {
		return -1;
	} catch (const std::exception& ex) {
		PyErr_SetString(PyExc_RuntimeError, ex.what());
		return -1;
	}
}

PyType_Slot dataset_type_slots[] = {
	{Py_tp_setattro, reinterpret_cast<void*>(dataset_setattro)},
	{0, nullptr},
};

PyType_Slot dicomfile_type_slots[] = {
	{Py_tp_setattro, reinterpret_cast<void*>(dicomfile_setattro)},
	{0, nullptr},
};

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

struct ReadonlySpanExporter {
	PyObject_HEAD
	const char* data;
	Py_ssize_t size;
	PyObject* owner;
};

int readonly_span_exporter_getbuffer(PyObject* exporter, Py_buffer* view, int flags) {
	auto* self = reinterpret_cast<ReadonlySpanExporter*>(exporter);
	char* ptr = self->size == 0 ? const_cast<char*>("") : const_cast<char*>(self->data);
	return PyBuffer_FillInfo(view, exporter, ptr, self->size, 1, flags);
}

void readonly_span_exporter_dealloc(PyObject* exporter) {
	auto* self = reinterpret_cast<ReadonlySpanExporter*>(exporter);
	Py_XDECREF(self->owner);
#if defined(Py_LIMITED_API)
	PyTypeObject* type = Py_TYPE(exporter);
	auto* tp_free = reinterpret_cast<freefunc>(PyType_GetSlot(type, Py_tp_free));
	if (tp_free == nullptr) {
		PyObject_Free(exporter);
	}
	else {
		tp_free(exporter);
	}
	Py_DECREF(reinterpret_cast<PyObject*>(type));
#else
	Py_TYPE(exporter)->tp_free(exporter);
#endif
}

#if defined(Py_LIMITED_API)
PyObject* readonly_span_exporter_type() {
	static PyType_Slot slots[] = {
	    {Py_tp_dealloc, reinterpret_cast<void*>(readonly_span_exporter_dealloc)},
	    {Py_bf_getbuffer, reinterpret_cast<void*>(readonly_span_exporter_getbuffer)},
	    {Py_tp_new, reinterpret_cast<void*>(PyType_GenericNew)},
	    {0, nullptr},
	};
	static PyType_Spec spec = {
	    "dicomsdl._ReadonlySpanExporter",
	    static_cast<int>(sizeof(ReadonlySpanExporter)),
	    0,
	    Py_TPFLAGS_DEFAULT,
	    slots,
	};
	static PyObject* type = []() -> PyObject* {
		PyObject* result = PyType_FromSpec(&spec);
		if (result == nullptr) {
			throw nb::python_error();
		}
		return result;
	}();
	return type;
}
#else
PyTypeObject* readonly_span_exporter_type() {
	static PyBufferProcs buffer_procs = {
	    .bf_getbuffer = readonly_span_exporter_getbuffer,
	    .bf_releasebuffer = nullptr,
	};
	static PyTypeObject type = {
	    PyVarObject_HEAD_INIT(nullptr, 0)
	};
	static bool initialized = false;
	if (!initialized) {
		type.tp_name = "dicomsdl._ReadonlySpanExporter";
		type.tp_basicsize = sizeof(ReadonlySpanExporter);
		type.tp_flags = Py_TPFLAGS_DEFAULT;
		type.tp_dealloc = readonly_span_exporter_dealloc;
		type.tp_as_buffer = &buffer_procs;
		if (PyType_Ready(&type) < 0) {
			throw nb::python_error();
		}
		initialized = true;
	}
	return &type;
}
#endif

nb::object readonly_memoryview_from_span(
    const void* data, std::size_t size, nb::handle owner) {
#if defined(Py_LIMITED_API)
	PyObject* type = readonly_span_exporter_type();
	PyObject* exporter_raw = PyObject_CallNoArgs(type);
	if (exporter_raw == nullptr) {
		throw nb::python_error();
	}
	auto* exporter = reinterpret_cast<ReadonlySpanExporter*>(exporter_raw);
#else
	auto* type = readonly_span_exporter_type();
	auto* exporter = PyObject_New(ReadonlySpanExporter, type);
	if (exporter == nullptr) {
		throw nb::python_error();
	}
#endif
	exporter->data = reinterpret_cast<const char*>(data);
	exporter->size = static_cast<Py_ssize_t>(size);
	exporter->owner =
	    (!owner.is_valid() || owner.is_none()) ? nullptr : new_reference_or_null(owner.ptr());
#if defined(Py_LIMITED_API)
	nb::object exporter_obj = nb::steal<nb::object>(exporter_raw);
#else
	nb::object exporter_obj = nb::steal<nb::object>(reinterpret_cast<PyObject*>(exporter));
#endif
	PyObject* memoryview = PyMemoryView_FromObject(exporter_obj.ptr());
	if (memoryview == nullptr) {
		throw nb::python_error();
	}
	return nb::steal<nb::object>(memoryview);
}

std::optional<dicom::SpecificCharacterSet> parse_specific_character_set_option(
    nb::handle value, const char* argument_name) {
	if (value.is_none()) {
		return std::nullopt;
	}
	if (!nb::isinstance<nb::str>(value)) {
		throw nb::type_error((std::string(argument_name) + " must be str or None").c_str());
	}
	const auto term = nb::cast<std::string>(value);
	if (term.empty()) {
		return dicom::SpecificCharacterSet::NONE;
	}
	const auto charset = dicom::specific_character_set_from_term(term);
	if (charset == dicom::SpecificCharacterSet::Unknown) {
		const auto message = std::string("Unknown Specific Character Set: ") + term;
		throw nb::value_error(message.c_str());
	}
	return charset;
}

std::vector<dicom::SpecificCharacterSet> parse_specific_character_sets_option(
    nb::handle value, const char* argument_name) {
	if (value.is_none()) {
		return {};
	}
	if (nb::isinstance<nb::str>(value)) {
		throw nb::type_error(
		    (std::string(argument_name) + " must be a sequence of strings or None").c_str());
	}
	const auto terms = nb::cast<std::vector<std::string>>(value);
	if (terms.empty()) {
		throw nb::value_error((std::string(argument_name) + " must not be empty").c_str());
	}
	std::vector<dicom::SpecificCharacterSet> parsed_terms;
	parsed_terms.reserve(terms.size());
	for (std::size_t index = 0; index < terms.size(); ++index) {
		const auto& term = terms[index];
		if (term.empty()) {
			if (index == 0u) {
				parsed_terms.push_back(dicom::SpecificCharacterSet::NONE);
				continue;
			}
			throw nb::value_error((std::string(argument_name) +
			                       " may contain an empty term only in the first position")
			                          .c_str());
		}
		const auto charset = dicom::specific_character_set_from_term(term);
		if (charset == dicom::SpecificCharacterSet::Unknown) {
			const auto message = std::string("Unknown Specific Character Set: ") + term;
			throw nb::value_error(message.c_str());
		}
		parsed_terms.push_back(charset);
	}
	return parsed_terms;
}

std::vector<dicom::SpecificCharacterSet> parse_specific_character_set_argument(
    nb::handle value, const char* argument_name) {
	if (value.is_none()) {
		return {dicom::SpecificCharacterSet::NONE};
	}
	if (nb::isinstance<nb::str>(value)) {
		const auto single = parse_specific_character_set_option(value, argument_name);
		if (single.has_value()) {
			return {*single};
		}
		return {dicom::SpecificCharacterSet::NONE};
	}
	return parse_specific_character_sets_option(value, argument_name);
}

dicom::CharsetEncodeErrorPolicy parse_charset_encode_error_policy(
    nb::handle value, const char* argument_name) {
	if (value.is_none()) {
		return dicom::CharsetEncodeErrorPolicy::strict;
	}
	if (!nb::isinstance<nb::str>(value)) {
		throw nb::type_error((std::string(argument_name) + " must be str or None").c_str());
	}
	const auto text = nb::cast<std::string>(value);
	if (text == "strict") {
		return dicom::CharsetEncodeErrorPolicy::strict;
	}
	if (text == "replace_qmark") {
		return dicom::CharsetEncodeErrorPolicy::replace_qmark;
	}
	if (text == "replace_unicode_escape") {
		return dicom::CharsetEncodeErrorPolicy::replace_unicode_escape;
	}
	throw nb::value_error(
	    (std::string(argument_name) +
	        " must be one of: 'strict', 'replace_qmark', 'replace_unicode_escape'")
	        .c_str());
}

dicom::CharsetDecodeErrorPolicy parse_charset_decode_error_policy(
    nb::handle value, const char* argument_name) {
	if (value.is_none()) {
		return dicom::CharsetDecodeErrorPolicy::strict;
	}
	if (!nb::isinstance<nb::str>(value)) {
		throw nb::type_error((std::string(argument_name) + " must be str or None").c_str());
	}
	const auto text = nb::cast<std::string>(value);
	if (text == "strict") {
		return dicom::CharsetDecodeErrorPolicy::strict;
	}
	if (text == "replace_fffd") {
		return dicom::CharsetDecodeErrorPolicy::replace_fffd;
	}
	if (text == "replace_hex_escape") {
		return dicom::CharsetDecodeErrorPolicy::replace_hex_escape;
	}
	throw nb::value_error(
	    (std::string(argument_name) +
	        " must be one of: 'strict', 'replace_fffd', 'replace_hex_escape'")
	        .c_str());
}

dicom::JsonBulkDataMode parse_json_bulk_data_mode(
    nb::handle value, const char* argument_name) {
	if (value.is_none()) {
		return dicom::JsonBulkDataMode::inline_;
	}
	if (!nb::isinstance<nb::str>(value)) {
		throw nb::type_error((std::string(argument_name) + " must be str or None").c_str());
	}
	const auto text = nb::cast<std::string>(value);
	if (text == "inline") {
		return dicom::JsonBulkDataMode::inline_;
	}
	if (text == "uri") {
		return dicom::JsonBulkDataMode::uri;
	}
	if (text == "omit") {
		return dicom::JsonBulkDataMode::omit;
	}
	throw nb::value_error(
	    (std::string(argument_name) + " must be one of: 'inline', 'uri', 'omit'").c_str());
}

dicom::PersonNameGroup make_person_name_group(
    const std::vector<std::string>& components, std::string_view arg_name) {
	if (components.size() > 5) {
		throw nb::value_error(
		    (std::string(arg_name) + " must have at most 5 PN components").c_str());
	}
	dicom::PersonNameGroup group;
	group.explicit_component_count_ = static_cast<std::uint8_t>(components.size());
	for (std::size_t i = 0; i < components.size(); ++i) {
		group.components[i] = components[i];
	}
	return group;
}

std::optional<dicom::PersonNameGroup> parse_person_name_group_argument(
    nb::handle value, std::string_view arg_name) {
	if (!value || value.is_none()) {
		return std::nullopt;
	}
	if (nb::isinstance<dicom::PersonNameGroup>(value)) {
		return nb::cast<dicom::PersonNameGroup>(value);
	}
	return make_person_name_group(nb::cast<std::vector<std::string>>(value), arg_name);
}

std::vector<std::string> person_name_group_components(
    const dicom::PersonNameGroup& group) {
	return std::vector<std::string>(group.components.begin(), group.components.end());
}

dicom::WriteOptions make_write_options(
    bool include_preamble, bool write_file_meta, bool keep_existing_meta) {
	dicom::WriteOptions options;
	options.include_preamble = include_preamble;
	options.write_file_meta = write_file_meta;
	options.keep_existing_meta = keep_existing_meta;
	return options;
}

dicom::JsonWriteOptions make_json_write_options(
    bool include_group_0002, nb::handle bulk_data, std::size_t bulk_data_threshold,
    const std::string& bulk_data_uri_template, const std::string& pixel_data_uri_template,
    nb::handle charset_errors) {
	dicom::JsonWriteOptions options;
	options.include_group_0002 = include_group_0002;
	options.bulk_data_mode = parse_json_bulk_data_mode(bulk_data, "bulk_data");
	options.bulk_data_threshold = bulk_data_threshold;
	options.bulk_data_uri_template = bulk_data_uri_template;
	options.pixel_data_uri_template = pixel_data_uri_template;
	options.charset_errors = parse_charset_decode_error_policy(charset_errors, "charset_errors");
	return options;
}

dicom::JsonReadOptions make_json_read_options(nb::handle charset_errors) {
	dicom::JsonReadOptions options;
	options.charset_errors = parse_charset_encode_error_policy(charset_errors, "charset_errors");
	return options;
}

nb::bytes to_python_bytes(std::vector<std::uint8_t>&& bytes) {
	if (bytes.empty()) {
		return nb::bytes("", 0);
	}
	return nb::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::pair<std::string, std::vector<std::uint8_t>> json_source_to_named_buffer(
    nb::handle source, std::string name) {
	if (nb::isinstance<nb::str>(source)) {
		auto text = nb::cast<std::string>(source);
		return {std::move(name), std::vector<std::uint8_t>(text.begin(), text.end())};
	}
	return {std::move(name), pybuffer_to_bytes(source)};
}

nb::object json_write_result_to_python(
    dicom::JsonWriteResult&& result, nb::handle owner) {
	nb::list bulk_parts;
	for (const auto& part : result.bulk_parts) {
		const auto bytes = part.bytes();
		bulk_parts.append(nb::make_tuple(
		    nb::str(part.uri.c_str(), part.uri.size()),
		    readonly_memoryview_from_span(bytes.data(), bytes.size(), owner),
		    nb::str(part.media_type.c_str(), part.media_type.size()),
		    nb::str(part.transfer_syntax_uid.c_str(), part.transfer_syntax_uid.size())));
	}
	return nb::make_tuple(
	    nb::str(result.json.c_str(), result.json.size()),
	    std::move(bulk_parts));
}

nb::object json_read_result_to_python(dicom::JsonReadResult&& result) {
	nb::list items;
	for (auto& item : result.items) {
		nb::object py_file = nb::cast(std::move(item.file));
		nb::list pending_bulk_data;
		for (const auto& ref : item.pending_bulk_data) {
			pending_bulk_data.append(nb::cast(ref));
		}
		items.append(nb::make_tuple(std::move(py_file), std::move(pending_bulk_data)));
	}
	return std::move(items);
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

struct PyDataSetWalkIterator;

struct PyDataSetWalkBorrowState {
	PyDataSetWalkIterator* iterator{nullptr};
	std::size_t generation{0};
};

struct PyDataSetWalkPath {
	PyDataSetWalkPath() = default;
	PyDataSetWalkPath(std::shared_ptr<PyDataSetWalkBorrowState> state, std::size_t generation)
	    : state_(std::move(state)), generation_(generation) {}

	[[nodiscard]] dicom::DataSetWalkPathRef ref() const noexcept;

	std::shared_ptr<PyDataSetWalkBorrowState> state_{};
	std::size_t generation_{0};
};

struct PyDataSetWalkEntry {
	PyDataSetWalkEntry() = default;

	PyDataSetWalkEntry(nb::handle owner,
	    std::shared_ptr<PyDataSetWalkBorrowState> borrow_state,
	    std::size_t generation, DataElement* element)
	    : owner_(nb::borrow(owner)),
	      borrow_state_(std::move(borrow_state)),
	      generation_(generation),
	      element_(element) {}

	nb::object path_object() {
		if (!path_object_.is_valid()) {
			path_object_ = nb::cast(PyDataSetWalkPath(borrow_state_, generation_));
		}
		return path_object_;
	}

	nb::object element_object() {
		if (!element_object_.is_valid()) {
			element_object_ =
			    nb::cast(element_, nb::rv_policy::reference_internal, owner_);
		}
		return element_object_;
	}

	void skip_sequence() noexcept;

	void skip_current_dataset() noexcept;

	nb::object owner_{};
	std::shared_ptr<PyDataSetWalkBorrowState> borrow_state_{};
	std::size_t generation_{0};
	DataElement* element_{nullptr};
	mutable nb::object path_object_{};
	mutable nb::object element_object_{};
};

struct PyDataSetWalkEntryIterator {
	PyDataSetWalkEntryIterator() = default;
	explicit PyDataSetWalkEntryIterator(PyDataSetWalkEntry entry)
	    : entry_(std::move(entry)) {}

	nb::object next() {
		if (index_ == 0) {
			++index_;
			return entry_.path_object();
		}
		if (index_ == 1) {
			++index_;
			return entry_.element_object();
		}
		throw nb::stop_iteration();
	}

	PyDataSetWalkEntry entry_{};
	std::uint8_t index_{0};
};

struct PyDataSetWalkIterator {
	PyDataSetWalkIterator(DataSet& data_set, nb::handle owner)
	    : owner_(nb::borrow(owner)),
	      current_(data_set.walk().begin()),
	      end_(data_set.walk().end()),
	      borrow_state_(std::make_shared<PyDataSetWalkBorrowState>()) {
		rebind_borrow_state();
	}

	PyDataSetWalkIterator(const PyDataSetWalkIterator&) = delete;
	PyDataSetWalkIterator& operator=(const PyDataSetWalkIterator&) = delete;

	PyDataSetWalkIterator(PyDataSetWalkIterator&& other) noexcept
	    : owner_(std::move(other.owner_)),
	      current_(std::move(other.current_)),
	      end_(std::move(other.end_)),
	      borrow_state_(std::move(other.borrow_state_)),
	      generation_(other.generation_),
	      pending_advance_(other.pending_advance_) {
		rebind_borrow_state();
	}

	PyDataSetWalkIterator& operator=(PyDataSetWalkIterator&& other) noexcept {
		if (this != &other) {
			release_borrow_state();
			owner_ = std::move(other.owner_);
			current_ = std::move(other.current_);
			end_ = std::move(other.end_);
			borrow_state_ = std::move(other.borrow_state_);
			generation_ = other.generation_;
			pending_advance_ = other.pending_advance_;
			rebind_borrow_state();
		}
		return *this;
	}

	~PyDataSetWalkIterator() {
		release_borrow_state();
	}

	[[nodiscard]] dicom::DataSetWalkPathRef current_path() const noexcept {
		if (current_ == end_) {
			return dicom::DataSetWalkPathRef();
		}
		return (*current_).path;
	}

	PyDataSetWalkEntry next() {
		if (pending_advance_) {
			++current_;
			pending_advance_ = false;
			++generation_;
			if (borrow_state_ != nullptr) {
				borrow_state_->generation = generation_;
			}
		}
		if (current_ == end_) {
			throw nb::stop_iteration();
		}

		auto entry = *current_;
		pending_advance_ = true;
		return PyDataSetWalkEntry(owner_, borrow_state_, generation_, &entry.element);
	}

	void skip_sequence() noexcept {
		if (pending_advance_ && current_ != end_) {
			current_.skip_sequence();
		}
	}

	void skip_current_dataset() noexcept {
		if (pending_advance_ && current_ != end_) {
			current_.skip_current_dataset();
		}
	}

	nb::object owner_;
	dicom::DataSetWalkIterator current_;
	dicom::DataSetWalkIterator end_;
	std::shared_ptr<PyDataSetWalkBorrowState> borrow_state_{};
	std::size_t generation_{0};
	bool pending_advance_{false};

private:
	void rebind_borrow_state() noexcept {
		if (borrow_state_ != nullptr) {
			borrow_state_->iterator = this;
			borrow_state_->generation = generation_;
		}
	}

	void release_borrow_state() noexcept {
		if (borrow_state_ != nullptr && borrow_state_->iterator == this) {
			borrow_state_->iterator = nullptr;
		}
	}
};

void PyDataSetWalkEntry::skip_sequence() noexcept {
	if (borrow_state_ != nullptr && borrow_state_->iterator != nullptr &&
	    borrow_state_->generation == generation_) {
		borrow_state_->iterator->skip_sequence();
	}
}

void PyDataSetWalkEntry::skip_current_dataset() noexcept {
	if (borrow_state_ != nullptr && borrow_state_->iterator != nullptr &&
	    borrow_state_->generation == generation_) {
		borrow_state_->iterator->skip_current_dataset();
	}
}

dicom::DataSetWalkPathRef PyDataSetWalkPath::ref() const noexcept {
	if (state_ == nullptr || state_->iterator == nullptr || state_->generation != generation_) {
		return dicom::DataSetWalkPathRef();
	}
	return state_->iterator->current_path();
}

}  // namespace

NB_MODULE(_dicomsdl, m) {
	m.doc() = "nanobind bindings for DicomSDL";

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
			.def("__init__",
			    [](diag::FileReporter* self, nb::handle path) {
				    new (self) diag::FileReporter(python_path_to_filesystem_path(path, "path"));
			    },
			    nb::arg("path"),
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

	nb::class_<dicom::PersonNameGroup>(m, "PersonNameGroup",
	    "One PN component group with up to 5 components in DICOM order. "
	    "Use component(index) for a neutral view; family_name/given_name/... are "
	    "human-use convenience aliases.")
		.def(nb::init<>())
		.def("__init__",
		    [](dicom::PersonNameGroup* self, const std::vector<std::string>& components) {
			    new (self) dicom::PersonNameGroup(make_person_name_group(components, "components"));
		    },
		    nb::arg("components"))
		.def_prop_rw("components",
		    [](const dicom::PersonNameGroup& group) {
			    return person_name_group_components(group);
		    },
		    [](dicom::PersonNameGroup& group, const std::vector<std::string>& components) {
			    group = make_person_name_group(components, "components");
		    })
		.def("component",
		    [](const dicom::PersonNameGroup& group, std::size_t index) -> std::string {
			    if (index >= group.components.size()) {
				    throw nb::index_error("PN component index must be in [0, 4]");
			    }
			    return group.components[index];
		    },
		    nb::arg("index"))
		.def("empty", &dicom::PersonNameGroup::empty)
		.def("to_dicom_string", &dicom::PersonNameGroup::to_dicom_string)
		.def_prop_rw("family_name",
		    [](const dicom::PersonNameGroup& group) {
			    return std::string(group.family_name());
		    },
		    [](dicom::PersonNameGroup& group, const std::string& value) {
			    group.components[0] = value;
		    })
		.def_prop_rw("given_name",
		    [](const dicom::PersonNameGroup& group) {
			    return std::string(group.given_name());
		    },
		    [](dicom::PersonNameGroup& group, const std::string& value) {
			    group.components[1] = value;
		    })
		.def_prop_rw("middle_name",
		    [](const dicom::PersonNameGroup& group) {
			    return std::string(group.middle_name());
		    },
		    [](dicom::PersonNameGroup& group, const std::string& value) {
			    group.components[2] = value;
		    })
		.def_prop_rw("name_prefix",
		    [](const dicom::PersonNameGroup& group) {
			    return std::string(group.name_prefix());
		    },
		    [](dicom::PersonNameGroup& group, const std::string& value) {
			    group.components[3] = value;
		    })
		.def_prop_rw("name_suffix",
		    [](const dicom::PersonNameGroup& group) {
			    return std::string(group.name_suffix());
		    },
		    [](dicom::PersonNameGroup& group, const std::string& value) {
			    group.components[4] = value;
		    })
		.def("__repr__",
		    [](const dicom::PersonNameGroup& group) {
			    return "PersonNameGroup(" + group.to_dicom_string() + ")";
		    });

	nb::class_<dicom::PersonName>(m, "PersonName",
	    "Structured PN value with alphabetic, ideographic, and phonetic component groups.")
		.def(nb::init<>())
		.def("__init__",
		    [](dicom::PersonName* self, nb::handle alphabetic, nb::handle ideographic,
		        nb::handle phonetic) {
			    dicom::PersonName value;
			    value.alphabetic = parse_person_name_group_argument(alphabetic, "alphabetic");
			    value.ideographic = parse_person_name_group_argument(ideographic, "ideographic");
			    value.phonetic = parse_person_name_group_argument(phonetic, "phonetic");
			    new (self) dicom::PersonName(std::move(value));
		    },
		    nb::kw_only(),
		    nb::arg("alphabetic") = nb::none(),
		    nb::arg("ideographic") = nb::none(),
		    nb::arg("phonetic") = nb::none())
		.def_prop_rw("alphabetic",
		    [](const dicom::PersonName& value) -> nb::object {
			    if (!value.alphabetic) {
				    return nb::none();
			    }
			    return nb::cast(*value.alphabetic);
		    },
		    [](dicom::PersonName& value, nb::handle group) {
			    value.alphabetic = parse_person_name_group_argument(group, "alphabetic");
		    })
		.def_prop_rw("ideographic",
		    [](const dicom::PersonName& value) -> nb::object {
			    if (!value.ideographic) {
				    return nb::none();
			    }
			    return nb::cast(*value.ideographic);
		    },
		    [](dicom::PersonName& value, nb::handle group) {
			    value.ideographic = parse_person_name_group_argument(group, "ideographic");
		    })
		.def_prop_rw("phonetic",
		    [](const dicom::PersonName& value) -> nb::object {
			    if (!value.phonetic) {
				    return nb::none();
			    }
			    return nb::cast(*value.phonetic);
		    },
		    [](dicom::PersonName& value, nb::handle group) {
			    value.phonetic = parse_person_name_group_argument(group, "phonetic");
		    })
		.def("empty", &dicom::PersonName::empty)
		.def("to_dicom_string", &dicom::PersonName::to_dicom_string)
		.def("__repr__",
		    [](const dicom::PersonName& value) {
			    return "PersonName(" + value.to_dicom_string() + ")";
		    });

	nb::class_<DataElement>(m, "DataElement",
	    "Single DICOM element. Provides tag/VR/length/offset and typed value helpers.\n"
	    "For sequences and pixel data, holds nested Sequence or PixelSequence objects.")
		.def_prop_ro("tag", &DataElement::tag)
		.def_prop_ro("vr", &DataElement::vr)
		.def_prop_ro("length", &DataElement::length)
		.def_prop_ro("offset", &DataElement::offset)
		.def_prop_ro("vm", &DataElement::vm)
		.def_prop_rw("value",
		    [](DataElement& element) -> nb::object {
			    return dataelement_get_value_py(
			        element, nb::cast(&element, nb::rv_policy::reference));
		    },
		    [](DataElement& element, nb::handle value) {
			    dataelement_set_value_or_throw_py(element, value);
		    },
		    "Best-effort typed value property. Reading mirrors get_value(); writing "
		    "accepts the same Python types as set_value(); `None` writes a zero-length value. "
		    "Binary memoryviews keep the owning DataElement alive but are still invalidated if the "
		    "underlying value bytes are replaced.")
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
		.def("set_value",
		    [](DataElement& element, nb::handle value) {
			    return dataelement_set_value_py(element, value);
		    },
		    nb::arg("value"),
		    "Best-effort typed assignment. Accepts the same Python value forms as "
		    "DataSet.set_value() and the `value` property setter, updates this element in place, and returns "
		    "True on success or False when the value cannot be encoded for this VR. "
		    "`None` writes a zero-length value. On failure, the owning DataSet remains valid but this "
		    "element's state is unspecified.")
		.def("from_double", &DataElement::from_double, nb::arg("value"),
		    "Encode and store a floating-point value according to this element VR.")
		.def("from_double_vector",
		    [](DataElement& element, const std::vector<double>& values) {
			    return element.from_double_vector(values);
		    },
		    nb::arg("values"),
		    "Encode and store multiple floating-point values according to this element VR.")
		.def("from_tag", &DataElement::from_tag, nb::arg("value"),
		    "Encode and store a tag value (AT VR only).")
		.def("from_tag_vector",
		    [](DataElement& element, const std::vector<Tag>& values) {
			    return element.from_tag_vector(values);
		    },
		    nb::arg("values"),
		    "Encode and store multiple tag values (AT VR only).")
		.def("from_string_view", &DataElement::from_string_view, nb::arg("value"),
		    "Encode and store a textual value according to this element VR.")
		.def("from_string_views",
		    [](DataElement& element, const std::vector<std::string>& values) {
			    std::vector<std::string_view> views;
			    views.reserve(values.size());
			    for (const auto& value : values) {
				    views.push_back(value);
			    }
			    return element.from_string_views(views);
		    },
		    nb::arg("values"),
		    "Encode and store multiple textual values according to this element VR.")
		.def("from_utf8_view",
		    [](DataElement& element, const std::string& value, nb::handle errors,
		        bool return_replaced) -> nb::object {
			    bool replaced = false;
			    const auto ok = element.from_utf8_view(
			        value, parse_charset_encode_error_policy(errors, "errors"),
			        return_replaced ? &replaced : nullptr);
			    if (return_replaced) {
				    return nb::make_tuple(ok, replaced);
			    }
			    return nb::cast(ok);
		    },
		    nb::arg("value"),
		    nb::kw_only(),
		    nb::arg("errors") = nb::str("strict"),
		    nb::arg("return_replaced") = false,
		    "Encode and store a UTF-8 textual value according to this element VR.\n"
		    "errors: 'strict', 'replace_qmark', or 'replace_unicode_escape'.\n"
		    "When return_replaced=True, returns (ok, replaced).")
		.def("from_utf8_views",
		    [](DataElement& element, const std::vector<std::string>& values, nb::handle errors,
		        bool return_replaced) -> nb::object {
			    std::vector<std::string_view> views;
			    views.reserve(values.size());
			    for (const auto& value : values) {
				    views.push_back(value);
			    }
			    bool replaced = false;
			    const auto ok = element.from_utf8_views(
			        views, parse_charset_encode_error_policy(errors, "errors"),
			        return_replaced ? &replaced : nullptr);
			    if (return_replaced) {
				    return nb::make_tuple(ok, replaced);
			    }
			    return nb::cast(ok);
		    },
		    nb::arg("values"),
		    nb::kw_only(),
		    nb::arg("errors") = nb::str("strict"),
		    nb::arg("return_replaced") = false,
		    "Encode and store multiple UTF-8 textual values according to this element VR.\n"
		    "errors: 'strict', 'replace_qmark', or 'replace_unicode_escape'.\n"
		    "When return_replaced=True, returns (ok, replaced).")
		.def("from_person_name",
		    [](DataElement& element, const dicom::PersonName& value, nb::handle errors,
		        bool return_replaced) -> nb::object {
			    bool replaced = false;
			    const auto ok = element.from_person_name(
			        value, parse_charset_encode_error_policy(errors, "errors"),
			        return_replaced ? &replaced : nullptr);
			    if (return_replaced) {
				    return nb::make_tuple(ok, replaced);
			    }
			    return nb::cast(ok);
		    },
		    nb::arg("value"),
		    nb::kw_only(),
		    nb::arg("errors") = nb::str("strict"),
		    nb::arg("return_replaced") = false,
		    "Serialize structured PN data and encode it into this PN element.\n"
		    "errors: 'strict', 'replace_qmark', or 'replace_unicode_escape'.\n"
		    "When return_replaced=True, returns (ok, replaced).")
		.def("from_person_names",
		    [](DataElement& element, const std::vector<dicom::PersonName>& values, nb::handle errors,
		        bool return_replaced) -> nb::object {
			    bool replaced = false;
			    const auto ok = element.from_person_names(
			        values, parse_charset_encode_error_policy(errors, "errors"),
			        return_replaced ? &replaced : nullptr);
			    if (return_replaced) {
				    return nb::make_tuple(ok, replaced);
			    }
			    return nb::cast(ok);
		    },
		    nb::arg("values"),
		    nb::kw_only(),
		    nb::arg("errors") = nb::str("strict"),
		    nb::arg("return_replaced") = false,
		    "Serialize multiple structured PN values and encode them into this PN element.\n"
		    "errors: 'strict', 'replace_qmark', or 'replace_unicode_escape'.\n"
		    "When return_replaced=True, returns (ok, replaced).")
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
		.def("to_utf8_string",
	    [](const DataElement& element, nb::handle errors, bool return_replaced) -> nb::object {
	        bool replaced = false;
	        auto value = element.to_utf8_string(
	            parse_charset_decode_error_policy(errors, "errors"),
	            return_replaced ? &replaced : nullptr);
	        if (return_replaced) {
	            if (value) {
	                return nb::make_tuple(nb::cast(*value), replaced);
	            }
	            return nb::make_tuple(nb::none(), replaced);
	        }
	        if (value) {
	            return nb::cast(*value);
	        }
	        return nb::none();
	    },
	    nb::kw_only(),
	    nb::arg("errors") = nb::str("strict"),
	    nb::arg("return_replaced") = false,
	    "Return a charset-decoded owned UTF-8 string when available, else None.\n"
	    "errors: 'strict', 'replace_fffd', or 'replace_hex_escape'.\n"
	    "When return_replaced=True, returns (value_or_none, replaced).")
		.def("to_utf8_strings",
	    [](const DataElement& element, nb::handle errors, bool return_replaced) -> nb::object {
	        bool replaced = false;
	        auto values = element.to_utf8_strings(
	            parse_charset_decode_error_policy(errors, "errors"),
	            return_replaced ? &replaced : nullptr);
	        if (return_replaced) {
	            if (!values) {
	                return nb::make_tuple(nb::none(), replaced);
	            }
	            return nb::make_tuple(nb::cast(*values), replaced);
	        }
	        if (!values) {
	            return nb::none();
	        }
	        return nb::cast(*values);
	    },
	    nb::kw_only(),
	    nb::arg("errors") = nb::str("strict"),
	    nb::arg("return_replaced") = false,
	    "Return a list of charset-decoded owned UTF-8 strings when available, else None.\n"
	    "errors: 'strict', 'replace_fffd', or 'replace_hex_escape'.\n"
	    "When return_replaced=True, returns (values_or_none, replaced).")
		.def("to_person_name",
	    [](const DataElement& element, nb::handle errors, bool return_replaced) -> nb::object {
	        bool replaced = false;
	        auto value = element.to_person_name(
	            parse_charset_decode_error_policy(errors, "errors"),
	            return_replaced ? &replaced : nullptr);
	        if (return_replaced) {
	            if (value) {
	                return nb::make_tuple(nb::cast(*value), replaced);
	            }
	            return nb::make_tuple(nb::none(), replaced);
	        }
	        if (value) {
	            return nb::cast(*value);
	        }
	        return nb::none();
	    },
	    nb::kw_only(),
	    nb::arg("errors") = nb::str("strict"),
	    nb::arg("return_replaced") = false,
	    "Return the first PN value parsed into alphabetic/ideographic/phonetic groups, else None.\n"
	    "errors: 'strict', 'replace_fffd', or 'replace_hex_escape'.\n"
	    "When return_replaced=True, returns (value_or_none, replaced).")
		.def("to_person_names",
	    [](const DataElement& element, nb::handle errors, bool return_replaced) -> nb::object {
	        bool replaced = false;
	        auto values = element.to_person_names(
	            parse_charset_decode_error_policy(errors, "errors"),
	            return_replaced ? &replaced : nullptr);
	        if (return_replaced) {
	            if (!values) {
	                return nb::make_tuple(nb::none(), replaced);
	            }
	            return nb::make_tuple(nb::cast(*values), replaced);
	        }
	        if (!values) {
	            return nb::none();
	        }
	        return nb::cast(*values);
	    },
	    nb::kw_only(),
	    nb::arg("errors") = nb::str("strict"),
	    nb::arg("return_replaced") = false,
	    "Return all PN values parsed into alphabetic/ideographic/phonetic groups, else None.\n"
	    "errors: 'strict', 'replace_fffd', or 'replace_hex_escape'.\n"
	    "When return_replaced=True, returns (values_or_none, replaced).")
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
		.def("to_tag_vector", &dataelement_to_tag_vector_py)
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
	.def("to_int_vector", &dataelement_to_int_vector_py, nb::arg("default") = nb::none())
	.def("to_long_vector", &dataelement_to_long_vector_py, nb::arg("default") = nb::none())
		.def("to_longlong_vector", &dataelement_to_longlong_vector_py, nb::arg("default") = nb::none())
		.def("to_double_vector", &dataelement_to_double_vector_py, nb::arg("default") = nb::none())
		.def("get_value",
		    [](DataElement& element) -> nb::object {
			    return dataelement_get_value_py(
			        element, nb::cast(&element, nb::rv_policy::reference));
		    },
		    "Best-effort typed access: returns Sequence / PixelSequence for SQ/PX, "
		    "int/float/list for numeric-like VRs (including [] for zero-length numeric values), "
		    "UTF-8 strings for charset-aware text, "
		    "PersonName / list[PersonName] for PN when possible, raw strings for other text, "
		    "and falls back to raw bytes when charset decode/parsing fails or to raw bytes "
		    "(memoryview) for binary VRs; returns None for missing elements. Binary "
		    "memoryviews keep the owning DataElement alive but are still invalidated if the "
		    "underlying value bytes are replaced.")
		.def("value_span",
		    [](DataElement& element) {
			    auto span = element.value_span();
			    nb::object owner = nb::cast(&element, nb::rv_policy::reference);
			    return readonly_memoryview_from_span(span.data(), span.size(), owner);
		    },
		    "Return the raw value bytes as a read-only zero-copy memoryview. The owning "
		    "DataElement is kept alive, but the view is still invalidated if the underlying "
		    "value bytes are replaced. Prefer this for large payloads or buffer-protocol "
		    "consumers such as NumPy.")
		.def("value_bytes",
		    [](const DataElement& element) {
			    auto span = element.value_span();
			    return nb::bytes(reinterpret_cast<const char*>(span.data()), span.size());
		    },
		    "Return the raw value bytes as an owned Python bytes object. This always copies "
		    "the payload. Prefer this when the next API explicitly wants bytes, when you need "
		    "an independent immutable copy, or when payloads are small enough that copy cost is negligible.")
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

	nb::class_<PyDataSetWalkPath>(m, "DataSetWalkPath",
	    "Borrowed ancestors-only walk path view.\n"
	    "\n"
	    "Use the view only within the current iteration step. Persist\n"
	    "``path.to_string()`` if you need to keep the path after the walker\n"
	    "advances.")
		.def("__len__", [](const PyDataSetWalkPath& self) { return self.ref().size(); })
		.def("__bool__", [](const PyDataSetWalkPath& self) { return !self.ref().empty(); })
		.def("contains_sequence",
		    [](const PyDataSetWalkPath& self, nb::handle key) {
			    return self.ref().contains_sequence(dataset_assignment_key_to_tag(key));
		    },
		    nb::arg("tag"),
		    "Return True when the ancestors path contains the given sequence tag.")
		.def("to_string", [](const PyDataSetWalkPath& self) { return self.ref().to_string(); },
		    "Return ancestors-only packed-hex dotted path text, e.g. '00081110.0'.")
		.def("__str__", [](const PyDataSetWalkPath& self) { return self.ref().to_string(); })
		.def("__repr__",
		    [](const PyDataSetWalkPath& self) {
			    return "DataSetWalkPath('" + self.ref().to_string() + "')";
		    });

	nb::class_<PyDataSetWalkEntry>(m, "DataSetWalkEntry")
		.def_prop_ro("path",
		    [](PyDataSetWalkEntry& self) -> nb::object { return self.path_object(); })
		.def_prop_ro("element",
		    [](PyDataSetWalkEntry& self) -> nb::object { return self.element_object(); })
		.def("skip_sequence", &PyDataSetWalkEntry::skip_sequence,
		    "When this entry is the current SQ walk step, prune its nested subtree.")
		.def("skip_current_dataset", &PyDataSetWalkEntry::skip_current_dataset,
		    "When this entry is the current walk step, prune the rest of the current dataset.")
		.def("__len__", [](const PyDataSetWalkEntry&) { return 2; })
		.def("__getitem__",
		    [](PyDataSetWalkEntry& self, std::size_t index) -> nb::object {
			    if (index == 0) {
				    return self.path_object();
			    }
			    if (index == 1) {
				    return self.element_object();
			    }
			    throw nb::index_error("DataSetWalkEntry index out of range");
		    })
		.def("__iter__",
		    [](PyDataSetWalkEntry& self) { return PyDataSetWalkEntryIterator(self); })
		.def("__repr__",
		    [](PyDataSetWalkEntry& self) {
			    auto path =
			        nb::cast<std::string>(self.path_object().attr("to_string")());
			    auto tag = nb::cast<std::string>(
			        self.element_object().attr("tag").attr("__str__")());
			    return "DataSetWalkEntry(path='" + path + "', tag=" + tag + ")";
		    });

	nb::class_<PyDataSetWalkEntryIterator>(m, "DataSetWalkEntryIterator")
		.def("__iter__",
		    [](PyDataSetWalkEntryIterator& self) -> PyDataSetWalkEntryIterator& { return self; },
		    nb::rv_policy::reference_internal)
		.def("__next__", &PyDataSetWalkEntryIterator::next);

	nb::class_<PyDataSetWalkIterator>(m, "DataSetWalkIterator")
		.def("__iter__",
		    [](PyDataSetWalkIterator& self) -> PyDataSetWalkIterator& { return self; },
		    nb::rv_policy::reference_internal)
		.def("__next__", &PyDataSetWalkIterator::next)
		.def("skip_sequence", &PyDataSetWalkIterator::skip_sequence,
		    "When the last yielded entry is an SQ element, prune its nested subtree.")
		.def("skip_current_dataset", &PyDataSetWalkIterator::skip_current_dataset,
		    "Prune the rest of the current dataset before the next iteration step.");

	nb::enum_<dicom::JsonBulkTargetKind>(m, "JsonBulkTargetKind",
	    "Destination kind for unresolved BulkDataURI entries returned by read_json().")
		.value("element", dicom::JsonBulkTargetKind::element)
		.value("pixel_frame", dicom::JsonBulkTargetKind::pixel_frame);

	nb::class_<dicom::JsonBulkRef>(m, "JsonBulkRef",
	    "Reference to unresolved DICOM JSON bulk content returned by read_json().")
		.def(nb::init<>())
		.def_rw("kind", &dicom::JsonBulkRef::kind,
		    "Destination kind: whole element or encapsulated PixelData frame.")
		.def_rw("path", &dicom::JsonBulkRef::path,
		    "Tag-path destination such as '7FE00010' or '0040A730.0.0040A123'.")
		.def_rw("frame_index", &dicom::JsonBulkRef::frame_index,
		    "Zero-based frame index used only when kind == JsonBulkTargetKind.pixel_frame.")
	.def_rw("uri", &dicom::JsonBulkRef::uri,
		    "BulkDataURI that still needs to be downloaded or resolved.")
		.def_rw("media_type", &dicom::JsonBulkRef::media_type,
		    "DICOMweb media type for the bulk payload, or '' when unknown.")
		.def_rw("transfer_syntax_uid", &dicom::JsonBulkRef::transfer_syntax_uid,
		    "Transfer Syntax UID string for pixel payloads, or '' when unknown/not applicable.")
		.def_rw("vr", &dicom::JsonBulkRef::vr,
		    "Target VR parsed from the JSON attribute.")
		.def("__repr__",
		    [](const dicom::JsonBulkRef& self) {
			    std::ostringstream oss;
			    oss << "JsonBulkRef(kind="
			        << (self.kind == dicom::JsonBulkTargetKind::pixel_frame ? "pixel_frame"
			                                                                 : "element")
			        << ", path='" << self.path << "', frame_index=" << self.frame_index
			        << ", uri='" << self.uri << "', media_type='" << self.media_type
			        << "', transfer_syntax_uid='" << self.transfer_syntax_uid
			        << "', vr='" << vr_to_string_view(self.vr) << "')";
			    return oss.str();
		    });

	nb::class_<DataSet>(m, "DataSet",
	    "In-memory DICOM dataset. Created via read_file/read_bytes or directly.\n"
	    "\n"
	    "Features\n"
	    "--------\n"
	    "- Iterable over DataElements in tag order\n"
	    "- Indexing by Tag, packed int, or tag-path string\n"
	    "- Attribute access by keyword (e.g., ds.PatientName)\n"
	    "- Missing lookups return a falsey DataElement (VR::None)",
	    nb::type_slots(dataset_type_slots))
		.def(nb::init<>())
	.def_prop_ro("path", &DataSet::path, "Identifier of the attached stream (file path, provided name, or '<memory>')")
		.def("size", &DataSet::size,
		    "Number of active DataElements currently available in this DataSet")
		.def("add_dataelement",
		    [](DataSet& self, nb::handle key, std::optional<VR> vr) -> DataElement& {
		        if (auto text = dataset_assignment_key_to_text(key)) {
			        return self.add_dataelement(*text, vr.value_or(VR::None));
		        }
		        const Tag tag = dataset_assignment_key_to_tag(key);
		        return self.add_dataelement(tag, vr.value_or(VR::None));
		    },
		    nb::arg("tag"), nb::arg("vr") = nb::none(),
		    nb::rv_policy::reference_internal,
		    "Add or replace a DataElement and return a reference to it. "
		    "On partially loaded file-backed datasets, unread future tags raise instead of "
		    "implicitly continuing the load.")
		.def("ensure_dataelement",
		    [](DataSet& self, nb::handle key, std::optional<VR> vr) -> DataElement& {
		        if (auto text = dataset_assignment_key_to_text(key)) {
			        return self.ensure_dataelement(*text, vr.value_or(VR::None));
		        }
		        const Tag tag = dataset_assignment_key_to_tag(key);
		        return self.ensure_dataelement(tag, vr.value_or(VR::None));
		    },
		    nb::arg("tag"), nb::arg("vr") = nb::none(),
		    nb::rv_policy::reference_internal,
		    "Return the existing DataElement for a Tag, packed int, or keyword/tag-path string, "
		    "or add a new zero-length element when missing. When `vr` is omitted/None, an "
		    "existing element is preserved as-is. When `vr` is explicit and differs from the "
		    "existing element VR, the existing element is reset in place so the requested VR "
		    "is guaranteed. "
		    "On partially loaded file-backed datasets, unread future tags raise instead of "
		    "mutating past the current load frontier.")
		.def("ensure_loaded",
		    [](DataSet& self, nb::handle key) {
			    self.ensure_loaded(dataset_assignment_key_to_tag(key));
		    },
		    nb::arg("tag"),
		    "Advance partial loading through the requested Tag, packed int, or keyword "
		    "string frontier. Nested tag-path strings are not supported.")
		.def("remove_dataelement",
		    [](DataSet& self, nb::handle key) {
		        self.remove_dataelement(dataset_assignment_key_to_tag(key));
		    },
		    nb::arg("tag"),
		    "Remove a DataElement by Tag, packed int, or keyword string if it exists")
		.def("dump_elements", &DataSet::dump_elements,
		    "Print internal element storage for debugging")
		.def("dump", &DataSet::dump,
		    nb::arg("max_print_chars") = static_cast<std::size_t>(80),
		    nb::arg("include_offset") = true,
		    "Return a human-readable DataSet dump as text")
		.def("write_json",
		    [](DataSet& self, bool include_group_0002, nb::handle bulk_data,
		        std::size_t bulk_data_threshold, const std::string& bulk_data_uri_template,
		        const std::string& pixel_data_uri_template, nb::handle charset_errors) {
			    return json_write_result_to_python(
			        self.write_json(make_json_write_options(
			            include_group_0002, bulk_data, bulk_data_threshold,
			            bulk_data_uri_template, pixel_data_uri_template, charset_errors)),
			        nb::cast(&self, nb::rv_policy::reference));
		    },
		    nb::kw_only(),
		    nb::arg("include_group_0002") = false,
		    nb::arg("bulk_data") = nb::str("inline"),
		    nb::arg("bulk_data_threshold") = static_cast<std::size_t>(1024),
		    nb::arg("bulk_data_uri_template") = "",
		    nb::arg("pixel_data_uri_template") = "",
		    nb::arg("charset_errors") = nb::str("strict"),
		    "Serialize this DataSet using the DICOM JSON Model.\n"
		    "\n"
		    "Returns\n"
		    "-------\n"
		    "(json_text, bulk_parts) : tuple[str, list[tuple[str, memoryview, str, str]]]\n"
		    "    `json_text` is the DICOM JSON payload. `bulk_parts` contains\n"
		    "    `(uri, memoryview, media_type, transfer_syntax_uid)` tuples for\n"
		    "    bulk values emitted via `BulkDataURI`.\n"
		    "    Encapsulated multi-frame PixelData is returned one frame per bulk\n"
		    "    part while the JSON keeps a single base `BulkDataURI`. Native\n"
		    "    multi-frame PixelData remains one aggregate bulk part.\n"
		    "    The returned memoryviews borrow the underlying DataSet/stream storage,\n"
		    "    so keep the owning DataSet alive while using them.\n"
		    "\n"
		    "Parameters\n"
		    "----------\n"
		    "include_group_0002 : bool, optional\n"
		    "    Include file meta group 0002 when present. Defaults to False.\n"
		    "bulk_data : {'inline', 'uri', 'omit'}, optional\n"
		    "    Bulk value policy. Defaults to 'inline'.\n"
		    "bulk_data_threshold : int, optional\n"
		    "    Minimum value-field size in bytes before `bulk_data='uri'` emits\n"
		    "    `BulkDataURI`. Defaults to 1024.\n"
		    "bulk_data_uri_template : str, optional\n"
		    "    URI template used when `bulk_data='uri'`. Supports placeholders\n"
		    "    `{study}`, `{series}`, `{instance}`, and `{tag}`.\n"
		    "    `{tag}` expands to the 8-digit tag at top level and to a dotted\n"
		    "    tag path for nested sequence items, e.g. `22002200.0.12340012`.\n"
		    "    Example:\n"
		    "    `/dicomweb/studies/{study}/series/{series}/instances/{instance}/bulk/{tag}`\n"
		    "pixel_data_uri_template : str, optional\n"
		    "    Optional override for PixelData (7FE0,0010), useful for frame routes.\n"
		    "    Example:\n"
		    "    `/dicomweb/studies/{study}/series/{series}/instances/{instance}/frames`\n"
		    "    Keep `bulk_data_uri_template` distinct for other bulk elements,\n"
		    "    typically with `{tag}` in the path.\n"
		    "charset_errors : {'strict', 'replace_fffd', 'replace_hex_escape'}, optional\n"
		    "    Charset decode policy for text VRs. Defaults to 'strict'.")
		.def("get_dataelement",
		    [](DataSet& self, nb::object key) -> DataElement& {
			    return dataset_lookup_dataelement_py(
			        self, key, "DataSet lookup keys must be Tag, int (0xGGGEEEE), or str");
		    },
		    nb::arg("key"),
		    nb::rv_policy::reference_internal,
		    "Return the DataElement for a Tag, packed int, or tag-path string.\n"
		    "Supported examples:\n"
		    "  - Hex tag with/without parens: '00100010', '(0010,0010)'\n"
		    "  - Keyword: 'PatientName'\n"
		    "  - Private creator: 'gggg,xxee,CREATOR' (odd group, xx block placeholder ok)\n"
		    "    e.g., '0009,xx1e,GEMS_GENIE_1'\n"
		    "  - Nested sequences: '00082112.0.00081190' or\n"
		    "    'RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose'\n"
		    "Missing lookups return a falsey DataElement (VR::None); malformed keys raise.")
		.def("get_value",
		    [](DataSet& self, nb::object key, nb::object default_value) -> nb::object {
			    DataElement& el = dataset_lookup_dataelement_py(
			        self, key, "DataSet lookup keys must be Tag, int (0xGGGEEEE), or str");
			    if (el.is_missing()) {
				    return default_value;
			    }
			    return dataelement_get_value_py(el, nb::cast(&self, nb::rv_policy::reference));
		    },
		    nb::arg("key"), nb::arg("default") = nb::none(),
		    "Best-effort typed lookup by Tag, packed int, or tag-path string. "
		    "Returns `default` only when the element is missing; zero-length present "
		    "elements still return typed empty values such as [], '', or an empty container. "
		    "This API does not implicitly continue partial loading.")
		.def("set_value",
		    [](DataSet& self, nb::object key, nb::handle value) {
			    return dataset_try_set_value_py(self, key, value);
		    },
		    nb::arg("key"), nb::arg("value"),
		    "Best-effort typed assignment by Tag, packed int, or keyword/tag-path string. "
		    "Returns True on success, False when the value cannot be encoded for "
		    "the target VR, and treats `None` as a zero-length present value. "
		    "On partially loaded file-backed datasets, unread future tags raise instead of "
		    "implicitly continuing the load. On assignment failure, the "
		    "DataSet remains valid but the destination element state is unspecified.")
		.def("set_value",
		    [](DataSet& self, nb::object key, VR vr, nb::handle value) {
			    return dataset_try_set_value_with_vr_py(self, key, vr, value);
		    },
		    nb::arg("key"), nb::arg("vr"), nb::arg("value"),
		    "Overload: set_value(key, vr, value). Uses the explicit VR when creating "
		    "a missing element and enforces that VR on existing elements before assignment. "
		    "Returns False when the value cannot be encoded for the resolved VR. `None` writes "
		    "a zero-length value for that VR. On partially loaded file-backed datasets, unread "
		    "future tags raise instead of implicitly continuing the load. On assignment failure, "
		    "the DataSet remains valid but the destination element state is unspecified.")
		.def("__getitem__",
		    [](DataSet& self, nb::object key) -> DataElement& {
			    return dataset_lookup_dataelement_py(
			        self, key, "DataSet indices must be Tag, int (0xGGGEEEE), or str");
		    },
		    nb::arg("key"),
		    nb::rv_policy::reference_internal,
		    "Index syntax: ds[tag|packed_int|tag_str] -> DataElement. Missing lookups "
		    "return a falsey DataElement sentinel (VR::None).")
		.def("__contains__",
		    [](DataSet& self, nb::object key) {
			    return dataset_contains_py(
			        self, key, "DataSet membership keys must be Tag, int (0xGGGEEEE), or str");
		    },
		    nb::arg("key"),
		    "Presence probe: key in ds -> bool. Accepts Tag, packed int, or str; "
		    "missing and malformed string keys return False.")
		.def("__getattr__",
		    [](DataSet& self, const std::string& name) -> nb::object {
			    // Allow keyword-style attribute access: ds.PatientName -> get_value("PatientName")
			    if (!name.empty() && name.size() >= 2 && name[0] != '_') {
				    const auto [tag, _vr] = dicom::lookup::keyword_to_tag_vr(name);
				    if (static_cast<bool>(tag)) {
					    DataElement& el = self.get_dataelement(tag);
					    return dataelement_get_value_py(el, nb::cast(&self, nb::rv_policy::reference));
				    }
			    }
			    throw nb::attribute_error(("DataSet has no attribute '" + name + "'").c_str());
		    },
		    nb::arg("name"),
		    "Attribute sugar: ds.PatientName -> ds.get_value('PatientName'). "
		    "Valid DICOM keywords return None when the element is missing; unknown keywords "
		    "raise AttributeError.")
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
		    nb::keep_alive<0, 1>(), "Iterate over DataElements in tag order")
		.def("walk",
		    [](DataSet& self) {
			    return PyDataSetWalkIterator(self, nb::cast(&self, nb::rv_policy::reference));
		    },
		    nb::keep_alive<0, 1>(),
		    "Depth-first preorder walk including SQ elements and nested sequence items.\n"
		    "Each iteration yields DataSetWalkEntry(path, element). Call entry.skip_sequence()\n"
		    "or walker.skip_sequence() after receiving an SQ entry to prune its nested\n"
		    "subtree. Use entry.skip_current_dataset() or walker.skip_current_dataset()\n"
		    "to prune the rest of the current dataset.\n"
		    "\n"
		    "The returned ``entry.path`` is a borrowed view. Use it only within the\n"
		    "current iteration step, and persist ``entry.path.to_string()`` if you\n"
		    "need to keep it after the walker advances.");

	nb::enum_<dicom::pixel::Photometric>(m, "Photometric",
	    "DICOM Photometric Interpretation values used by DecodeInfo when representable.")
		.value("monochrome1", dicom::pixel::Photometric::monochrome1)
		.value("monochrome2", dicom::pixel::Photometric::monochrome2)
		.value("palette_color", dicom::pixel::Photometric::palette_color)
		.value("rgb", dicom::pixel::Photometric::rgb)
		.value("ybr_full", dicom::pixel::Photometric::ybr_full)
		.value("ybr_full_422", dicom::pixel::Photometric::ybr_full_422)
		.value("ybr_rct", dicom::pixel::Photometric::ybr_rct)
		.value("ybr_ict", dicom::pixel::Photometric::ybr_ict)
		.value("ybr_partial_420", dicom::pixel::Photometric::ybr_partial_420)
		.value("xyb", dicom::pixel::Photometric::xyb)
		.value("hsv", dicom::pixel::Photometric::hsv)
		.value("argb", dicom::pixel::Photometric::argb)
		.value("cmyk", dicom::pixel::Photometric::cmyk)
		.value("ybr_partial_422", dicom::pixel::Photometric::ybr_partial_422);

	nb::enum_<dicom::pixel::EncodedLossyState>(m, "EncodedLossyState",
	    "Whether the encoded source frame was lossless, lossy, or near-lossless.")
		.value("unknown", dicom::pixel::EncodedLossyState::unknown)
		.value("lossless", dicom::pixel::EncodedLossyState::lossless)
		.value("lossy", dicom::pixel::EncodedLossyState::lossy)
		.value("near_lossless", dicom::pixel::EncodedLossyState::near_lossless);

	nb::enum_<dicom::pixel::Planar>(m, "Planar",
	    "Pixel sample layout for multi-sample decode output.")
		.value("interleaved", dicom::pixel::Planar::interleaved)
		.value("planar", dicom::pixel::Planar::planar);

	nb::class_<dicom::pixel::DecodeOptions>(m, "DecodeOptions",
	    "Decode option set used to build a reusable DecodePlan.")
		.def("__init__",
		    [](dicom::pixel::DecodeOptions* self, std::uint16_t alignment,
		        std::size_t row_stride, std::size_t frame_stride,
		        dicom::pixel::Planar planar_out, bool decode_mct,
		        int worker_threads, int codec_threads) {
			    new (self) dicom::pixel::DecodeOptions{};
			    self->alignment = alignment;
			    self->row_stride = row_stride;
			    self->frame_stride = frame_stride;
			    self->planar_out = planar_out;
			    self->decode_mct = decode_mct;
			    self->worker_threads = worker_threads;
			    self->codec_threads = codec_threads;
		    },
		    nb::kw_only(),
		    nb::arg("alignment") = static_cast<std::uint16_t>(1),
		    nb::arg("row_stride") = static_cast<std::size_t>(0),
		    nb::arg("frame_stride") = static_cast<std::size_t>(0),
		    nb::arg("planar_out") = dicom::pixel::Planar::interleaved,
		    nb::arg("decode_mct") = true,
		    nb::arg("worker_threads") = -1,
		    nb::arg("codec_threads") = -1)
		.def_rw("alignment", &dicom::pixel::DecodeOptions::alignment,
		    "Requested output row/frame alignment in bytes. Ignored when an explicit stride is set.")
		.def_rw("row_stride", &dicom::pixel::DecodeOptions::row_stride,
		    "Explicit output row stride in bytes. Zero means auto-compute.")
		.def_rw("frame_stride", &dicom::pixel::DecodeOptions::frame_stride,
		    "Explicit output frame stride in bytes. Zero means auto-compute.")
		.def_rw("planar_out", &dicom::pixel::DecodeOptions::planar_out,
		    "Requested output sample layout.")
		.def_rw("decode_mct", &dicom::pixel::DecodeOptions::decode_mct,
		    "Apply codestream-level inverse MCT/color transform when supported.")
		.def_rw("worker_threads", &dicom::pixel::DecodeOptions::worker_threads,
		    "DicomSDL-managed outer worker count used by batch decode paths.")
		.def_rw("codec_threads", &dicom::pixel::DecodeOptions::codec_threads,
		    "Codec/backend internal thread-count hint.")
		.def("__repr__",
		    [](const dicom::pixel::DecodeOptions& self) {
			    std::ostringstream oss;
			    oss << "DecodeOptions(alignment=" << self.alignment
			        << ", row_stride=" << self.row_stride
			        << ", frame_stride=" << self.frame_stride
			        << ", planar_out="
			        << (self.planar_out == dicom::pixel::Planar::planar ? "Planar.planar"
			                                                            : "Planar.interleaved")
			        << ", decode_mct=" << (self.decode_mct ? "True" : "False")
			        << ", worker_threads=" << self.worker_threads
			        << ", codec_threads=" << self.codec_threads << ")";
			    return oss.str();
		    });

	nb::class_<dicom::pixel::DecodePlan>(m, "DecodePlan",
	    "Reusable decode-plan snapshot for repeated frame decode.")
		.def_prop_ro("options",
		    [](const dicom::pixel::DecodePlan& self) { return self.options; },
		    "Decode options captured in this plan.")
		.def_prop_ro("has_pixel_data",
		    [](const dicom::pixel::DecodePlan& self) {
			    return !self.output_layout.empty();
		    })
		.def_prop_ro("rows",
		    [](const dicom::pixel::DecodePlan& self) {
			    return static_cast<int>(self.output_layout.rows);
		    })
		.def_prop_ro("cols",
		    [](const dicom::pixel::DecodePlan& self) {
			    return static_cast<int>(self.output_layout.cols);
		    })
		.def_prop_ro("frames",
		    [](const dicom::pixel::DecodePlan& self) {
			    return static_cast<int>(self.output_layout.frames);
		    })
		.def_prop_ro("samples_per_pixel",
		    [](const dicom::pixel::DecodePlan& self) {
			    return static_cast<int>(self.output_layout.samples_per_pixel);
		    })
		.def_prop_ro("bits_stored",
		    [](const dicom::pixel::DecodePlan& self) {
			    return static_cast<int>(self.output_layout.bits_stored);
		    })
		.def_prop_ro("row_stride",
		    [](const dicom::pixel::DecodePlan& self) { return self.output_layout.row_stride; },
		    "Decoded output row stride in bytes.")
		.def_prop_ro("frame_stride",
		    [](const dicom::pixel::DecodePlan& self) { return self.output_layout.frame_stride; },
		    "Decoded output frame stride in bytes.")
		.def_prop_ro("dtype",
		    [](const dicom::pixel::DecodePlan& self) { return numpy_dtype_object(self); },
		    "NumPy dtype implied by this plan.")
		.def_prop_ro("bytes_per_sample",
		    [](const dicom::pixel::DecodePlan& self) {
			    return decoded_array_spec(self).bytes_per_sample;
		    },
		    "Decoded output bytes per sample.")
		.def("shape",
		    [](const dicom::pixel::DecodePlan& self, long frame) {
			    return shape_tuple_from_layout(build_decode_layout_from_plan(self, frame));
		    },
		    nb::arg("frame") = -1,
		    "Return the NumPy shape implied by this plan for the selected frame.")
		.def("required_bytes",
		    [](const dicom::pixel::DecodePlan& self, long frame) {
			    return build_decode_layout_from_plan(self, frame).required_bytes;
		    },
		    nb::arg("frame") = -1,
		    "Return the required output buffer size in bytes for the selected frame.")
		.def("__repr__",
		    [](const dicom::pixel::DecodePlan& self) {
			    std::ostringstream oss;
			    oss << "DecodePlan(rows=" << self.output_layout.rows
			        << ", cols=" << self.output_layout.cols
			        << ", frames=" << self.output_layout.frames
			        << ", samples_per_pixel=" << self.output_layout.samples_per_pixel
			        << ", frame_stride=" << self.output_layout.frame_stride << ")";
			    return oss.str();
		    });

	nb::class_<dicom::pixel::DecodeInfo>(m, "DecodeInfo",
	    "Metadata reported by a successful decode operation.")
		.def(nb::init<>())
		.def_prop_ro("photometric",
		    [](const dicom::pixel::DecodeInfo& self) -> nb::object {
			    if (!self.photometric) {
				    return nb::none();
			    }
			    return nb::cast(*self.photometric);
		    },
		    "Decoded output photometric when representable as a DICOM Photometric value, else None."
		    " This reflects the backend's actual decoded buffer domain and may differ"
		    " from stored metadata such as YBR_RCT/YBR_ICT.")
		.def_prop_ro("encoded_lossy_state",
		    [](const dicom::pixel::DecodeInfo& self) {
			    return self.encoded_lossy_state;
		    },
		    "Lossiness classification of the encoded source frame.")
		.def_prop_ro("dtype",
		    [](const dicom::pixel::DecodeInfo& self) -> nb::object {
			    if (!self.data_type) {
				    return nb::none();
			    }
			    return numpy_dtype_object(*self.data_type);
		    },
		    "NumPy dtype implied by the decoded output, or None when unknown.")
		.def_prop_ro("planar",
		    [](const dicom::pixel::DecodeInfo& self) -> nb::object {
			    if (!self.planar) {
				    return nb::none();
			    }
			    return nb::cast(*self.planar);
		    },
		    "Decoded sample layout, or None when unknown.")
		.def_prop_ro("bits_per_sample",
		    [](const dicom::pixel::DecodeInfo& self) {
			    return static_cast<int>(self.bits_per_sample);
		    },
		    "Decoded bits per component, or 0 when unknown.")
		.def("__repr__",
		    [](const dicom::pixel::DecodeInfo& self) {
			    std::ostringstream oss;
			    oss << "DecodeInfo(photometric=";
			    if (self.photometric) {
				    oss << "Photometric." << photometric_name(*self.photometric);
			    } else {
				    oss << "None";
			    }
			    oss << ", encoded_lossy_state=EncodedLossyState."
			        << encoded_lossy_state_name(self.encoded_lossy_state)
			        << ", dtype=";
			    if (self.data_type) {
				    oss << data_type_name(*self.data_type);
			    } else {
				    oss << "None";
			    }
			    oss << ", planar=";
			    if (self.planar) {
				    oss << "Planar."
				        << (*self.planar == dicom::pixel::Planar::planar ? "planar"
				                                                          : "interleaved");
			    } else {
				    oss << "None";
			    }
			    oss << ", bits_per_sample=" << self.bits_per_sample << ")";
			    return oss.str();
		    });

	nb::class_<dicom::pixel::RescaleTransform>(m, "RescaleTransform",
	    "Linear rescale transform applied after decode.")
		.def(nb::init<>())
		.def_rw("slope", &dicom::pixel::RescaleTransform::slope,
		    "Multiplicative rescale slope.")
		.def_rw("intercept", &dicom::pixel::RescaleTransform::intercept,
		    "Additive rescale intercept.")
		.def("__repr__",
		    [](const dicom::pixel::RescaleTransform& self) {
			    std::ostringstream oss;
			    oss << "RescaleTransform(slope=" << self.slope
			        << ", intercept=" << self.intercept << ")";
			    return oss.str();
		    });

	nb::class_<dicom::pixel::ModalityLut>(m, "ModalityLut",
	    "Modality LUT values applied after decode.")
		.def(nb::init<>())
		.def_rw("first_mapped", &dicom::pixel::ModalityLut::first_mapped,
		    "First stored-value input mapped by this LUT.")
		.def_rw("values", &dicom::pixel::ModalityLut::values,
		    "LUT output values as float32-compatible samples.")
		.def("__repr__",
		    [](const dicom::pixel::ModalityLut& self) {
			    std::ostringstream oss;
			    oss << "ModalityLut(first_mapped=" << self.first_mapped
			        << ", values=" << self.values.size() << " entries)";
			    return oss.str();
		    });

	nb::class_<dicom::pixel::PaletteLut>(m, "PaletteLut",
	    "Palette color LUT values applied after decode.")
		.def(nb::init<>())
		.def_rw("first_mapped", &dicom::pixel::PaletteLut::first_mapped,
		    "First stored-value input mapped by this LUT.")
		.def_rw("bits_per_entry", &dicom::pixel::PaletteLut::bits_per_entry,
		    "Bit depth of each LUT entry. Zero lets the implementation infer 8/16-bit output.")
		.def_rw("red_values", &dicom::pixel::PaletteLut::red_values,
		    "Red channel LUT values.")
		.def_rw("green_values", &dicom::pixel::PaletteLut::green_values,
		    "Green channel LUT values.")
		.def_rw("blue_values", &dicom::pixel::PaletteLut::blue_values,
		    "Blue channel LUT values.")
		.def_rw("alpha_values", &dicom::pixel::PaletteLut::alpha_values,
		    "Optional alpha channel LUT values.")
		.def("__repr__",
		    [](const dicom::pixel::PaletteLut& self) {
			    std::ostringstream oss;
			    oss << "PaletteLut(first_mapped=" << self.first_mapped
			        << ", bits_per_entry=" << self.bits_per_entry
			        << ", values=" << self.red_values.size()
			        << ", alpha=" << self.alpha_values.size() << ")";
			    return oss.str();
		    });

	nb::enum_<dicom::pixel::PixelPresentation>(m, "PixelPresentation",
	    "Root-level pixel presentation model used by display-pipeline palette metadata.")
		.value("monochrome", dicom::pixel::PixelPresentation::monochrome)
		.value("color", dicom::pixel::PixelPresentation::color)
		.value("mixed", dicom::pixel::PixelPresentation::mixed)
		.value("true_color", dicom::pixel::PixelPresentation::true_color)
		.value("color_range", dicom::pixel::PixelPresentation::color_range)
		.value("color_ref", dicom::pixel::PixelPresentation::color_ref);

	nb::class_<dicom::pixel::SupplementalPaletteInfo>(m, "SupplementalPaletteInfo",
	    "Root-level Supplemental Palette metadata kept separate from classic PALETTE COLOR LUTs.")
		.def(nb::init<>())
		.def_rw("pixel_presentation",
		    &dicom::pixel::SupplementalPaletteInfo::pixel_presentation,
		    "Pixel Presentation value associated with this supplemental palette.")
		.def_rw("palette", &dicom::pixel::SupplementalPaletteInfo::palette,
		    "Palette lookup table payload, including optional alpha values.")
		.def_rw("has_stored_value_range",
		    &dicom::pixel::SupplementalPaletteInfo::has_stored_value_range,
		    "True when Stored Value Color Range Sequence is present.")
		.def_rw("minimum_stored_value_mapped",
		    &dicom::pixel::SupplementalPaletteInfo::minimum_stored_value_mapped,
		    "Minimum stored value covered by the supplemental color range.")
		.def_rw("maximum_stored_value_mapped",
		    &dicom::pixel::SupplementalPaletteInfo::maximum_stored_value_mapped,
		    "Maximum stored value covered by the supplemental color range.")
		.def("__repr__",
		    [](const dicom::pixel::SupplementalPaletteInfo& self) {
			    std::ostringstream oss;
			    oss << "SupplementalPaletteInfo(values="
			        << self.palette.red_values.size()
			        << ", alpha=" << self.palette.alpha_values.size() << ")";
			    return oss.str();
		    });

	nb::class_<dicom::pixel::EnhancedPaletteDataPathAssignmentInfo>(
	    m, "EnhancedPaletteDataPathAssignmentInfo",
	    "One Data Frame Assignment item from the Enhanced Palette display pipeline.")
		.def(nb::init<>())
		.def_rw("data_type", &dicom::pixel::EnhancedPaletteDataPathAssignmentInfo::data_type,
		    "Data Type string from the assignment item.")
		.def_rw("data_path_assignment",
		    &dicom::pixel::EnhancedPaletteDataPathAssignmentInfo::data_path_assignment,
		    "Data Path Assignment string such as PRIMARY_PVALUES or PRIMARY_SINGLE.")
		.def_rw("has_bits_mapped_to_color_lookup_table",
		    &dicom::pixel::EnhancedPaletteDataPathAssignmentInfo::has_bits_mapped_to_color_lookup_table,
		    "True when Bits Mapped to Color Lookup Table is present.")
		.def_rw("bits_mapped_to_color_lookup_table",
		    &dicom::pixel::EnhancedPaletteDataPathAssignmentInfo::bits_mapped_to_color_lookup_table,
		    "Bits Mapped to Color Lookup Table value.");

	nb::class_<dicom::pixel::EnhancedBlendingLutInfo>(m, "EnhancedBlendingLutInfo",
	    "One Enhanced Palette blending LUT specification.")
		.def(nb::init<>())
		.def_rw("transfer_function",
		    &dicom::pixel::EnhancedBlendingLutInfo::transfer_function,
		    "Transfer function string for this blending LUT.")
		.def_rw("has_weight_constant",
		    &dicom::pixel::EnhancedBlendingLutInfo::has_weight_constant,
		    "True when Blending Weight Constant is present.")
		.def_rw("weight_constant",
		    &dicom::pixel::EnhancedBlendingLutInfo::weight_constant,
		    "Blending Weight Constant value.")
		.def_rw("bits_per_entry",
		    &dicom::pixel::EnhancedBlendingLutInfo::bits_per_entry,
		    "Bit depth of TABLE-based blending LUT values.")
		.def_rw("values", &dicom::pixel::EnhancedBlendingLutInfo::values,
		    "Expanded TABLE-based blending LUT values.")
		.def("__repr__",
		    [](const dicom::pixel::EnhancedBlendingLutInfo& self) {
			    std::ostringstream oss;
			    oss << "EnhancedBlendingLutInfo(transfer_function="
			        << self.transfer_function
			        << ", values=" << self.values.size() << ")";
			    return oss.str();
		    });

	nb::class_<dicom::pixel::EnhancedPaletteItemInfo>(m, "EnhancedPaletteItemInfo",
	    "One Enhanced Palette Color Lookup Table Sequence item.")
		.def(nb::init<>())
		.def_rw("data_path_id", &dicom::pixel::EnhancedPaletteItemInfo::data_path_id,
		    "PRIMARY or SECONDARY path identifier.")
		.def_rw("rgb_lut_transfer_function",
		    &dicom::pixel::EnhancedPaletteItemInfo::rgb_lut_transfer_function,
		    "RGB LUT Transfer Function string.")
		.def_rw("alpha_lut_transfer_function",
		    &dicom::pixel::EnhancedPaletteItemInfo::alpha_lut_transfer_function,
		    "Alpha LUT Transfer Function string.")
		.def_rw("palette", &dicom::pixel::EnhancedPaletteItemInfo::palette,
		    "Palette lookup table payload, including optional alpha values.");

	nb::class_<dicom::pixel::EnhancedPaletteInfo>(m, "EnhancedPaletteInfo",
	    "Root-level Enhanced Palette metadata kept separate from classic PALETTE COLOR LUTs.")
		.def(nb::init<>())
		.def_rw("pixel_presentation", &dicom::pixel::EnhancedPaletteInfo::pixel_presentation,
		    "Pixel Presentation value associated with this enhanced palette pipeline.")
		.def_rw("data_frame_assignments",
		    &dicom::pixel::EnhancedPaletteInfo::data_frame_assignments,
		    "Data Frame Assignment Sequence items.")
		.def_rw("has_blending_lut_1",
		    &dicom::pixel::EnhancedPaletteInfo::has_blending_lut_1,
		    "True when Blending LUT 1 Sequence is present.")
		.def_rw("blending_lut_1", &dicom::pixel::EnhancedPaletteInfo::blending_lut_1,
		    "Blending LUT 1 metadata.")
		.def_rw("has_blending_lut_2",
		    &dicom::pixel::EnhancedPaletteInfo::has_blending_lut_2,
		    "True when Blending LUT 2 Sequence is present.")
		.def_rw("blending_lut_2", &dicom::pixel::EnhancedPaletteInfo::blending_lut_2,
		    "Blending LUT 2 metadata.")
		.def_rw("palette_items", &dicom::pixel::EnhancedPaletteInfo::palette_items,
		    "Enhanced Palette Color Lookup Table Sequence items.")
		.def_rw("has_icc_profile", &dicom::pixel::EnhancedPaletteInfo::has_icc_profile,
		    "True when ICC Profile is present.")
		.def_rw("color_space", &dicom::pixel::EnhancedPaletteInfo::color_space,
		    "Color Space value when present.")
		.def("__repr__",
		    [](const dicom::pixel::EnhancedPaletteInfo& self) {
			    std::ostringstream oss;
			    oss << "EnhancedPaletteInfo(assignments="
			        << self.data_frame_assignments.size()
			        << ", palette_items=" << self.palette_items.size() << ")";
			    return oss.str();
		    });

	nb::class_<dicom::pixel::VoiLut>(m, "VoiLut",
	    "VOI LUT metadata extracted from VOILUTSequence or constructed manually.")
		.def(nb::init<>())
		.def_rw("first_mapped", &dicom::pixel::VoiLut::first_mapped,
		    "First stored or modality value covered by this VOI LUT.")
		.def_rw("bits_per_entry", &dicom::pixel::VoiLut::bits_per_entry,
		    "Declared VOI LUT output bit depth. 0 lets the implementation infer 8 or 16 bits.")
		.def_rw("values", &dicom::pixel::VoiLut::values,
		    "VOI LUT output values.")
		.def("__repr__",
		    [](const dicom::pixel::VoiLut& self) {
			    std::ostringstream oss;
			    oss << "VoiLut(first_mapped=" << self.first_mapped
			        << ", bits_per_entry=" << self.bits_per_entry
			        << ", values=" << self.values.size() << " entries)";
			    return oss.str();
		    });

	nb::enum_<dicom::pixel::VoiLutFunction>(m, "VoiLutFunction",
	    "VOI LUT function used by window center/width transforms.")
		.value("linear", dicom::pixel::VoiLutFunction::linear)
		.value("linear_exact", dicom::pixel::VoiLutFunction::linear_exact)
		.value("sigmoid", dicom::pixel::VoiLutFunction::sigmoid);

	nb::class_<dicom::pixel::WindowTransform>(m, "WindowTransform",
	    "VOI window center/width transform applied after decode.")
		.def(nb::init<>())
		.def_rw("center", &dicom::pixel::WindowTransform::center,
		    "VOI window center.")
		.def_rw("width", &dicom::pixel::WindowTransform::width,
		    "VOI window width.")
		.def_rw("function", &dicom::pixel::WindowTransform::function,
		    "VOI LUT function used by this window.")
		.def("__repr__",
		    [](const dicom::pixel::WindowTransform& self) {
			    std::ostringstream oss;
			    oss << "WindowTransform(center=" << self.center
			        << ", width=" << self.width
			        << ", function=";
			    switch (self.function) {
			    case dicom::pixel::VoiLutFunction::linear:
				    oss << "linear";
				    break;
			    case dicom::pixel::VoiLutFunction::linear_exact:
				    oss << "linear_exact";
				    break;
			    case dicom::pixel::VoiLutFunction::sigmoid:
				    oss << "sigmoid";
				    break;
			    default:
				    oss << "unknown";
				    break;
			    }
			    oss << ")";
			    return oss.str();
		    });

	nb::class_<EncoderContext>(m, "EncoderContext",
	    "Reusable encoder option context bound to one transfer syntax.")
		.def(nb::init<>())
		.def("__enter__",
		    [](EncoderContext& self) -> EncoderContext& { return self; },
		    nb::rv_policy::reference_internal)
		.def("__exit__",
		    [](EncoderContext&, nb::handle, nb::handle, nb::handle) {
			    return false;
		    },
		    nb::arg("exc_type").none(), nb::arg("exc_value").none(),
		    nb::arg("traceback").none())
		.def("configure",
		    [](EncoderContext& self, const Uid& transfer_syntax, nb::handle options) {
			    configure_encoder_context_with_options(
			        self, transfer_syntax, options);
		    },
		    nb::arg("transfer_syntax"),
		    nb::kw_only(),
		    nb::arg("options") = nb::none(),
		    "Configure this context from transfer syntax and optional codec options.")
		.def("configure",
		    [](EncoderContext& self, const std::string& transfer_syntax_text,
		        nb::handle options) {
			    const auto transfer_syntax =
			        parse_transfer_syntax_text_or_throw(transfer_syntax_text);
			    configure_encoder_context_with_options(
			        self, transfer_syntax, options);
		    },
		    nb::arg("transfer_syntax"),
		    nb::kw_only(),
		    nb::arg("options") = nb::none(),
		    "Configure this context from transfer syntax text and optional codec options.")
		.def_prop_ro("configured", &EncoderContext::configured,
		    "True when this context has been configured.")
		.def_prop_ro("transfer_syntax_uid",
		    [](const EncoderContext& self) -> nb::object {
			    const auto uid = self.transfer_syntax_uid();
			    if (!uid.valid()) {
				    return nb::none();
			    }
			    return nb::cast(uid);
		    },
		    "Configured transfer syntax UID, or None when not configured.");

	nb::class_<DicomFile>(m, "DicomFile",
	    "DICOM file/session object that owns the root DataSet.",
	    nb::type_slots(dicomfile_type_slots),
	    nb::dynamic_attr())
		.def(nb::init<>())
		.def_prop_ro("path", &DicomFile::path,
		    "Identifier of the attached root stream (file path or provided memory name)")
		.def_prop_ro("transfer_syntax_uid",
		    [](const DicomFile& self) -> nb::object {
			    const auto uid = self.transfer_syntax_uid();
			    if (!uid.valid()) {
				    return nb::none();
			    }
			    return nb::cast(uid);
		    },
		    "Current transfer syntax UID as a Uid object, or None if unavailable.")
		.def_prop_ro("window_transform",
		    [](const DicomFile& self) -> nb::object {
			    const auto transform = self.window_transform();
			    if (!transform) {
				    return nb::none();
			    }
			    return nb::cast(*transform);
		    },
		    "Window center/width metadata for frame 0 as a WindowTransform, or None when absent.")
		.def("window_transform_for_frame",
		    [](const DicomFile& self, std::size_t frame_index) -> nb::object {
			    const auto transform = self.window_transform(frame_index);
			    if (!transform) {
				    return nb::none();
			    }
			    return nb::cast(*transform);
		    },
		    nb::arg("frame_index"),
		    "Window center/width metadata for the requested frame, resolved from Per-Frame Functional Groups, Shared Functional Groups, then the root dataset.")
		.def_prop_ro("rescale_transform",
		    [](const DicomFile& self) -> nb::object {
			    const auto transform = self.rescale_transform();
			    if (!transform) {
				    return nb::none();
			    }
			    return nb::cast(*transform);
		    },
		    "Rescale slope/intercept metadata for frame 0 as a RescaleTransform, or None when absent.")
		.def("rescale_transform_for_frame",
		    [](const DicomFile& self, std::size_t frame_index) -> nb::object {
			    const auto transform = self.rescale_transform(frame_index);
			    if (!transform) {
				    return nb::none();
			    }
			    return nb::cast(*transform);
		    },
		    nb::arg("frame_index"),
		    "Rescale slope/intercept metadata for the requested frame, resolved from Per-Frame Functional Groups, Shared Functional Groups, then the root dataset.")
		.def_prop_ro("modality_lut",
		    [](const DicomFile& self) -> nb::object {
			    const auto lut = self.modality_lut();
			    if (!lut) {
				    return nb::none();
			    }
			    return nb::cast(*lut);
		    },
		    "Modality LUT metadata for frame 0 as a ModalityLut, or None when absent.")
		.def("modality_lut_for_frame",
		    [](const DicomFile& self, std::size_t frame_index) -> nb::object {
			    const auto lut = self.modality_lut(frame_index);
			    if (!lut) {
				    return nb::none();
			    }
			    return nb::cast(*lut);
		    },
		    nb::arg("frame_index"),
		    "Modality LUT metadata for the requested frame. Root-level ModalityLUTSequence is shared across frames; frame-specific modality transforms are exposed through rescale_transform_for_frame().")
		.def_prop_ro("pixel_presentation",
		    [](const DicomFile& self) -> nb::object {
			    const auto presentation = self.pixel_presentation();
			    if (!presentation) {
				    return nb::none();
			    }
			    return nb::cast(*presentation);
		    },
		    "Root-level Pixel Presentation value, or None when absent.")
		.def_prop_ro("voi_lut",
		    [](const DicomFile& self) -> nb::object {
			    const auto lut = self.voi_lut();
			    if (!lut) {
				    return nb::none();
			    }
			    return nb::cast(*lut);
		    },
		    "VOI LUT metadata for frame 0 as a VoiLut, or None when absent.")
		.def("voi_lut_for_frame",
		    [](const DicomFile& self, std::size_t frame_index) -> nb::object {
			    const auto lut = self.voi_lut(frame_index);
			    if (!lut) {
				    return nb::none();
			    }
			    return nb::cast(*lut);
		    },
		    nb::arg("frame_index"),
		    "VOI LUT metadata for the requested frame, resolved from Per-Frame Functional Groups, Shared Functional Groups, then the root dataset.")
		.def_prop_ro("palette_lut",
		    [](const DicomFile& self) -> nb::object {
			    const auto lut = self.palette_lut();
			    if (!lut) {
				    return nb::none();
			    }
			    return nb::cast(*lut);
		    },
		    "Palette color LUT metadata as a PaletteLut, or None when absent.")
		.def_prop_ro("supplemental_palette",
		    [](const DicomFile& self) -> nb::object {
			    const auto info = self.supplemental_palette();
			    if (!info) {
				    return nb::none();
			    }
			    return nb::cast(*info);
		    },
		    "Supplemental Palette metadata as a SupplementalPaletteInfo, or None when absent.")
		.def_prop_ro("enhanced_palette",
		    [](const DicomFile& self) -> nb::object {
			    const auto info = self.enhanced_palette();
			    if (!info) {
				    return nb::none();
			    }
			    return nb::cast(*info);
		    },
		    "Enhanced Palette metadata as an EnhancedPaletteInfo, or None when absent.")
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
		.def("add_dataelement",
		    [](DicomFile& self, nb::handle key, std::optional<VR> vr) -> DataElement& {
		        if (auto text = dataset_assignment_key_to_text(key)) {
			        return self.add_dataelement(*text, vr.value_or(VR::None));
		        }
		        const Tag tag = dataset_assignment_key_to_tag(key);
		        return self.add_dataelement(tag, vr.value_or(VR::None));
		    },
		    nb::arg("tag"), nb::arg("vr") = nb::none(),
		    nb::rv_policy::reference_internal,
		    "Add or replace a DataElement in the root DataSet and return a reference to it. "
		    "On partially loaded file-backed datasets, unread future tags raise instead of "
		    "implicitly continuing the load.")
		.def("ensure_dataelement",
		    [](DicomFile& self, nb::handle key, std::optional<VR> vr) -> DataElement& {
		        if (auto text = dataset_assignment_key_to_text(key)) {
			        return self.ensure_dataelement(*text, vr.value_or(VR::None));
		        }
		        const Tag tag = dataset_assignment_key_to_tag(key);
		        return self.ensure_dataelement(tag, vr.value_or(VR::None));
		    },
		    nb::arg("tag"), nb::arg("vr") = nb::none(),
		    nb::rv_policy::reference_internal,
		    "Return the existing root DataSet element for a Tag, packed int, or keyword/tag-path "
		    "string, or add a new zero-length element when missing. When `vr` is omitted/None, "
		    "an existing element is preserved as-is. When `vr` is explicit and differs from the "
		    "existing element VR, the existing element is reset in place so the requested VR "
		    "is guaranteed. On partially loaded file-backed datasets, unread future tags raise "
		    "instead of mutating past the current load frontier.")
		.def("ensure_loaded",
		    [](DicomFile& self, nb::handle key) {
			    self.ensure_loaded(dataset_assignment_key_to_tag(key));
		    },
		    nb::arg("tag"),
		    "Advance partial loading through the requested Tag, packed int, or keyword "
		    "string frontier of the root DataSet. Nested tag-path strings are not supported.")
		.def("remove_dataelement",
		    [](DicomFile& self, nb::handle key) {
		        self.remove_dataelement(dataset_assignment_key_to_tag(key));
		    },
		    nb::arg("tag"),
		    "Remove a root DataSet DataElement by Tag, packed int, or keyword string if it exists")
		.def("get_dataelement",
		    [](DicomFile& self, nb::object key) -> DataElement& {
			    return dataset_lookup_dataelement_py(
			        self.dataset(), key,
			        "DicomFile lookup keys must be Tag, int (0xGGGEEEE), or str");
		    },
		    nb::arg("key"),
		    nb::rv_policy::reference_internal,
		    "Return the root DataSet DataElement for a Tag, packed int, or tag-path string.\n"
		    "Supported examples:\n"
		    "  - Hex tag with/without parens: '00100010', '(0010,0010)'\n"
		    "  - Keyword: 'PatientName'\n"
		    "  - Private creator: 'gggg,xxee,CREATOR' (odd group, xx block placeholder ok)\n"
		    "    e.g., '0009,xx1e,GEMS_GENIE_1'\n"
		    "  - Nested sequences: '00082112.0.00081190' or\n"
		    "    'RadiopharmaceuticalInformationSequence.0.RadionuclideTotalDose'\n"
		    "Missing lookups return a falsey DataElement (VR::None); malformed keys raise.")
		.def("get_value",
		    [](DicomFile& self, nb::object key, nb::object default_value) -> nb::object {
		        DataElement& el = dataset_lookup_dataelement_py(
		            self.dataset(), key, "DicomFile lookup keys must be Tag, int (0xGGGEEEE), or str");
		        if (el.is_missing()) {
			        return default_value;
		        }
		        return dataelement_get_value_py(el, nb::cast(&self, nb::rv_policy::reference));
		    },
		    nb::arg("key"), nb::arg("default") = nb::none(),
		    "Best-effort typed lookup by Tag, packed int, or tag-path string against the root "
		    "DataSet. Returns `default` only when the element is missing; zero-length present "
		    "elements still return typed empty values such as [], '', or an empty container. "
		    "This API does not implicitly continue partial loading.")
		.def("set_value",
		    [](DicomFile& self, nb::object key, nb::handle value) {
		        return dataset_try_set_value_py(self.dataset(), key, value);
		    },
		    nb::arg("key"), nb::arg("value"),
		    "Best-effort typed assignment by Tag, packed int, or keyword/tag-path string "
		    "against the root DataSet. Returns True on success, False when the value cannot "
		    "be encoded for the target VR, and treats `None` as a zero-length present value. "
		    "On partially loaded file-backed datasets, unread future tags raise instead of "
		    "implicitly continuing the load. On assignment failure, the DicomFile/root DataSet "
		    "remains valid but the destination element state is unspecified.")
		.def("set_value",
		    [](DicomFile& self, nb::object key, VR vr, nb::handle value) {
		        return dataset_try_set_value_with_vr_py(self.dataset(), key, vr, value);
		    },
		    nb::arg("key"), nb::arg("vr"), nb::arg("value"),
		    "Overload: set_value(key, vr, value) against the root DataSet. Uses the explicit VR "
		    "when creating a missing element and enforces that VR on existing elements before "
		    "assignment. Returns False when the value cannot be encoded for the resolved VR. "
		    "`None` writes a zero-length value for that VR. On partially loaded file-backed "
		    "datasets, unread future tags raise instead of implicitly continuing the load. "
		    "On assignment failure, the DicomFile/root DataSet remains valid but the destination "
		    "element state is unspecified.")
		.def("create_decode_plan",
		    [](const DicomFile& self) { return self.create_decode_plan(); },
		    "Compute a reusable DecodePlan using default DecodeOptions.")
		.def("create_decode_plan",
		    [](const DicomFile& self, const dicom::pixel::DecodeOptions& options) {
			    return self.create_decode_plan(options);
		    },
		    nb::arg("options"),
		    "Compute a reusable DecodePlan from explicit DecodeOptions.")
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
		.def("set_transfer_syntax",
		    [](DicomFile& self, const Uid& transfer_syntax, nb::handle options) {
			    set_transfer_syntax_with_options(self, transfer_syntax, options);
		    },
		    nb::arg("transfer_syntax"),
		    nb::kw_only(),
		    nb::arg("options") = nb::none(),
		    "Set transfer syntax using a Uid object and update file meta TransferSyntaxUID.\n"
		    "`options` accepts None/'auto', 'none', 'rle', 'j2k', 'htj2k', 'jpegls', 'jpeg', 'jpegxl',\n"
		    "or dict form such as:\n"
		    "{'type': 'j2k', 'target_psnr': 45.0, 'target_bpp': 0.0, 'threads': -1, 'color_transform': True},\n"
		    "{'type': 'htj2k', 'target_psnr': 45.0, 'color_transform': False},\n"
		    "{'type': 'jpegls', 'near_lossless_error': 2},\n"
		    "{'type': 'jpeg', 'quality': 90},\n"
		    "{'type': 'jpegxl', 'distance': 1.5, 'effort': 7, 'threads': -1}.")
		.def("set_transfer_syntax",
		    [](DicomFile& self, const std::string& transfer_syntax_text,
		        nb::handle options) {
			    set_transfer_syntax_with_options(
			        self, parse_transfer_syntax_text_or_throw(transfer_syntax_text),
			        options);
		    },
		    nb::arg("transfer_syntax"),
		    nb::kw_only(),
		    nb::arg("options") = nb::none(),
		    "Set transfer syntax using a UID keyword or dotted UID string.\n"
		    "`options` accepts None/'auto', 'none', 'rle', 'j2k', 'htj2k', 'jpegls', 'jpeg', 'jpegxl',\n"
		    "or dict form such as:\n"
		    "{'type': 'j2k', 'target_psnr': 45.0, 'target_bpp': 0.0, 'threads': -1, 'color_transform': True},\n"
		    "{'type': 'htj2k', 'target_psnr': 45.0, 'color_transform': False},\n"
		    "{'type': 'jpegls', 'near_lossless_error': 2},\n"
		    "{'type': 'jpeg', 'quality': 90},\n"
		    "{'type': 'jpegxl', 'distance': 1.5, 'effort': 7, 'threads': -1}.")
		.def("set_transfer_syntax",
		    [](DicomFile& self, const Uid& transfer_syntax,
		        const EncoderContext& encoder_context) {
			    self.set_transfer_syntax(transfer_syntax, encoder_context);
		    },
		    nb::arg("transfer_syntax"),
		    nb::kw_only(),
		    nb::arg("encoder_context"),
		    "Set transfer syntax using a preconfigured EncoderContext.")
		.def("set_transfer_syntax",
		    [](DicomFile& self, const std::string& transfer_syntax_text,
		        const EncoderContext& encoder_context) {
			    self.set_transfer_syntax(
			        parse_transfer_syntax_text_or_throw(transfer_syntax_text),
			        encoder_context);
		    },
		    nb::arg("transfer_syntax"),
		    nb::kw_only(),
		    nb::arg("encoder_context"),
		    "Set transfer syntax using transfer syntax text and a preconfigured EncoderContext.")
		.def("set_transfer_syntax_state_only",
		    [](DicomFile& self, const Uid& transfer_syntax) {
			    self.set_transfer_syntax_state_only(
			        require_transfer_syntax_uid_or_throw(transfer_syntax));
		    },
		    nb::arg("transfer_syntax"),
		    "Advanced API: update only the in-memory transfer syntax state.\n"
		    "This does not transcode PixelData and does not update file meta (0002,0010).\n"
		    "Use set_transfer_syntax(...) for a full conversion, or rebuild_file_meta()\n"
		    "afterward if file meta should match the current runtime state.")
		.def("set_transfer_syntax_state_only",
		    [](DicomFile& self, const std::string& transfer_syntax_text) {
			    self.set_transfer_syntax_state_only(
			        parse_transfer_syntax_text_or_throw(transfer_syntax_text));
		    },
		    nb::arg("transfer_syntax"),
		    "Advanced API: update only the in-memory transfer syntax state.\n"
		    "This does not transcode PixelData and does not update file meta (0002,0010).\n"
		    "Use set_transfer_syntax(...) for a full conversion, or rebuild_file_meta()\n"
		    "afterward if file meta should match the current runtime state.")
		.def("set_declared_specific_charset",
		    [](DicomFile& self, nb::handle value) {
			    const auto charsets =
			        parse_specific_character_set_argument(value, "specific_charset");
			    self.set_declared_specific_charset(charsets);
		    },
		    nb::arg("specific_charset"),
		    "Update only the declared (0008,0005) Specific Character Set.\n"
		    "Accepts a defined-term string such as 'ISO_IR 192', an empty/None value for the default repertoire,\n"
		    "or a non-empty sequence of defined-term strings for ISO 2022 multi-term declarations.")
		.def("set_specific_charset",
		    [](DicomFile& self, nb::handle value, nb::handle errors,
		        bool return_replaced) -> nb::object {
			    const auto charsets =
			        parse_specific_character_set_argument(value, "specific_charset");
			    bool replaced = false;
			    self.set_specific_charset(charsets,
			        parse_charset_encode_error_policy(errors, "errors"),
			        return_replaced ? &replaced : nullptr);
			    if (return_replaced) {
				    return nb::cast(replaced);
			    }
			    return nb::none();
		    },
		    nb::arg("specific_charset"),
		    nb::kw_only(),
		    nb::arg("errors") = nb::str("strict"),
		    nb::arg("return_replaced") = false,
		    "Transcode text value bytes to a new Specific Character Set and update (0008,0005).\n"
		    "Accepts the same forms as set_declared_specific_charset().\n"
		    "errors: 'strict', 'replace_qmark', or 'replace_unicode_escape'.\n"
		    "When return_replaced=True, returns whether replacement occurred.")
		.def("set_pixel_data",
		    [](DicomFile& self, const Uid& transfer_syntax, nb::handle source,
		        std::optional<std::size_t> frame_index, nb::handle options) {
			    set_pixel_data_with_options(
			        self, transfer_syntax, source, frame_index, options);
		    },
		    nb::arg("transfer_syntax"),
		    nb::arg("source"),
		    nb::kw_only(),
		    nb::arg("frame_index") = nb::none(),
		    nb::arg("options") = nb::none(),
		    "Set PixelData from a C-contiguous numeric buffer.\n"
		    "\n"
		    "Supported source shapes:\n"
		    "- (rows, cols)                        -> single-frame monochrome\n"
		    "- (rows, cols, samples[1|3])         -> single-frame interleaved\n"
		    "- (frames, rows, cols)               -> multi-frame monochrome\n"
		    "- (frames, rows, cols, samples[1|3]) -> multi-frame interleaved\n"
		    "\n"
		    "Supported dtypes: int8/uint8/int16/uint16/int32/uint32/float32/float64.\n"
		    "\n"
		    "When frame_index is provided, encode one single-frame source buffer into an existing encapsulated PixelData slot.")
		.def("set_pixel_data",
		    [](DicomFile& self, const std::string& transfer_syntax_text,
		        nb::handle source, std::optional<std::size_t> frame_index,
		        nb::handle options) {
			    set_pixel_data_with_options(self,
			        parse_transfer_syntax_text_or_throw(transfer_syntax_text),
			        source, frame_index, options);
		    },
		    nb::arg("transfer_syntax"),
		    nb::arg("source"),
		    nb::kw_only(),
		    nb::arg("frame_index") = nb::none(),
		    nb::arg("options") = nb::none(),
		    "Set PixelData from transfer syntax text and a C-contiguous numeric buffer.")
		.def("set_pixel_data",
		    [](DicomFile& self, const Uid& transfer_syntax, nb::handle source,
		        std::optional<std::size_t> frame_index,
		        const EncoderContext& encoder_context) {
			    set_pixel_data_with_encoder_context(
			        self, transfer_syntax, source, frame_index, encoder_context);
		    },
		    nb::arg("transfer_syntax"),
		    nb::arg("source"),
		    nb::kw_only(),
		    nb::arg("frame_index") = nb::none(),
		    nb::arg("encoder_context"),
		    "Set PixelData using a preconfigured EncoderContext.")
		.def("set_pixel_data",
		    [](DicomFile& self, const std::string& transfer_syntax_text,
		        nb::handle source, std::optional<std::size_t> frame_index,
		        const EncoderContext& encoder_context) {
			    set_pixel_data_with_encoder_context(self,
			        parse_transfer_syntax_text_or_throw(transfer_syntax_text),
			        source, frame_index, encoder_context);
		    },
		    nb::arg("transfer_syntax"),
		    nb::arg("source"),
		    nb::kw_only(),
		    nb::arg("frame_index") = nb::none(),
		    nb::arg("encoder_context"),
		    "Set PixelData from transfer syntax text using a preconfigured EncoderContext.")
		.def("write_file",
		    [](DicomFile& self, nb::handle path, bool include_preamble,
		        bool write_file_meta, bool keep_existing_meta) {
			    self.write_file(python_path_to_filesystem_path(path, "path"),
			        make_write_options(include_preamble, write_file_meta, keep_existing_meta));
		    },
		    nb::arg("path"),
		    nb::kw_only(),
		    nb::arg("include_preamble") = true,
		    nb::arg("write_file_meta") = true,
		    nb::arg("keep_existing_meta") = true,
		    "Write this DicomFile to `path` using the current dataset state.")
		.def("write_with_transfer_syntax",
		    [](DicomFile& self, nb::handle path, const Uid& transfer_syntax,
		        nb::handle options, bool include_preamble, bool write_file_meta,
		        bool keep_existing_meta) {
			    write_with_transfer_syntax_with_options(self, path, transfer_syntax,
			        options, include_preamble, write_file_meta, keep_existing_meta);
		    },
		    nb::arg("path"),
		    nb::arg("transfer_syntax"),
		    nb::kw_only(),
		    nb::arg("options") = nb::none(),
		    nb::arg("include_preamble") = true,
		    nb::arg("write_file_meta") = true,
		    nb::arg("keep_existing_meta") = true,
		    "Write this DicomFile to `path` using the requested transfer syntax without "
		    "mutating the source object.")
		.def("write_with_transfer_syntax",
		    [](DicomFile& self, nb::handle path, const std::string& transfer_syntax_text,
		        nb::handle options, bool include_preamble, bool write_file_meta,
		        bool keep_existing_meta) {
			    write_with_transfer_syntax_with_options(self, path,
			        parse_transfer_syntax_text_or_throw(transfer_syntax_text), options,
			        include_preamble, write_file_meta, keep_existing_meta);
		    },
		    nb::arg("path"),
		    nb::arg("transfer_syntax"),
		    nb::kw_only(),
		    nb::arg("options") = nb::none(),
		    nb::arg("include_preamble") = true,
		    nb::arg("write_file_meta") = true,
		    nb::arg("keep_existing_meta") = true,
		    "Write this DicomFile to `path` using transfer syntax text without mutating "
		    "the source object.")
		.def("write_with_transfer_syntax",
		    [](DicomFile& self, nb::handle path, const Uid& transfer_syntax,
		        const EncoderContext& encoder_context, bool include_preamble,
		        bool write_file_meta, bool keep_existing_meta) {
			    write_with_transfer_syntax_with_encoder_context(self, path, transfer_syntax,
			        encoder_context, include_preamble, write_file_meta,
			        keep_existing_meta);
		    },
		    nb::arg("path"),
		    nb::arg("transfer_syntax"),
		    nb::kw_only(),
		    nb::arg("encoder_context"),
		    nb::arg("include_preamble") = true,
		    nb::arg("write_file_meta") = true,
		    nb::arg("keep_existing_meta") = true,
		    "Write this DicomFile to `path` using the requested transfer syntax and a "
		    "preconfigured EncoderContext without mutating the source object.")
		.def("write_with_transfer_syntax",
		    [](DicomFile& self, nb::handle path, const std::string& transfer_syntax_text,
		        const EncoderContext& encoder_context, bool include_preamble,
		        bool write_file_meta, bool keep_existing_meta) {
			    write_with_transfer_syntax_with_encoder_context(self, path,
			        parse_transfer_syntax_text_or_throw(transfer_syntax_text),
			        encoder_context, include_preamble, write_file_meta,
			        keep_existing_meta);
		    },
		    nb::arg("path"),
		    nb::arg("transfer_syntax"),
		    nb::kw_only(),
		    nb::arg("encoder_context"),
		    nb::arg("include_preamble") = true,
		    nb::arg("write_file_meta") = true,
		    nb::arg("keep_existing_meta") = true,
		    "Write this DicomFile to `path` using transfer syntax text and a "
		    "preconfigured EncoderContext without mutating the source object.")
		.def("write_bytes",
		    [](DicomFile& self, bool include_preamble, bool write_file_meta,
		        bool keep_existing_meta) {
			    return to_python_bytes(self.write_bytes(
			        make_write_options(include_preamble, write_file_meta, keep_existing_meta)));
		    },
		    nb::kw_only(),
		    nb::arg("include_preamble") = true,
		    nb::arg("write_file_meta") = true,
		    nb::arg("keep_existing_meta") = true,
		    "Serialize this DicomFile to bytes using the current dataset state.")
		.def("write_json",
		    [](DicomFile& self, bool include_group_0002, nb::handle bulk_data,
		        std::size_t bulk_data_threshold, const std::string& bulk_data_uri_template,
		        const std::string& pixel_data_uri_template, nb::handle charset_errors) {
			    return json_write_result_to_python(
			        self.write_json(make_json_write_options(
			            include_group_0002, bulk_data, bulk_data_threshold,
			            bulk_data_uri_template, pixel_data_uri_template, charset_errors)),
			        nb::cast(&self, nb::rv_policy::reference));
		    },
		    nb::kw_only(),
		    nb::arg("include_group_0002") = false,
		    nb::arg("bulk_data") = nb::str("inline"),
		    nb::arg("bulk_data_threshold") = static_cast<std::size_t>(1024),
		    nb::arg("bulk_data_uri_template") = "",
		    nb::arg("pixel_data_uri_template") = "",
		    nb::arg("charset_errors") = nb::str("strict"),
		    "Serialize this DicomFile root dataset using the DICOM JSON Model.\n"
		    "\n"
		    "Returns\n"
		    "-------\n"
		    "(json_text, bulk_parts) : tuple[str, list[tuple[str, memoryview, str, str]]]\n"
		    "    `json_text` is the DICOM JSON payload. `bulk_parts` contains\n"
		    "    `(uri, memoryview, media_type, transfer_syntax_uid)` tuples for\n"
		    "    bulk values emitted via `BulkDataURI`.\n"
		    "    Encapsulated multi-frame PixelData is returned one frame per bulk\n"
		    "    part while the JSON keeps a single base `BulkDataURI`. Native\n"
		    "    multi-frame PixelData remains one aggregate bulk part.\n"
		    "    The returned memoryviews borrow the underlying DicomFile storage,\n"
		    "    so keep the owning DicomFile alive while using them.\n"
		    "\n"
		    "Keyword options match DataSet.write_json().\n"
		    "\n"
		    "Example bulk_data_uri_template:\n"
		    "    `/dicomweb/studies/{study}/series/{series}/instances/{instance}/bulk/{tag}`\n"
		    "Here `{tag}` expands to the 8-digit tag at top level and to a dotted\n"
		    "tag path for nested sequence items.\n"
		    "Example pixel_data_uri_template:\n"
		    "    `/dicomweb/studies/{study}/series/{series}/instances/{instance}/frames`\n"
		    "Use both together when a dataset may contain bulk attributes besides PixelData.")
		.def("set_bulk_data",
		    [](DicomFile& self, const dicom::JsonBulkRef& ref, nb::handle source) {
			    auto bytes = pybuffer_to_bytes(source);
			    return self.set_bulk_data(
			        ref, std::span<const std::uint8_t>(bytes.data(), bytes.size()));
		    },
		    nb::arg("ref"),
		    nb::arg("source"),
		    "Copy downloaded JSON BulkDataURI content into the referenced destination.\n"
		    "\n"
		    "Parameters\n"
		    "----------\n"
		    "ref : JsonBulkRef\n"
		    "    Pending bulk reference returned by read_json().\n"
		    "source : bytes-like\n"
		    "    Downloaded payload bytes for that URI.\n"
		    "\n"
		    "Returns\n"
		    "-------\n"
		    "bool\n"
		    "    True when the payload was accepted and written into the DicomFile.")
		.def("__iter__",
		    [](DicomFile& self) {
			    return PyDataElementIterator(self.dataset());
		    },
		    nb::keep_alive<0, 1>(),
		    "Iterate over DataElements from the root DataSet")
		.def("walk",
		    [](DicomFile& self) {
			    return PyDataSetWalkIterator(
			        self.dataset(), nb::cast(&self, nb::rv_policy::reference));
		    },
		    nb::keep_alive<0, 1>(),
		    "Depth-first preorder walk over the root DataSet and all nested sequence items.\n"
		    "Each iteration yields DataSetWalkEntry(path, element). Call entry.skip_sequence()\n"
		    "or walker.skip_sequence() after receiving an SQ entry to prune its nested\n"
		    "subtree. Use entry.skip_current_dataset() or walker.skip_current_dataset()\n"
		    "to prune the rest of the current dataset.\n"
		    "\n"
		    "The returned ``entry.path`` is a borrowed view. Use it only within the\n"
		    "current iteration step, and persist ``entry.path.to_string()`` if you\n"
		    "need to keep it after the walker advances.")
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
		.def("encoded_pixel_frame_bytes",
		    [](DicomFile& self, std::size_t frame_index) {
			    const auto encoded = self.encoded_pixel_frame_bytes(frame_index);
			    if (encoded.empty()) {
				    return nb::bytes("", 0);
			    }
			    return nb::bytes(reinterpret_cast<const char*>(encoded.data()), encoded.size());
		    },
		    nb::arg("frame_index"),
		    "Return one encapsulated PixelData frame as detached bytes.")
		.def("encoded_pixel_frame_view",
		    [](DicomFile& self, std::size_t frame_index) {
			    const auto span = self.encoded_pixel_frame_view(frame_index);
			    return readonly_memoryview_from_span(
			        span.data(), span.size(), nb::cast(&self, nb::rv_policy::reference));
		    },
		    nb::arg("frame_index"),
		    "Return a read-only memoryview over one encapsulated PixelData frame.")
		.def("set_encoded_pixel_frame",
		    [](DicomFile& self, std::size_t frame_index, nb::handle source) {
			    auto bytes = pybuffer_to_bytes(source);
			    self.set_encoded_pixel_frame(
			        frame_index, std::span<const std::uint8_t>(bytes.data(), bytes.size()));
		    },
		    nb::arg("frame_index"),
		    nb::arg("source"),
		    "Replace one encapsulated PixelData frame from a contiguous buffer object.")
		.def("add_encoded_pixel_frame",
		    [](DicomFile& self, nb::handle source) {
			    auto bytes = pybuffer_to_bytes(source);
			    static_cast<void>(self.add_encoded_pixel_frame(
			        std::span<const std::uint8_t>(bytes.data(), bytes.size())));
		    },
		    nb::arg("source"),
		    "Append one encapsulated PixelData frame from a contiguous buffer object.")
		.def("to_array",
		    &dicomfile_to_array_maybe_with_info,
		    nb::arg("frame") = -1,
		    nb::arg("decode_mct") = true,
		    nb::kw_only(),
		    nb::arg("worker_threads") = -1,
		    nb::arg("codec_threads") = -1,
		    nb::arg("plan") = nb::none(),
		    nb::arg("with_info") = false,
		    "Decode pixel samples and return a NumPy array.\n"
		    "\n"
		    "Parameters\n"
		    "----------\n"
		    "frame : int, optional\n"
		    "    -1 decodes all frames (multi-frame only), otherwise decode the selected frame index.\n"
		    "decode_mct : bool, optional\n"
		    "    Whether to apply codestream-level MCT/color inverse transform when supported.\n"
		    "    True by default. Currently honored by OpenJPEG-based decode paths.\n"
		    "    OpenJPH backend ignores this flag.\n"
		    "worker_threads : int, optional\n"
		    "    DicomSDL-managed outer worker count for batch/multi-frame decode.\n"
		    "    -1 uses the current API-specific auto scheduling policy,\n"
		    "    0/1 disables outer parallelism,\n"
		    "    and >1 requests an explicit worker count.\n"
		    "codec_threads : int, optional\n"
		    "    Codec/backend internal thread-count hint.\n"
		    "    -1 uses the current API-specific/backend-aware auto policy,\n"
		    "    0 uses library default/sequential,\n"
		    "    and >0 requests an explicit internal thread count.\n"
		    "plan : DecodePlan, optional\n"
		    "    Reuse a previously computed DecodePlan. When provided, plan options win.\n"
		    "with_info : bool, optional\n"
		    "    When True, return `(array, decode_info)` instead of just the array.\n"
		    "    For frame=-1 on multi-frame input, DecodeInfo reports frame-0/common\n"
		    "    decode metadata.\n"
		    "\n"
		    "Returns\n"
		    "-------\n"
		    "numpy.ndarray or tuple[numpy.ndarray, DecodeInfo]\n"
		    "    Shape is (rows, cols) or (rows, cols, samples) for a single frame,\n"
		    "    and (frames, rows, cols) or (frames, rows, cols, samples) when decoding all frames.\n"
		    "    When `with_info=True`, returns `(array, decode_info)`.")
		.def("to_array_view",
		    &dicomfile_to_array_view,
		    nb::arg("frame") = -1,
		    "Return a zero-copy read-only NumPy view over uncompressed source pixel data.\n"
		    "\n"
		    "This requires:\n"
		    "- uncompressed transfer syntax\n"
		    "- frame layout compatible with interleaved output.")
		.def("decode_into",
		    &dicomfile_decode_into_maybe_with_info,
		    nb::arg("out"),
		    nb::arg("frame") = 0,
		    nb::arg("decode_mct") = true,
		    nb::kw_only(),
		    nb::arg("worker_threads") = -1,
		    nb::arg("codec_threads") = -1,
		    nb::arg("plan") = nb::none(),
		    nb::arg("with_info") = false,
		    "Decode pixel samples into an existing writable C-contiguous buffer.\n"
		    "\n"
		    "Parameters\n"
		    "----------\n"
		    "out : buffer-like\n"
		    "    Destination NumPy array or writable contiguous buffer. Size must\n"
		    "    exactly match the decoded output for the requested frame selection.\n"
		    "frame : int, optional\n"
		    "    -1 decodes all frames (multi-frame only), otherwise decode the selected frame index.\n"
		    "decode_mct : bool, optional\n"
		    "    Whether to apply codestream-level MCT/color inverse transform when supported.\n"
		    "    True by default. Currently honored by OpenJPEG-based decode paths.\n"
		    "    OpenJPH backend ignores this flag.\n"
		    "worker_threads : int, optional\n"
		    "    DicomSDL-managed outer worker count for batch/multi-frame decode.\n"
		    "    -1 uses the current API-specific auto scheduling policy,\n"
		    "    0/1 disables outer parallelism,\n"
		    "    and >1 requests an explicit worker count.\n"
		    "codec_threads : int, optional\n"
		    "    Codec/backend internal thread-count hint.\n"
		    "    -1 uses the current API-specific/backend-aware auto policy,\n"
		    "    0 uses library default/sequential,\n"
		    "    and >0 requests an explicit internal thread count.\n"
		    "plan : DecodePlan, optional\n"
		    "    Reuse a previously computed DecodePlan. When provided, plan options win.\n"
		    "with_info : bool, optional\n"
		    "    When True, return DecodeInfo instead of echoing `out`.\n"
		    "    For frame=-1 on multi-frame input, DecodeInfo reports frame-0/common\n"
		    "    decode metadata.\n"
		    "\n"
		    "Raises\n"
		    "------\n"
		    "ValueError\n"
		    "    If the requested frame, output buffer size/itemsize, or decoded layout is invalid.\n"
		    "RuntimeError\n"
		    "    If native decode fails after argument validation.\n"
		    "\n"
		    "Returns\n"
		    "-------\n"
		    "Same object as `out` for call chaining, or DecodeInfo when `with_info=True`.")
		.def("pixel_array",
		    &dicomfile_to_array_maybe_with_info,
		    nb::arg("frame") = -1,
		    nb::arg("decode_mct") = true,
		    nb::kw_only(),
		    nb::arg("worker_threads") = -1,
		    nb::arg("codec_threads") = -1,
		    nb::arg("plan") = nb::none(),
		    nb::arg("with_info") = false,
		    "Alias of to_array(frame=-1, decode_mct=True).\n"
		    "When `with_info=True`, returns `(array, decode_info)` like to_array(...).")
		.def("__getitem__",
		    [](DicomFile& self, nb::object key) -> DataElement& {
			    return dataset_lookup_dataelement_py(
			        self.dataset(), key, "DicomFile indices must be Tag, int (0xGGGEEEE), or str");
		    },
		    nb::arg("key"),
		    nb::rv_policy::reference_internal,
		    "Index syntax delegated to the root DataSet; returns a DataElement "
		    "reference and preserves the missing-element sentinel behavior.")
		.def("__contains__",
		    [](DicomFile& self, nb::object key) {
			    return dataset_contains_py(
			        self.dataset(), key, "DicomFile membership keys must be Tag, int (0xGGGEEEE), or str");
		    },
		    nb::arg("key"),
		    "Presence probe delegated to the root DataSet.")
		.def("__getattr__",
		    [](DicomFile& self, const std::string& name) -> nb::object {
			    if (!name.empty() && name.size() >= 2 && name[0] != '_') {
				    const auto [tag, _vr] = dicom::lookup::keyword_to_tag_vr(name);
				    if (static_cast<bool>(tag)) {
					    DataElement& el = self.get_dataelement(tag);
					    return dataelement_get_value_py(el, nb::cast(&self, nb::rv_policy::reference));
				    }
			    }
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

	nb::class_<dicom::DataSetSelection>(m, "DataSetSelection",
	    "Canonicalized nested dataset selection used by selected-read APIs.\n"
	    "\n"
	    "Construct from a sequence of selection nodes. Leaf nodes may be Tag, packed\n"
	    "int (0xGGGGEEEE), or str. Nested nodes use a 2-item pair: (tag, children).\n"
	    "Example: ['StudyInstanceUID', ('ReferencedSeriesSequence', ['SeriesInstanceUID'])].")
		.def(nb::init<>())
		.def("__init__",
		    [](dicom::DataSetSelection* self, nb::handle nodes) {
			    new (self) dicom::DataSetSelection(selection_nodes_from_py(nodes, "selection"));
		    },
		    nb::arg("nodes"))
		.def("__len__", [](const dicom::DataSetSelection& self) { return self.nodes().size(); })
		.def("__bool__", [](const dicom::DataSetSelection& self) { return !self.empty(); })
		.def("__repr__",
		    [](const dicom::DataSetSelection& self) {
			    std::ostringstream oss;
			    oss << "DataSetSelection(size=" << self.nodes().size() << ")";
			    return oss.str();
		    });

	m.def("create_encoder_context",
	    [](const Uid& transfer_syntax, nb::handle options) {
		    return create_encoder_context_with_options(
		        transfer_syntax, options);
	    },
	    nb::arg("transfer_syntax"),
	    nb::kw_only(),
	    nb::arg("options") = nb::none(),
	    "Create an EncoderContext from a Uid transfer syntax and optional codec options.");

	m.def("create_encoder_context",
	    [](const std::string& transfer_syntax_text, nb::handle options) {
		    return create_encoder_context_with_options(
		        parse_transfer_syntax_text_or_throw(transfer_syntax_text), options);
	    },
	    nb::arg("transfer_syntax"),
	    nb::kw_only(),
	    nb::arg("options") = nb::none(),
	    "Create an EncoderContext from transfer syntax text and optional codec options.");

	m.def("apply_rescale",
	    [](nb::handle source, float slope, float intercept) {
		    return apply_rescale_to_numpy_array(source, slope, intercept);
	    },
	    nb::arg("source"),
	    nb::arg("slope"),
	    nb::arg("intercept"),
	    "Apply a linear rescale and return a NumPy array backed by owned output storage.");

	m.def("apply_rescale",
	    [](nb::handle source, const dicom::pixel::RescaleTransform& transform) {
		    return apply_rescale_to_numpy_array(
		        source, transform.slope, transform.intercept);
	    },
	    nb::arg("source"),
	    nb::arg("transform"),
	    "Apply a RescaleTransform and return a NumPy array backed by owned output storage.");

	m.def("apply_rescale_frames",
	    &apply_rescale_frames_to_numpy_array,
	    nb::arg("source"),
	    nb::arg("slopes"),
	    nb::arg("intercepts"),
	    "Apply per-frame linear rescale values and return a NumPy array backed by owned output storage.");

	m.def("apply_modality_lut",
	    &apply_modality_lut_to_numpy_array,
	    nb::arg("source"),
	    nb::arg("lut"),
	    "Apply a modality LUT and return a NumPy array backed by owned output storage.");
	m.def("apply_palette_lut",
	    &apply_palette_lut_to_numpy_array,
	    nb::arg("source"),
	    nb::arg("lut"),
	    "Apply a palette LUT and return a NumPy array backed by owned output storage.");
	m.def("apply_voi_lut",
	    &apply_voi_lut_to_numpy_array,
	    nb::arg("source"),
	    nb::arg("lut"),
	    "Apply a VOI LUT and return a NumPy array backed by owned output storage.");
	m.def("apply_window",
	    [](nb::handle source, float center, float width,
	        dicom::pixel::VoiLutFunction function) {
		    return apply_window_to_numpy_array(
		        source, dicom::pixel::WindowTransform{
		                    .center = center,
		                    .width = width,
		                    .function = function,
		                });
	    },
	    nb::arg("source"),
	    nb::arg("center"),
	    nb::arg("width"),
	    nb::arg("function") = dicom::pixel::VoiLutFunction::linear,
	    "Apply a VOI window transform and return a NumPy array backed by owned output storage.");
	m.def("apply_window",
	    &apply_window_to_numpy_array,
	    nb::arg("source"),
	    nb::arg("window"),
	    "Apply a WindowTransform and return a NumPy array backed by owned output storage.");

	m.def("read_json",
	    [](nb::handle source, const std::string& name, nb::handle charset_errors) {
		    const auto options = make_json_read_options(charset_errors);
		    if (nb::isinstance<nb::str>(source)) {
			    auto [buffer_name, buffer] = json_source_to_named_buffer(source, name);
			    return json_read_result_to_python(dicom::read_json(
			        std::move(buffer_name), std::move(buffer), options));
		    }

		    PyBufferView view(source);
		    const Py_buffer& info = view.view();
		    if (info.ndim != 1) {
			    throw std::invalid_argument("read_json expects a 1-D UTF-8 string or bytes-like object");
		    }
		    if (!PyBuffer_IsContiguous(&info, 'C')) {
			    throw std::invalid_argument("read_json expects a contiguous bytes-like object");
		    }
		    const std::size_t elem_size =
		        static_cast<std::size_t>(info.itemsize <= 0 ? 1 : info.itemsize);
		    if (elem_size != 1) {
			    throw std::invalid_argument(
			        "read_json requires a byte-oriented buffer for bytes-like input");
		    }
		    const auto size = static_cast<std::size_t>(info.len);
		    return json_read_result_to_python(
		        dicom::read_json(std::string{name},
		            reinterpret_cast<const std::uint8_t*>(info.buf), size, options));
	    },
	    nb::arg("source"),
	    nb::kw_only(),
	    nb::arg("name") = "<memory>",
	    nb::arg("charset_errors") = nb::str("strict"),
	    "Read DICOM JSON from a UTF-8 string or bytes-like object.\n"
	    "\n"
	    "Returns\n"
	    "-------\n"
	    "list[tuple[DicomFile, list[JsonBulkRef]]]\n"
	    "    One item per top-level JSON dataset object. A single JSON object returns\n"
	    "    a one-item list; a top-level JSON array returns one tuple per dataset.\n"
	    "    Each tuple contains the parsed DicomFile plus unresolved BulkDataURI\n"
	    "    references that still need payload bytes. JsonBulkRef.media_type and\n"
	    "    JsonBulkRef.transfer_syntax_uid are populated when they can be inferred\n"
	    "    from the JSON/meta context; otherwise they are empty strings.\n"
	    "\n"
	    "Parameters\n"
	    "----------\n"
	    "source : str | bytes-like\n"
	    "    UTF-8 DICOM JSON text or an equivalent bytes-like object.\n"
	    "name : str, optional\n"
	    "    Identifier attached to the in-memory dataset tree. Defaults to '<memory>'.\n"
	    "charset_errors : {'strict', 'replace_qmark', 'replace_unicode_escape'}, optional\n"
	    "    Policy used when lazy raw-byte materialization must encode UTF-8 JSON text\n"
	    "    into a declared SpecificCharacterSet. Defaults to 'strict'.\n"
	    "\n"
	    "Typical flow:\n"
	    "    items = dicom.read_json(text)\n"
	    "    for df, refs in items:\n"
	    "        for ref in refs:\n"
	    "            payload = download(ref.uri)\n"
	    "            df.set_bulk_data(ref, payload)\n");

m.def("read_file",
    [](nb::handle path, std::optional<Tag> load_until, std::optional<bool> keep_on_error) {
	    dicom::ReadOptions opts;
	    if (load_until) {
		    opts.load_until = *load_until;
	    }
	    if (keep_on_error) {
		    opts.keep_on_error = *keep_on_error;
	    }
	    return dicom::read_file(python_path_to_filesystem_path(path, "path"), opts);
    },
    nb::arg("path"),
    nb::arg("load_until") = nb::none(),
    nb::arg("keep_on_error") = nb::none(),
    "Read a DICOM file from disk and return a DicomFile.\n"
    "\n"
    "Parameters\n"
    "----------\n"
    "path : str | os.PathLike\n"
    "    Filesystem path to the DICOM Part 10 file.\n"
    "load_until : Tag | None, optional\n"
    "    Stop after this tag is read (inclusive). Defaults to reading entire file.\n"
    "keep_on_error : bool | None, optional\n"
    "    When True, keep partially read data instead of raising on parse errors.\n"
    "    Inspect DicomFile.has_error and DicomFile.error_message after reading.\n");

	m.def("read_file_selected",
	    [](nb::handle path, nb::handle selection_value, std::optional<bool> keep_on_error) {
		    const auto selection = selection_from_py(selection_value, "selection");
		    dicom::ReadOptions opts;
		    if (keep_on_error) {
			    opts.keep_on_error = *keep_on_error;
		    }
		    return dicom::read_file_selected(
		        python_path_to_filesystem_path(path, "path"), selection, opts);
	    },
	    nb::arg("path"),
	    nb::arg("selection"),
	    nb::arg("keep_on_error") = nb::none(),
	    "Read a DICOM file from disk using a nested DataSetSelection.\n"
	    "\n"
	    "The returned DicomFile keeps only the selected elements and nested\n"
	    "sequence children. TransferSyntaxUID and\n"
	    "SpecificCharacterSet are always considered at the root level. Malformed\n"
	    "data outside the selected region may remain unseen and therefore may not\n"
	    "set has_error or error_message.\n"
	    "\n"
	    "Parameters\n"
	    "----------\n"
	    "path : str | os.PathLike\n"
	    "    Filesystem path to the DICOM Part 10 file.\n"
	    "selection : DataSetSelection | sequence\n"
	    "    A DataSetSelection instance, or a raw nested selection tree built from\n"
	    "    leaf tags and (tag, children) pairs. Use DataSetSelection(...) when you\n"
	    "    want to validate/canonicalize once and reuse the same selection.\n"
	    "keep_on_error : bool | None, optional\n"
	    "    When True, keep partially read selected data instead of raising on parse errors.\n"
	    "    Inspect DicomFile.has_error and DicomFile.error_message after reading.\n");

m.def("is_dicom_file",
    [](nb::handle path) {
	    return dicom::is_dicom_file(python_path_to_filesystem_path(path, "path"));
    },
    nb::arg("path"),
    "Fast 1 KiB prefix probe for file filtering.\n"
    "\n"
    "Parameters\n"
    "----------\n"
    "path : str | os.PathLike\n"
    "    Filesystem path to probe.\n"
    "\n"
    "Returns\n"
    "-------\n"
    "bool\n"
    "    True when the file prefix looks like a DICOM Part 10 stream or a raw\n"
    "    little-endian DICOM dataset. This is a sniffing heuristic, not a full\n"
    "    validation pass.\n");

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

	m.def("read_bytes_selected",
	    [](nb::object buffer, nb::handle selection_value, const std::string& name,
	        std::optional<bool> keep_on_error, bool copy) {
		    const auto selection = selection_from_py(selection_value, "selection");
		    PyBufferView view(buffer);
		    const Py_buffer& info = view.view();
		    if (info.ndim != 1) {
			    throw std::invalid_argument("read_bytes_selected expects a 1-D bytes-like object");
		    }
		    if (!PyBuffer_IsContiguous(&info, 'C')) {
			    throw std::invalid_argument(
			        "read_bytes_selected expects a contiguous bytes-like object");
		    }

		    const std::size_t elem_size =
		        static_cast<std::size_t>(info.itemsize <= 0 ? 1 : info.itemsize);
		    const std::size_t total = static_cast<std::size_t>(info.len);

		    dicom::ReadOptions opts;
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
			    file = dicom::read_bytes_selected(std::string{name}, std::move(owned), selection, opts);
		    } else {
			    if (elem_size != 1) {
				    throw std::invalid_argument(
				        "read_bytes_selected(copy=False) requires a byte-oriented buffer");
			    }
			    file = dicom::read_bytes_selected(
			        std::string{name}, static_cast<const std::uint8_t*>(info.buf), total, selection, opts);
		    }

		    nb::object py_file = nb::cast(std::move(file));
		    if (!copy && total > 0) {
			    py_file.attr("_buffer_owner") = buffer;
		    }
		    return py_file;
	    },
	    nb::arg("data"),
	    nb::arg("selection"),
	    nb::arg("name") = std::string{"<memory>"},
	    nb::arg("keep_on_error") = nb::none(),
	    nb::arg("copy") = true,
	    "Read a DicomFile from a bytes-like object while keeping only the selected tags\n"
	    "and nested sequence children.\n"
	    "\n"
	    "Malformed data outside the selected region may remain unseen and therefore\n"
	    "may not set has_error or error_message.\n"
	    "\n"
	    "Parameters\n"
	    "----------\n"
	    "data : buffer\n"
	    "    1-D bytes-like object containing the Part 10 stream (or raw stream).\n"
	    "selection : DataSetSelection | sequence\n"
	    "    A DataSetSelection instance, or a raw nested selection tree built from\n"
	    "    leaf tags and (tag, children) pairs. Use DataSetSelection(...) when you\n"
	    "    want to validate/canonicalize once and reuse the same selection.\n"
	    "name : str, optional\n"
	    "    Identifier reported by DicomFile.path() and diagnostics. Default '<memory>'.\n"
	    "keep_on_error : bool | None, optional\n"
	    "    When True, keep partially read selected data instead of raising on parse errors.\n"
	    "    Inspect DicomFile.has_error and DicomFile.error_message after reading.\n"
	    "copy : bool, optional\n"
	    "    When False, avoid copying and reference the caller's buffer; caller must keep\n"
	    "    the buffer alive while the returned DicomFile exists.\n");

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

m.def("set_htj2k_decoder_backend",
    [](const std::string& backend) {
	    std::string error{};
	    const auto parsed = parse_htj2k_decoder_backend(backend);
	    if (!dicom::pixel::set_htj2k_decoder_backend(parsed, &error)) {
		    if (error.empty()) {
			    error = "failed to configure HTJ2K decoder backend";
		    }
		    throw std::runtime_error(error);
	    }
    },
    nb::arg("backend"),
    "Configure the preferred HTJ2K decoder backend before first pixel runtime use.\n"
    "Accepted values: 'auto', 'openjph', 'openjpeg'.");

m.def("get_htj2k_decoder_backend",
    []() {
	    return std::string(htj2k_decoder_backend_name(
	        dicom::pixel::get_htj2k_decoder_backend()));
    },
    "Return the configured HTJ2K decoder backend preference.");

m.def("use_openjph_for_htj2k_decoding",
    []() {
	    std::string error{};
	    if (!dicom::pixel::use_openjph_for_htj2k_decoding(&error)) {
		    if (error.empty()) {
			    error = "failed to configure HTJ2K decoder backend";
		    }
		    throw std::runtime_error(error);
	    }
    },
    "Prefer the OpenJPH HTJ2K decoder before first pixel runtime use.");

m.def("use_openjpeg_for_htj2k_decoding",
    []() {
	    std::string error{};
	    if (!dicom::pixel::use_openjpeg_for_htj2k_decoding(&error)) {
		    if (error.empty()) {
			    error = "failed to configure HTJ2K decoder backend";
		    }
		    throw std::runtime_error(error);
	    }
    },
    "Prefer the OpenJPEG HTJ2K decoder before first pixel runtime use.");

m.def("register_external_codec_plugin",
    [](nb::handle library_path) {
	    std::string error{};
	    if (!dicom::pixel::register_external_codec_plugin_from_library(
	            python_path_to_filesystem_path(library_path, "library_path"), &error)) {
		    if (error.empty()) {
			    error = "failed to register external codec plugin";
		    }
		    throw nb::value_error(error.c_str());
	    }
    },
    nb::arg("library_path"),
    "Load an external codec plugin shared library (.dll/.so/.dylib).\n"
    "\n"
    "Parameters\n"
    "----------\n"
    "library_path : str | os.PathLike\n"
    "    Filesystem path to the shared library to load.");

m.def("clear_external_codec_plugins",
    []() {
	    std::string error{};
	    if (!dicom::pixel::clear_external_codec_plugins(&error)) {
		    if (error.empty()) {
			    error = "failed to clear external codec plugins";
		    }
		    throw nb::value_error(error.c_str());
	    }
    },
    "Unload every external codec plugin and restore builtin dispatch.");

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
		.def("uses_specific_character_set", &VR::uses_specific_character_set)
		.def("allows_multiple_text_values", &VR::allows_multiple_text_values)
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
			    return readonly_memoryview_from_span(
			        span.data(), span.size(), nb::cast(&f, nb::rv_policy::reference));
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
			    return readonly_memoryview_from_span(
			        span.data(), span.size(), nb::cast(&self, nb::rv_policy::reference));
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

m.def("transfer_syntax_uids",
    []() {
	    nb::list result;
	    for (const auto& entry : dicom::kUidRegistry) {
		    if (entry.uid_type != dicom::UidType::TransferSyntax) {
			    continue;
		    }
		    if (auto uid = dicom::uid::from_value(entry.value)) {
			    result.append(nb::cast(*uid));
		    }
	    }
	    return result;
    },
    "Return all well-known Transfer Syntax UIDs in registry order.");

m.def("transfer_syntax_uids_encode_supported",
    []() {
	    nb::list result;
	    for (const auto& entry : dicom::kUidRegistry) {
		    if (entry.uid_type != dicom::UidType::TransferSyntax) {
			    continue;
		    }
		    if (auto uid = dicom::uid::from_value(entry.value)) {
			    if (uid->supports_pixel_encode() &&
			        transfer_syntax_has_runtime_encode_support(*uid)) {
				    result.append(nb::cast(*uid));
			    }
		    }
	    }
	    return result;
    },
    "Return Transfer Syntax UIDs currently available for target encoding in set_transfer_syntax.");

m.def("uid_prefix",
    []() {
	    const auto value = dicom::uid::uid_prefix();
	    return std::string(value.data(), value.size());
    },
    "Return DicomSDL UID root prefix.");

m.def("implementation_class_uid",
    []() {
	    const auto value = dicom::uid::implementation_class_uid();
	    return std::string(value.data(), value.size());
    },
    "Return default Implementation Class UID used by DicomSDL.");

m.def("implementation_version_name",
    []() {
	    const auto value = dicom::uid::implementation_version_name();
	    return std::string(value.data(), value.size());
    },
    "Return default Implementation Version Name used by DicomSDL.");

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
    "Generate a UID under DicomSDL prefix; returns None on failure.");

m.def("generate_uid",
    []() {
	    return generated_uid_to_string(dicom::uid::generate_uid());
    },
    "Generate a UID under DicomSDL prefix. Raises RuntimeError on failure.");

m.def("generate_sop_instance_uid",
    []() {
	    return generated_uid_to_string(dicom::uid::generate_sop_instance_uid());
    },
    "Generate a SOP Instance UID under DicomSDL prefix.");

m.def("generate_series_instance_uid",
    []() {
	    return generated_uid_to_string(dicom::uid::generate_series_instance_uid());
    },
    "Generate a Series Instance UID under DicomSDL prefix.");

m.def("generate_study_instance_uid",
    []() {
	    return generated_uid_to_string(dicom::uid::generate_study_instance_uid());
    },
    "Generate a Study Instance UID under DicomSDL prefix.");

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
	    "Photometric",
	    "EncodedLossyState",
	    "Planar",
	    "DecodeOptions",
	    "DecodePlan",
	    "DecodeInfo",
	    "VoiLutFunction",
	    "PixelPresentation",
	    "WindowTransform",
	    "RescaleTransform",
	    "ModalityLut",
	    "VoiLut",
	    "PaletteLut",
	    "SupplementalPaletteInfo",
	    "EnhancedPaletteDataPathAssignmentInfo",
	    "EnhancedBlendingLutInfo",
	    "EnhancedPaletteItemInfo",
	    "EnhancedPaletteInfo",
	    "EncoderContext",
	    "create_encoder_context",
	    "apply_rescale",
	    "apply_rescale_frames",
	    "apply_window",
	    "apply_modality_lut",
	    "apply_voi_lut",
	    "apply_palette_lut",
	    "DicomFile",
	    "DataElement",
	    "DataSet",
	    "DataSetSelection",
	    "PersonName",
	    "PersonNameGroup",
	    "JsonBulkTargetKind",
	    "JsonBulkRef",
	    "Tag",
	    "VR",
	    "Uid",
	    "read_json",
	    "read_file",
	    "read_file_selected",
	    "is_dicom_file",
	    "read_bytes",
	    "read_bytes_selected",
	    "load_root_elements_reserve_hint",
	    "reset_root_elements_reserve_hint",
	    "set_htj2k_decoder_backend",
	    "get_htj2k_decoder_backend",
	    "use_openjph_for_htj2k_decoding",
	    "use_openjpeg_for_htj2k_decoding",
	    "register_external_codec_plugin",
	    "clear_external_codec_plugins",
	    "keyword_to_tag_vr",
	    "tag_to_keyword",
	    "tag_to_entry",
	    "lookup_uid",
	    "uid_from_value",
	    "uid_from_keyword",
	    "transfer_syntax_uids",
	    "transfer_syntax_uids_encode_supported",
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

