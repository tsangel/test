#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <iomanip>
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
#include "pixel/host/support/dicom_pixel_support.hpp"

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
	dicom::pixel::DecodePlan plan{};
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

struct DirectRawArrayAccess {
	DecodedArrayLayout layout{};
	std::span<const std::uint8_t> source_bytes{};
	std::size_t byte_offset{0};
};

DecodedArraySpec decoded_array_spec(
    const dicom::pixel::PixelDataInfo& info, bool to_modality_value) {
	if (to_modality_value) {
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

[[nodiscard]] dicom::pixel::PixelSource build_pixel_source_or_throw(
    const Py_buffer& view) {
	if (view.ndim < 2 || view.ndim > 4) {
		throw nb::value_error("set_pixel_data source must have ndim 2, 3, or 4");
	}
	if (view.itemsize <= 0) {
		throw nb::value_error("set_pixel_data source must have a positive itemsize");
	}
	if (view.len < 0) {
		throw nb::value_error("set_pixel_data source buffer length is invalid");
	}
	if (view.len > 0 && view.buf == nullptr) {
		throw nb::value_error("set_pixel_data source buffer is null");
	}
	if (view.shape == nullptr) {
		throw nb::value_error("set_pixel_data source must expose shape metadata");
	}
	if (view.format == nullptr || view.format[0] == '\0') {
		throw nb::value_error("set_pixel_data source must expose dtype format metadata");
	}

	const auto format = std::string_view(view.format);
	if (!format.empty() && format.front() == '>') {
		throw nb::value_error("set_pixel_data does not support big-endian source dtype");
	}

	const auto bytes_per_sample = static_cast<std::size_t>(view.itemsize);
	const auto data_type =
	    parse_pixel_source_data_type_or_throw(format, bytes_per_sample);

	const auto read_dim = [&](int axis) -> std::size_t {
		const auto value = view.shape[axis];
		if (value <= 0) {
			throw nb::value_error("set_pixel_data source shape values must be positive");
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
		throw nb::value_error(
		    "set_pixel_data currently supports samples_per_pixel 1 or 3");
	}

	if (rows > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
	    cols > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
	    frames > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
	    samples_per_pixel > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
		throw nb::value_error("set_pixel_data source dimensions exceed int range");
	}

	const auto max_size = std::numeric_limits<std::size_t>::max();
	if (samples_per_pixel != 0 &&
	    cols > (max_size / samples_per_pixel)) {
		throw nb::value_error("set_pixel_data row size overflow");
	}
	const auto row_components = cols * samples_per_pixel;
	if (bytes_per_sample != 0 &&
	    row_components > (max_size / bytes_per_sample)) {
		throw nb::value_error("set_pixel_data row stride overflow");
	}
	const auto row_stride = row_components * bytes_per_sample;
	if (rows != 0 && row_stride > (max_size / rows)) {
		throw nb::value_error("set_pixel_data frame stride overflow");
	}
	const auto frame_stride = row_stride * rows;
	if (frames != 0 && frame_stride > (max_size / frames)) {
		throw nb::value_error("set_pixel_data total byte size overflow");
	}
	const auto required_bytes = frame_stride * frames;
	const auto actual_bytes = static_cast<std::size_t>(view.len);
	if (actual_bytes != required_bytes) {
		throw nb::value_error(
		    "set_pixel_data source buffer size does not match inferred shape and dtype");
	}

	dicom::pixel::PixelSource source{};
	source.bytes = std::span<const std::uint8_t>(
	    reinterpret_cast<const std::uint8_t*>(view.buf), actual_bytes);
	source.data_type = data_type;
	source.rows = static_cast<int>(rows);
	source.cols = static_cast<int>(cols);
	source.row_stride = row_stride;
	source.frames = static_cast<int>(frames);
	source.frame_stride = frame_stride;
	source.samples_per_pixel = static_cast<int>(samples_per_pixel);
	source.planar = dicom::pixel::Planar::interleaved;
	source.photometric = samples_per_pixel == 1
	                         ? dicom::pixel::Photometric::monochrome2
	                         : dicom::pixel::Photometric::rgb;
	return source;
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

void set_pixel_data_with_options(DicomFile& self, Uid transfer_syntax,
    nb::handle source_obj, nb::handle options) {
	PyReadOnlyBufferView source_view(source_obj);
	const auto source = build_pixel_source_or_throw(source_view.view());
	const auto text_options =
	    parse_encoder_options_to_text_storage(options, transfer_syntax);
	if (text_options.auto_mode) {
		self.set_pixel_data(transfer_syntax, source);
		return;
	}
	self.set_pixel_data(transfer_syntax, source, text_options.span());
}

void set_pixel_data_with_encoder_context(DicomFile& self, Uid transfer_syntax,
    nb::handle source_obj, const EncoderContext& encoder_context) {
	PyReadOnlyBufferView source_view(source_obj);
	const auto source = build_pixel_source_or_throw(source_view.view());
	self.set_pixel_data(transfer_syntax, source, encoder_context);
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

DecodedArrayLayout build_decode_layout(
    const DicomFile& self, long frame, bool to_modality_value, int decoder_threads = -1,
    bool decode_mct = true) {
	if (frame < -1) {
		throw nb::value_error("frame must be >= -1");
	}
	if (decoder_threads < -1) {
		throw nb::value_error("threads must be -1, 0, or positive");
	}

	DecodedArrayLayout layout{};
	layout.opt.planar_out = dicom::pixel::Planar::interleaved;
	layout.opt.alignment = 1;
	layout.opt.to_modality_value = to_modality_value;
	layout.opt.decode_mct = decode_mct;
	layout.opt.decoder_threads = decoder_threads;
	layout.plan = self.create_decode_plan(layout.opt);
	layout.opt = layout.plan.options;
	layout.dst_strides = layout.plan.strides;

	const auto& info = layout.plan.info;
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

	const auto rows = static_cast<std::size_t>(info.rows);
	const auto cols = static_cast<std::size_t>(info.cols);
	const auto samples_per_pixel = static_cast<std::size_t>(info.samples_per_pixel);
	layout.frames = static_cast<std::size_t>(info.frames);

	const bool effective_to_modality_value = layout.plan.options.to_modality_value;
	layout.spec = decoded_array_spec(info, effective_to_modality_value);

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

[[nodiscard]] bool resolve_to_modality_value_option(
    bool to_modality_value, nb::handle scaled_alias) {
	if (!scaled_alias.is_none()) {
		return nb::cast<bool>(scaled_alias);
	}
	return to_modality_value;
}

void decode_layout_into(const DicomFile& self, const DecodedArrayLayout& layout,
    std::span<std::uint8_t> out) {
	if (out.size() < layout.required_bytes) {
		throw nb::value_error("decode_into output buffer is smaller than required size");
	}
	if (!layout.decode_all_frames) {
		self.decode_into(layout.frame_index, out, layout.plan);
		return;
	}

	for (std::size_t frame_index = 0; frame_index < layout.frames; ++frame_index) {
		auto frame_span = out.subspan(frame_index * layout.frame_stride, layout.frame_stride);
		self.decode_into(frame_index, frame_span, layout.plan);
	}
}

DirectRawArrayAccess build_direct_raw_array_access_or_throw(
    const DicomFile& self, long frame) {
	if (frame < -1) {
		throw nb::value_error("frame must be >= -1");
	}

	const auto& info = self.pixeldata_info();
	if (!info.has_pixel_data) {
		throw nb::value_error(
		    "to_array/decode_into requires PixelData, FloatPixelData, or DoubleFloatPixelData");
	}
	if (!info.ts.is_uncompressed()) {
		throw nb::value_error("to_array_view requires an uncompressed transfer syntax");
	}
	if (info.rows <= 0 || info.cols <= 0 || info.samples_per_pixel <= 0) {
		throw nb::value_error(
		    "to_array/decode_into requires positive Rows/Columns/SamplesPerPixel");
	}
	if (info.frames <= 0) {
		throw nb::value_error("to_array/decode_into requires NumberOfFrames >= 1");
	}

	const auto source_view =
	    dicom::pixel::support_detail::compute_native_decode_source_view_or_throw(self, info);
	if (source_view.planar_source && source_view.samples_per_pixel > std::size_t{1}) {
		throw nb::value_error(
		    "to_array_view requires PlanarConfiguration=interleaved when SamplesPerPixel > 1");
	}

	DirectRawArrayAccess access{};
	auto& layout = access.layout;
	layout.spec = decoded_array_spec(info, false);
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
    const DicomFile& self, long frame, bool to_modality_value) {
	if (to_modality_value) {
		return std::nullopt;
	}
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
	if (access.layout.required_bytes == 0) {
		return;
	}
	std::memcpy(dst.data(), access.source_bytes.data() + access.byte_offset,
	    access.layout.required_bytes);
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
    const DicomFile& self, long frame, bool to_modality_value, bool decode_mct,
    nb::handle scaled) {
	const auto effective_to_modality_value =
	    resolve_to_modality_value_option(to_modality_value, scaled);
	if (auto direct = try_build_direct_raw_array_access(
	        self, frame, effective_to_modality_value)) {
		auto out = make_writable_numpy_array(direct->layout.ndim, direct->layout.shape,
		    direct->layout.strides, direct->layout.spec.dtype,
		    direct->layout.required_bytes);
		copy_direct_raw_array_access(*direct, out.bytes);
		return nb::cast(std::move(out.array));
	}
	const auto layout = build_decode_layout(
	    self, frame, effective_to_modality_value, -1, decode_mct);
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
    long frame, bool to_modality_value, int threads, bool decode_mct, nb::handle scaled) {
	const auto effective_to_modality_value =
	    resolve_to_modality_value_option(to_modality_value, scaled);
	if (threads < -1) {
		throw nb::value_error("threads must be -1, 0, or positive");
	}
	const auto direct =
	    try_build_direct_raw_array_access(self, frame, effective_to_modality_value);
	const auto layout =
	    direct ? direct->layout
	           : build_decode_layout(
	                 self, frame, effective_to_modality_value, threads, decode_mct);

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
		copy_direct_raw_array_access(*direct, out_span);
	} else {
		decode_layout_into(self, layout, out_span);
	}
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
	for (const auto& term : terms) {
		if (term.empty()) {
			throw nb::value_error(
			    (std::string(argument_name) + " must not contain empty terms").c_str());
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
		        std::size_t offset, std::size_t length) -> DataElement& {
		        const VR resolved = vr.value_or(VR::None);
		        return self.add_dataelement(tag, resolved, offset, length);
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
		        return self.get_dataelement(tag);
		    },
		    nb::arg("tag"), nb::rv_policy::reference_internal,
		    "Return the DataElement for a tag; missing lookups return a falsey DataElement (VR::None)")
		.def("get_dataelement",
		    [](DataSet& self, std::uint32_t packed) -> DataElement& {
			    const Tag tag(packed);
			    return self.get_dataelement(tag);
		    },
		    nb::arg("packed_tag"),
		    nb::rv_policy::reference_internal,
		    "Overload: pass packed 0xGGGEEEE integer; missing lookups return a falsey DataElement")
		.def("get_dataelement",
		    [](DataSet& self, const std::string& tag_str) -> DataElement& {
			    return self.get_dataelement(tag_str);
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
			    DataElement& el = [&]() -> DataElement& {
				    if (nb::isinstance<Tag>(key)) {
					    return self.get_dataelement(nb::cast<Tag>(key));
				    }
				    if (nb::isinstance<nb::int_>(key)) {
					    return self.get_dataelement(Tag(nb::cast<std::uint32_t>(key)));
				    }
				    if (nb::isinstance<nb::str>(key)) {
					    // Allow full tag-path strings (including sequences)
					    return self.get_dataelement(nb::cast<std::string>(key));
				    }
				    throw nb::type_error("DataSet indices must be Tag, int (0xGGGEEEE), or str");
			    }();

			    if (el.is_missing()) {
				    return nb::none();
			    }
			    return dataelement_get_value_py(el, nb::cast(&self, nb::rv_policy::reference));
		    },
		    nb::arg("key"),
		    "Index syntax: ds[tag|packed_int|tag_str] -> element.get_value(); returns None if missing")
		.def("__getattr__",
		    [](DataSet& self, const std::string& name) -> nb::object {
			    // Allow keyword-style attribute access: ds.PatientName -> get_value("PatientName")
			    if (!name.empty() && name.size() >= 2 && name[0] != '_') {
				    try {
					    Tag tag(name);
					    DataElement& el = self.get_dataelement(tag);
					    if (el.is_present()) {
						    return dataelement_get_value_py(el, nb::cast(&self, nb::rv_policy::reference));
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
	    "DICOM file/session object that owns the root DataSet.")
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
		        nb::handle options) {
			    set_pixel_data_with_options(self, transfer_syntax, source, options);
		    },
		    nb::arg("transfer_syntax"),
		    nb::arg("source"),
		    nb::kw_only(),
		    nb::arg("options") = nb::none(),
		    "Set PixelData from a C-contiguous numeric buffer.\n"
		    "\n"
		    "Supported source shapes:\n"
		    "- (rows, cols)                        -> single-frame monochrome\n"
		    "- (rows, cols, samples[1|3])         -> single-frame interleaved\n"
		    "- (frames, rows, cols)               -> multi-frame monochrome\n"
		    "- (frames, rows, cols, samples[1|3]) -> multi-frame interleaved\n"
		    "\n"
		    "Supported dtypes: int8/uint8/int16/uint16/int32/uint32/float32/float64.")
		.def("set_pixel_data",
		    [](DicomFile& self, const std::string& transfer_syntax_text,
		        nb::handle source, nb::handle options) {
			    set_pixel_data_with_options(self,
			        parse_transfer_syntax_text_or_throw(transfer_syntax_text),
			        source, options);
		    },
		    nb::arg("transfer_syntax"),
		    nb::arg("source"),
		    nb::kw_only(),
		    nb::arg("options") = nb::none(),
		    "Set PixelData from transfer syntax text and a C-contiguous numeric buffer.")
		.def("set_pixel_data",
		    [](DicomFile& self, const Uid& transfer_syntax, nb::handle source,
		        const EncoderContext& encoder_context) {
			    set_pixel_data_with_encoder_context(
			        self, transfer_syntax, source, encoder_context);
		    },
		    nb::arg("transfer_syntax"),
		    nb::arg("source"),
		    nb::kw_only(),
		    nb::arg("encoder_context"),
		    "Set PixelData using a preconfigured EncoderContext.")
		.def("set_pixel_data",
		    [](DicomFile& self, const std::string& transfer_syntax_text,
		        nb::handle source, const EncoderContext& encoder_context) {
			    set_pixel_data_with_encoder_context(self,
			        parse_transfer_syntax_text_or_throw(transfer_syntax_text),
			        source, encoder_context);
		    },
		    nb::arg("transfer_syntax"),
		    nb::arg("source"),
		    nb::kw_only(),
		    nb::arg("encoder_context"),
		    "Set PixelData from transfer syntax text using a preconfigured EncoderContext.")
		.def("write_file",
		    [](DicomFile& self, const std::string& path, bool include_preamble,
		        bool write_file_meta, bool keep_existing_meta) {
			    self.write_file(path,
			        make_write_options(include_preamble, write_file_meta, keep_existing_meta));
		    },
		    nb::arg("path"),
		    nb::kw_only(),
		    nb::arg("include_preamble") = true,
		    nb::arg("write_file_meta") = true,
		    nb::arg("keep_existing_meta") = true,
		    "Write this DicomFile to `path` using the current dataset state.")
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
		    nb::arg("to_modality_value") = false,
		    nb::arg("decode_mct") = true,
		    nb::arg("scaled") = nb::none(),
		    "Decode pixel samples and return a NumPy array.\n"
		    "\n"
		    "Parameters\n"
		    "----------\n"
		    "frame : int, optional\n"
		    "    -1 decodes all frames (multi-frame only), otherwise decode the selected frame index.\n"
		    "to_modality_value : bool, optional\n"
		    "    If True, convert stored values to modality values via Modality LUT/Rescale when available.\n"
		    "    Modality-value output is ignored when SamplesPerPixel != 1, or when both\n"
		    "    Modality LUT Sequence and Rescale Slope/Intercept are absent.\n"
		    "scaled : bool, optional\n"
		    "    Deprecated alias of to_modality_value.\n"
		    "decode_mct : bool, optional\n"
		    "    Whether to apply codestream-level MCT/color inverse transform when supported.\n"
		    "    True by default. Currently honored by OpenJPEG-based decode paths.\n"
		    "    OpenJPH backend ignores this flag.\n"
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
		    "- frame layout compatible with interleaved output.")
		.def("decode_into",
		    &dicomfile_decode_into_array,
		    nb::arg("out"),
		    nb::arg("frame") = 0,
		    nb::arg("to_modality_value") = false,
		    nb::arg("threads") = -1,
		    nb::arg("decode_mct") = true,
		    nb::arg("scaled") = nb::none(),
		    "Decode pixel samples into an existing writable C-contiguous buffer.\n"
		    "\n"
		    "Parameters\n"
		    "----------\n"
		    "out : buffer-like\n"
		    "    Destination NumPy array or writable contiguous buffer. Size must\n"
		    "    exactly match the decoded output for the requested frame selection.\n"
		    "frame : int, optional\n"
		    "    -1 decodes all frames (multi-frame only), otherwise decode the selected frame index.\n"
		    "to_modality_value : bool, optional\n"
		    "    If True, convert stored values to modality values via Modality LUT/Rescale when available.\n"
		    "scaled : bool, optional\n"
		    "    Deprecated alias of to_modality_value.\n"
		    "threads : int, optional\n"
		    "    Decoder thread count hint.\n"
		    "    Default is -1 (use all CPUs).\n"
		    "    0 uses library default, -1 uses all CPUs, >0 sets explicit thread count.\n"
		    "    Currently applied to JPEG 2000; unsupported decoders may ignore it.\n"
		    "    The OpenJPH HTJ2K backend currently accepts this hint and ignores it.\n"
		    "decode_mct : bool, optional\n"
		    "    Whether to apply codestream-level MCT/color inverse transform when supported.\n"
		    "    True by default. Currently honored by OpenJPEG-based decode paths.\n"
		    "    OpenJPH backend ignores this flag.\n"
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
		    "Same object as `out` for call chaining.")
		.def("pixel_array",
		    &dicomfile_to_array,
		    nb::arg("frame") = -1,
		    nb::arg("to_modality_value") = false,
		    nb::arg("decode_mct") = true,
		    nb::arg("scaled") = nb::none(),
		    "Alias of to_array(frame=-1, to_modality_value=False, decode_mct=True).")
		.def("__getitem__",
		    [](DicomFile& self, nb::object key) -> nb::object {
			    DataSet& dataset = self.dataset();
			    DataElement& el = [&]() -> DataElement& {
				    if (nb::isinstance<Tag>(key)) {
					    return dataset.get_dataelement(nb::cast<Tag>(key));
				    }
				    if (nb::isinstance<nb::int_>(key)) {
					    return dataset.get_dataelement(Tag(nb::cast<std::uint32_t>(key)));
				    }
				    if (nb::isinstance<nb::str>(key)) {
					    return dataset.get_dataelement(nb::cast<std::string>(key));
				    }
				    throw nb::type_error("DicomFile indices must be Tag, int (0xGGGEEEE), or str");
			    }();

			    if (el.is_missing()) {
				    return nb::none();
			    }
			    return dataelement_get_value_py(el, nb::cast(&dataset, nb::rv_policy::reference));
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
    [](const std::string& library_path) {
	    std::string error{};
	    if (!dicom::pixel::register_external_codec_plugin_from_library(
	            library_path, &error)) {
		    if (error.empty()) {
			    error = "failed to register external codec plugin";
		    }
		    throw nb::value_error(error.c_str());
	    }
    },
    nb::arg("library_path"),
    "Load an external codec plugin shared library (.dll/.so/.dylib).");

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
			    if (uid->supports_pixel_encode()) {
				    result.append(nb::cast(*uid));
			    }
		    }
	    }
	    return result;
    },
    "Return Transfer Syntax UIDs supported for target encoding in set_transfer_syntax.");

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
	    "EncoderContext",
	    "create_encoder_context",
	    "DicomFile",
	    "DataSet",
	    "Tag",
	    "VR",
	    "Uid",
	    "read_file",
	    "read_bytes",
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
