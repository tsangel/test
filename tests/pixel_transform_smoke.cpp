#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "dicom.h"

namespace {

[[noreturn]] void fail(const std::string& message) {
	std::cerr << message << std::endl;
	std::exit(1);
}

void expect_true(bool value, std::string_view label) {
	if (!value) {
		fail(std::string(label) + " expected true");
	}
}

template <typename T>
void expect_eq(const T& actual, const T& expected, std::string_view label) {
	if (!(actual == expected)) {
		fail(std::string(label) + " mismatch");
	}
}

void expect_near(double actual, double expected, double tolerance, std::string_view label) {
	if (std::fabs(actual - expected) > tolerance) {
		fail(std::string(label) + " mismatch");
	}
}

template <typename T>
std::vector<std::uint8_t> to_byte_buffer(std::span<const T> values) {
	std::vector<std::uint8_t> bytes(values.size() * sizeof(T), std::uint8_t{0});
	std::memcpy(bytes.data(), values.data(), bytes.size());
	return bytes;
}

template <typename T>
std::vector<T> from_byte_buffer(std::span<const std::uint8_t> bytes) {
	if (bytes.size() % sizeof(T) != 0) {
		fail("byte buffer size must be a multiple of sample size");
	}
	std::vector<T> values(bytes.size() / sizeof(T));
	std::memcpy(values.data(), bytes.data(), bytes.size());
	return values;
}

}  // namespace

int main() {
	using namespace dicom::literals;

	const dicom::pixel::PixelLayout packed_u16_layout{
	    .data_type = dicom::pixel::DataType::u16,
	    .photometric = dicom::pixel::Photometric::monochrome2,
	    .planar = dicom::pixel::Planar::interleaved,
	    .reserved = 0,
	    .rows = 2,
	    .cols = 3,
	    .frames = 2,
	    .samples_per_pixel = 1,
	    .bits_stored = 16,
	    .row_stride = 8,
	    .frame_stride = 16,
	};

	const auto add_single_item_sequence = [](auto& owner, dicom::Tag tag,
	                                      std::string_view label) -> dicom::DataSet* {
		auto& seq_elem = owner.add_dataelement(tag, dicom::VR::SQ);
		auto* seq = seq_elem.as_sequence();
		expect_true(seq != nullptr, std::string(label) + " should create sequence storage");
		auto* item = seq->add_dataset();
		expect_true(item != nullptr, std::string(label) + " should create item dataset");
		return item;
	};

	{
		// Rescale output layout should preserve geometry while normalizing to packed float storage.
		const auto rescaled_layout = dicom::pixel::make_rescale_output_layout(
		    packed_u16_layout, dicom::pixel::DataType::f32);
		expect_eq(rescaled_layout.data_type, dicom::pixel::DataType::f32,
		    "rescale output dtype");
		expect_eq(rescaled_layout.bits_stored, std::uint16_t{32},
		    "rescale output bits stored");
		expect_eq(rescaled_layout.rows, packed_u16_layout.rows, "rescale output rows");
		expect_eq(rescaled_layout.cols, packed_u16_layout.cols, "rescale output cols");
		expect_eq(rescaled_layout.frames, packed_u16_layout.frames, "rescale output frames");
		expect_eq(rescaled_layout.row_stride, std::size_t{12},
		    "rescale output row stride");
		expect_eq(rescaled_layout.frame_stride, std::size_t{24},
		    "rescale output frame stride");
	}

	{
		// Single-slope rescale should materialize a packed float buffer.
		const std::int16_t source_values[] = {-2, -1, 0, 1, 2, 3};
		const auto source_bytes =
		    to_byte_buffer<std::int16_t>(std::span<const std::int16_t>(source_values));
		const dicom::pixel::PixelLayout source_layout{
		    .data_type = dicom::pixel::DataType::s16,
		    .photometric = dicom::pixel::Photometric::monochrome2,
		    .planar = dicom::pixel::Planar::interleaved,
		    .reserved = 0,
		    .rows = 2,
		    .cols = 3,
		    .frames = 1,
		    .samples_per_pixel = 1,
		    .bits_stored = 16,
		    .row_stride = 3 * sizeof(std::int16_t),
		    .frame_stride = 2 * 3 * sizeof(std::int16_t),
		};

		const auto dst = dicom::pixel::apply_rescale(
		    dicom::pixel::ConstPixelSpan{
		        .layout = source_layout,
		        .bytes = std::span<const std::uint8_t>(source_bytes),
		    },
		    2.0f, 0.5f);

		expect_eq(dst.layout.data_type, dicom::pixel::DataType::f32,
		    "apply_rescale output dtype");
		const auto dst_values = from_byte_buffer<float>(dst.bytes);
		const double expected_values[] = {-3.5, -1.5, 0.5, 2.5, 4.5, 6.5};
		for (std::size_t index = 0; index < dst_values.size(); ++index) {
			expect_near(dst_values[index], expected_values[index], 1e-6,
			    "apply_rescale sample");
		}
	}

	{
		// Frame-specific rescale should use per-frame slope/intercept pairs.
		const std::uint16_t source_values[] = {1, 2, 3, 4, 10, 11, 12, 13};
		auto source_bytes =
		    to_byte_buffer<std::uint16_t>(std::span<const std::uint16_t>(source_values));
		const dicom::pixel::PixelLayout source_layout{
		    .data_type = dicom::pixel::DataType::u16,
		    .photometric = dicom::pixel::Photometric::monochrome2,
		    .planar = dicom::pixel::Planar::interleaved,
		    .reserved = 0,
		    .rows = 2,
		    .cols = 2,
		    .frames = 2,
		    .samples_per_pixel = 1,
		    .bits_stored = 16,
		    .row_stride = 2 * sizeof(std::uint16_t),
		    .frame_stride = 2 * 2 * sizeof(std::uint16_t),
		};
		const auto dst_layout =
		    dicom::pixel::make_rescale_output_layout(source_layout);
		std::vector<std::uint8_t> dst_bytes(
		    dst_layout.frames * dst_layout.frame_stride, std::uint8_t{0});
		const float slopes[] = {2.0f, 0.5f};
		const float intercepts[] = {-1.0f, 1.0f};

		dicom::pixel::apply_rescale_frames_into(
		    dicom::pixel::ConstPixelSpan{
		        .layout = source_layout,
		        .bytes = std::span<const std::uint8_t>(source_bytes),
		    },
		    dicom::pixel::PixelSpan{
		        .layout = dst_layout,
		        .bytes = std::span<std::uint8_t>(dst_bytes),
		    },
		    std::span<const float>(slopes), std::span<const float>(intercepts));

		const auto dst_values =
		    from_byte_buffer<float>(std::span<const std::uint8_t>(dst_bytes));
		const double expected_values[] = {1.0, 3.0, 5.0, 7.0, 6.0, 6.5, 7.0, 7.5};
		for (std::size_t index = 0; index < dst_values.size(); ++index) {
			expect_near(dst_values[index], expected_values[index], 1e-6,
			    "apply_rescale_frames_into sample");
		}
	}

	{
		// Rescale should reject multi-sample pixel layouts because DICOM rescale is monochrome.
		const std::uint8_t rgb_values[] = {1, 2, 3, 4, 5, 6};
		const auto source_bytes =
		    to_byte_buffer<std::uint8_t>(std::span<const std::uint8_t>(rgb_values));
		const dicom::pixel::PixelLayout source_layout{
		    .data_type = dicom::pixel::DataType::u8,
		    .photometric = dicom::pixel::Photometric::rgb,
		    .planar = dicom::pixel::Planar::interleaved,
		    .reserved = 0,
		    .rows = 1,
		    .cols = 2,
		    .frames = 1,
		    .samples_per_pixel = 3,
		    .bits_stored = 8,
		    .row_stride = 2 * 3 * sizeof(std::uint8_t),
		    .frame_stride = 2 * 3 * sizeof(std::uint8_t),
		};
		const auto dst_layout = dicom::pixel::make_rescale_output_layout(
		    source_layout.single_frame().with_samples(
		        std::uint16_t{1}, dicom::pixel::Photometric::monochrome2,
		        dicom::pixel::Planar::interleaved));
		std::vector<std::uint8_t> dst_bytes(dst_layout.frame_stride, std::uint8_t{0});

		bool threw = false;
		try {
			dicom::pixel::apply_rescale_into(
			    dicom::pixel::ConstPixelSpan{
			        .layout = source_layout,
			        .bytes = std::span<const std::uint8_t>(source_bytes),
			    },
			    dicom::pixel::PixelSpan{
			        .layout = dst_layout,
			        .bytes = std::span<std::uint8_t>(dst_bytes),
			    },
			    1.0f, 0.0f);
		} catch (const std::invalid_argument&) {
			threw = true;
		}
		expect_true(threw, "apply_rescale_into should reject spp != 1");
	}

	{
		// Window output layout should preserve geometry while normalizing to packed uint8 storage.
		const auto window_layout = dicom::pixel::make_window_output_layout(packed_u16_layout);
		expect_eq(window_layout.data_type, dicom::pixel::DataType::u8,
		    "window output dtype");
		expect_eq(window_layout.bits_stored, std::uint16_t{8},
		    "window output bits stored");
		expect_eq(window_layout.rows, packed_u16_layout.rows, "window output rows");
		expect_eq(window_layout.cols, packed_u16_layout.cols, "window output cols");
		expect_eq(window_layout.frames, packed_u16_layout.frames, "window output frames");
		expect_eq(window_layout.row_stride, std::size_t{3},
		    "window output row stride");
		expect_eq(window_layout.frame_stride, std::size_t{6},
		    "window output frame stride");
	}

	{
		// Window center/width should map stored values into display-ready uint8 samples.
		const std::int16_t source_values[] = {-1024, 0, 400};
		const auto source_bytes =
		    to_byte_buffer<std::int16_t>(std::span<const std::int16_t>(source_values));
		const dicom::pixel::PixelLayout source_layout{
		    .data_type = dicom::pixel::DataType::s16,
		    .photometric = dicom::pixel::Photometric::monochrome2,
		    .planar = dicom::pixel::Planar::interleaved,
		    .reserved = 0,
		    .rows = 1,
		    .cols = 3,
		    .frames = 1,
		    .samples_per_pixel = 1,
		    .bits_stored = 16,
		    .row_stride = 3 * sizeof(std::int16_t),
		    .frame_stride = 3 * sizeof(std::int16_t),
		};
		const auto dst = dicom::pixel::apply_window(
		    dicom::pixel::ConstPixelSpan{
		        .layout = source_layout,
		        .bytes = std::span<const std::uint8_t>(source_bytes),
		    },
		    dicom::pixel::WindowTransform{
		        .center = 0.0f,
		        .width = 400.0f,
		        .function = dicom::pixel::VoiLutFunction::linear,
		    });
		const auto dst_values = from_byte_buffer<std::uint8_t>(dst.bytes);
		const std::uint8_t expected_values[] = {0, 127, 255};
		for (std::size_t index = 0; index < dst_values.size(); ++index) {
			expect_eq(dst_values[index], expected_values[index],
			    "apply_window sample");
		}
	}

	{
		// Modality LUT should clamp samples below/above the descriptor range.
		const std::int16_t source_values[] = {8, 10, 11, 20};
		const auto source_bytes =
		    to_byte_buffer<std::int16_t>(std::span<const std::int16_t>(source_values));
		const dicom::pixel::PixelLayout source_layout{
		    .data_type = dicom::pixel::DataType::s16,
		    .photometric = dicom::pixel::Photometric::monochrome2,
		    .planar = dicom::pixel::Planar::interleaved,
		    .reserved = 0,
		    .rows = 2,
		    .cols = 2,
		    .frames = 1,
		    .samples_per_pixel = 1,
		    .bits_stored = 16,
		    .row_stride = 2 * sizeof(std::int16_t),
		    .frame_stride = 2 * 2 * sizeof(std::int16_t),
		};
		dicom::pixel::ModalityLut modality_lut{};
		modality_lut.first_mapped = 10;
		modality_lut.values = {100.0f, 101.0f, 102.0f};

		const auto dst = dicom::pixel::apply_modality_lut(
		    dicom::pixel::ConstPixelSpan{
		        .layout = source_layout,
		        .bytes = std::span<const std::uint8_t>(source_bytes),
		    },
		    modality_lut);
		const auto dst_values = from_byte_buffer<float>(dst.bytes);
		const double expected_values[] = {100.0, 100.0, 101.0, 102.0};
		for (std::size_t index = 0; index < dst_values.size(); ++index) {
			expect_near(dst_values[index], expected_values[index], 1e-6,
			    "apply_modality_lut sample");
		}
	}

	{
		// VOI LUT should clamp samples below/above the descriptor range and pick uint output width.
		const std::int16_t source_values[] = {0, 1, 2, 9};
		const auto source_bytes =
		    to_byte_buffer<std::int16_t>(std::span<const std::int16_t>(source_values));
		const dicom::pixel::PixelLayout source_layout{
		    .data_type = dicom::pixel::DataType::s16,
		    .photometric = dicom::pixel::Photometric::monochrome2,
		    .planar = dicom::pixel::Planar::interleaved,
		    .reserved = 0,
		    .rows = 2,
		    .cols = 2,
		    .frames = 1,
		    .samples_per_pixel = 1,
		    .bits_stored = 16,
		    .row_stride = 2 * sizeof(std::int16_t),
		    .frame_stride = 2 * 2 * sizeof(std::int16_t),
		};
		dicom::pixel::VoiLut voi_lut{};
		voi_lut.first_mapped = 1;
		voi_lut.bits_per_entry = 8;
		voi_lut.values = {5, 10, 15};

		const auto voi_layout =
		    dicom::pixel::make_voi_lut_output_layout(source_layout, voi_lut);
		expect_eq(voi_layout.data_type, dicom::pixel::DataType::u8,
		    "VOI LUT output dtype");
		expect_eq(voi_layout.bits_stored, std::uint16_t{8},
		    "VOI LUT output bits stored");

		const auto dst = dicom::pixel::apply_voi_lut(
		    dicom::pixel::ConstPixelSpan{
		        .layout = source_layout,
		        .bytes = std::span<const std::uint8_t>(source_bytes),
		    },
		    voi_lut);
		const auto dst_values = from_byte_buffer<std::uint8_t>(dst.bytes);
		const std::uint8_t expected_values[] = {5, 5, 10, 15};
		for (std::size_t index = 0; index < dst_values.size(); ++index) {
			expect_eq(dst_values[index], expected_values[index],
			    "apply_voi_lut sample");
		}
	}

	{
		// Palette LUT should expand indexed source pixels into RGB samples.
		const std::uint8_t source_values[] = {0, 1, 2, 3};
		const auto source_bytes =
		    to_byte_buffer<std::uint8_t>(std::span<const std::uint8_t>(source_values));
		const dicom::pixel::PixelLayout source_layout{
		    .data_type = dicom::pixel::DataType::u8,
		    .photometric = dicom::pixel::Photometric::palette_color,
		    .planar = dicom::pixel::Planar::interleaved,
		    .reserved = 0,
		    .rows = 2,
		    .cols = 2,
		    .frames = 1,
		    .samples_per_pixel = 1,
		    .bits_stored = 8,
		    .row_stride = 2 * sizeof(std::uint8_t),
		    .frame_stride = 2 * 2 * sizeof(std::uint8_t),
		};
		dicom::pixel::PaletteLut palette_lut{};
		palette_lut.first_mapped = 0;
		palette_lut.bits_per_entry = 8;
		palette_lut.red_values = {10, 20, 30, 40};
		palette_lut.green_values = {1, 2, 3, 4};
		palette_lut.blue_values = {100, 110, 120, 130};

		const auto dst = dicom::pixel::apply_palette_lut(
		    dicom::pixel::ConstPixelSpan{
		        .layout = source_layout,
		        .bytes = std::span<const std::uint8_t>(source_bytes),
		    },
		    palette_lut);
		expect_eq(dst.layout.data_type, dicom::pixel::DataType::u8,
		    "apply_palette_lut output dtype");
		expect_eq(dst.layout.samples_per_pixel, std::uint16_t{3},
		    "apply_palette_lut output spp");
		expect_eq(dst.layout.photometric, dicom::pixel::Photometric::rgb,
		    "apply_palette_lut output photometric");
		const auto dst_values = from_byte_buffer<std::uint8_t>(dst.bytes);
		const std::uint8_t expected_values[] = {
		    10, 1, 100, 20, 2, 110, 30, 3, 120, 40, 4, 130};
		for (std::size_t index = 0; index < dst_values.size(); ++index) {
			expect_eq(dst_values[index], expected_values[index],
			    "apply_palette_lut sample");
		}
	}

	{
		// Palette LUT with alpha should expand indexed values into RGBA samples.
		const std::uint8_t source_values[] = {0, 1, 2, 3};
		const auto source_bytes =
		    to_byte_buffer<std::uint8_t>(std::span<const std::uint8_t>(source_values));
		const dicom::pixel::PixelLayout source_layout{
		    .data_type = dicom::pixel::DataType::u8,
		    .photometric = dicom::pixel::Photometric::palette_color,
		    .planar = dicom::pixel::Planar::interleaved,
		    .reserved = 0,
		    .rows = 2,
		    .cols = 2,
		    .frames = 1,
		    .samples_per_pixel = 1,
		    .bits_stored = 8,
		    .row_stride = 2 * sizeof(std::uint8_t),
		    .frame_stride = 2 * 2 * sizeof(std::uint8_t),
		};
		dicom::pixel::PaletteLut palette_lut{};
		palette_lut.first_mapped = 0;
		palette_lut.bits_per_entry = 8;
		palette_lut.red_values = {10, 20, 30, 40};
		palette_lut.green_values = {1, 2, 3, 4};
		palette_lut.blue_values = {100, 110, 120, 130};
		palette_lut.alpha_values = {200, 210, 220, 230};

		const auto dst = dicom::pixel::apply_palette_lut(
		    dicom::pixel::ConstPixelSpan{
		        .layout = source_layout,
		        .bytes = std::span<const std::uint8_t>(source_bytes),
		    },
		    palette_lut);
		expect_eq(dst.layout.data_type, dicom::pixel::DataType::u8,
		    "apply_palette_lut alpha output dtype");
		expect_eq(dst.layout.samples_per_pixel, std::uint16_t{4},
		    "apply_palette_lut alpha output spp");
		expect_eq(dst.layout.photometric, dicom::pixel::Photometric::rgb,
		    "apply_palette_lut alpha output photometric");
		const auto dst_values = from_byte_buffer<std::uint8_t>(dst.bytes);
		const std::uint8_t expected_values[] = {
		    10, 1, 100, 200, 20, 2, 110, 210,
		    30, 3, 120, 220, 40, 4, 130, 230};
		for (std::size_t index = 0; index < dst_values.size(); ++index) {
			expect_eq(dst_values[index], expected_values[index],
			    "apply_palette_lut alpha sample");
		}
	}

	{
		// VOI LUT metadata should expose the first VOILUTSequence item.
		dicom::DicomFile file{};
		expect_true(!file.voi_lut().has_value(),
		    "empty file should not expose VOI LUT");
		auto& seq_elem = file.add_dataelement("VOILUTSequence"_tag, dicom::VR::SQ);
		auto* seq = seq_elem.as_sequence();
		expect_true(seq != nullptr, "VOILUTSequence should create sequence storage");
		auto* item = seq->add_dataset();
		expect_true(item != nullptr, "VOILUTSequence item should be created");
		const std::array<long, 3> descriptor_values{4, 0, 8};
		expect_true(item->set_value(
		                "LUTDescriptor"_tag, dicom::VR::US,
		                std::span<const long>(descriptor_values)),
		    "VOI LUT descriptor write");
		item->add_dataelement("LUTData"_tag, dicom::VR::OW)
		    .set_value_bytes(std::vector<std::uint8_t>{9, 8, 7, 6});

		const auto lut = file.voi_lut();
		expect_true(lut.has_value(), "VOI LUT should be present");
		expect_eq(lut->first_mapped, std::int64_t{0}, "VOI LUT first mapped");
		expect_eq(lut->bits_per_entry, std::uint16_t{8}, "VOI LUT bits");
		expect_eq(lut->values.size(), std::size_t{4}, "VOI LUT entry count");
		expect_eq(lut->values[0], std::uint16_t{9}, "VOI LUT value[0]");
		expect_eq(lut->values[3], std::uint16_t{6}, "VOI LUT value[3]");
	}

	{
		// Window metadata should expose the first center/width pair and VOI LUT function.
		dicom::DicomFile file{};
		expect_true(!file.window_transform().has_value(),
		    "empty file should not expose window transform");
		expect_true(file.add_dataelement("WindowCenter"_tag, dicom::VR::DS).from_double(40.0),
		    "WindowCenter write");
		expect_true(file.add_dataelement("WindowWidth"_tag, dicom::VR::DS).from_double(400.0),
		    "WindowWidth write");
		expect_true(file.add_dataelement("VOILUTFunction"_tag, dicom::VR::CS)
		                .from_string_view("SIGMOID"),
		    "VOILUTFunction write");
		const auto tx = file.window_transform();
		expect_true(tx.has_value(), "window transform should be present");
		expect_near(tx->center, 40.0, 1e-6, "window center");
		expect_near(tx->width, 400.0, 1e-6, "window width");
		expect_eq(tx->function, dicom::pixel::VoiLutFunction::sigmoid,
		    "window function");
	}

	{
		// Rescale metadata should be exposed independently from pixel decode.
		dicom::DicomFile file{};
		expect_true(!file.rescale_transform().has_value(),
		    "empty file should not expose rescale transform");
		expect_true(file.add_dataelement("RescaleSlope"_tag, dicom::VR::DS).from_double(2.5),
		    "RescaleSlope write");
		expect_true(
		    file.add_dataelement("RescaleIntercept"_tag, dicom::VR::DS).from_double(-1.25),
		    "RescaleIntercept write");
		const auto tx = file.rescale_transform();
		expect_true(tx.has_value(), "rescale transform should be present");
		expect_near(tx->slope, 2.5, 1e-6, "rescale slope");
		expect_near(tx->intercept, -1.25, 1e-6, "rescale intercept");
	}

	{
		// Per-frame pixel value transforms should override shared entries, then root fallback.
		dicom::DicomFile file{};
		expect_true(file.add_dataelement("NumberOfFrames"_tag, dicom::VR::IS).from_long(2),
		    "NumberOfFrames write");
		expect_true(file.add_dataelement("RescaleSlope"_tag, dicom::VR::DS).from_double(10.0),
		    "root RescaleSlope write");
		expect_true(file.add_dataelement("RescaleIntercept"_tag, dicom::VR::DS).from_double(100.0),
		    "root RescaleIntercept write");
		auto* shared_fg = add_single_item_sequence(
		    file, "SharedFunctionalGroupsSequence"_tag, "Shared Functional Groups");
		auto* shared_tx = add_single_item_sequence(
		    *shared_fg, "PixelValueTransformationSequence"_tag, "Shared Pixel Value Transformation");
		expect_true(shared_tx->add_dataelement("RescaleSlope"_tag, dicom::VR::DS).from_double(2.0),
		    "shared RescaleSlope write");
		expect_true(shared_tx->add_dataelement("RescaleIntercept"_tag, dicom::VR::DS).from_double(20.0),
		    "shared RescaleIntercept write");

		auto& per_frame_elem =
		    file.add_dataelement("PerFrameFunctionalGroupsSequence"_tag, dicom::VR::SQ);
		auto* per_frame_seq = per_frame_elem.as_sequence();
		expect_true(per_frame_seq != nullptr,
		    "Per-Frame Functional Groups should create sequence storage");
		auto* frame0_fg = per_frame_seq->add_dataset();
		auto* frame1_fg = per_frame_seq->add_dataset();
		expect_true(frame0_fg != nullptr && frame1_fg != nullptr,
		    "Per-Frame Functional Groups should create two items");
		auto* frame0_tx = add_single_item_sequence(
		    *frame0_fg, "PixelValueTransformationSequence"_tag,
		    "Per-Frame Pixel Value Transformation");
		expect_true(frame0_tx->add_dataelement("RescaleSlope"_tag, dicom::VR::DS).from_double(3.0),
		    "frame0 RescaleSlope write");
		expect_true(frame0_tx->add_dataelement("RescaleIntercept"_tag, dicom::VR::DS).from_double(30.0),
		    "frame0 RescaleIntercept write");

		const auto tx0 = file.rescale_transform(0);
		const auto tx1 = file.rescale_transform(1);
		const auto default_tx = file.rescale_transform();
		expect_true(tx0.has_value(), "frame0 rescale transform should be present");
		expect_true(tx1.has_value(), "frame1 rescale transform should be present");
		expect_true(default_tx.has_value(), "default rescale transform should be present");
		expect_near(tx0->slope, 3.0, 1e-6, "frame0 rescale slope");
		expect_near(tx0->intercept, 30.0, 1e-6, "frame0 rescale intercept");
		expect_near(tx1->slope, 2.0, 1e-6, "frame1 rescale slope");
		expect_near(tx1->intercept, 20.0, 1e-6, "frame1 rescale intercept");
		expect_near(default_tx->slope, 3.0, 1e-6, "default rescale slope");
		expect_near(default_tx->intercept, 30.0, 1e-6, "default rescale intercept");
	}

	{
		// Palette LUT metadata should round-trip from the root dataset.
		dicom::DicomFile file{};
		expect_true(!file.palette_lut().has_value(),
		    "empty file should not expose palette LUT");
		const std::array<long, 3> descriptor_values{4, 0, 8};
		expect_true(file.set_value(
		                "RedPaletteColorLookupTableDescriptor"_tag, dicom::VR::US,
		                std::span<const long>(descriptor_values)),
		    "Red Palette LUT descriptor write");
		expect_true(file.set_value(
		                "GreenPaletteColorLookupTableDescriptor"_tag, dicom::VR::US,
		                std::span<const long>(descriptor_values)),
		    "Green Palette LUT descriptor write");
		expect_true(file.set_value(
		                "BluePaletteColorLookupTableDescriptor"_tag, dicom::VR::US,
		                std::span<const long>(descriptor_values)),
		    "Blue Palette LUT descriptor write");
		file.add_dataelement("RedPaletteColorLookupTableData"_tag, dicom::VR::OW)
		    .set_value_bytes(std::vector<std::uint8_t>{10, 20, 30, 40});
		file.add_dataelement("GreenPaletteColorLookupTableData"_tag, dicom::VR::OW)
		    .set_value_bytes(std::vector<std::uint8_t>{1, 2, 3, 4});
		file.add_dataelement("BluePaletteColorLookupTableData"_tag, dicom::VR::OW)
		    .set_value_bytes(std::vector<std::uint8_t>{100, 110, 120, 130});

		const auto lut = file.palette_lut();
		expect_true(lut.has_value(), "palette LUT should be present");
		expect_eq(lut->first_mapped, std::int64_t{0}, "palette LUT first mapped");
		expect_eq(lut->bits_per_entry, std::uint16_t{8}, "palette LUT bits");
		expect_eq(lut->red_values.size(), std::size_t{4}, "palette LUT entry count");
		expect_eq(lut->red_values[0], std::uint16_t{10}, "palette LUT red[0]");
		expect_eq(lut->green_values[1], std::uint16_t{2}, "palette LUT green[1]");
		expect_eq(lut->blue_values[3], std::uint16_t{130}, "palette LUT blue[3]");
	}

	{
		// Segmented palette LUT metadata should expand discrete, linear, and
		// indirect segments into the same classic PaletteLut model.
		dicom::DicomFile file{};
		const std::array<long, 3> descriptor_values{8, 0, 8};
		expect_true(file.set_value(
		                "RedPaletteColorLookupTableDescriptor"_tag, dicom::VR::US,
		                std::span<const long>(descriptor_values)),
		    "Segmented Red Palette LUT descriptor write");
		expect_true(file.set_value(
		                "GreenPaletteColorLookupTableDescriptor"_tag, dicom::VR::US,
		                std::span<const long>(descriptor_values)),
		    "Segmented Green Palette LUT descriptor write");
		expect_true(file.set_value(
		                "BluePaletteColorLookupTableDescriptor"_tag, dicom::VR::US,
		                std::span<const long>(descriptor_values)),
		    "Segmented Blue Palette LUT descriptor write");
		file.add_dataelement("SegmentedRedPaletteColorLookupTableData"_tag, dicom::VR::OW)
		    .set_value_bytes(std::vector<std::uint8_t>{0, 1, 10, 1, 3, 40, 2, 2, 0, 0, 0, 0});
		file.add_dataelement("SegmentedGreenPaletteColorLookupTableData"_tag, dicom::VR::OW)
		    .set_value_bytes(std::vector<std::uint8_t>{0, 1, 1, 1, 3, 4, 2, 2, 0, 0, 0, 0});
		file.add_dataelement("SegmentedBluePaletteColorLookupTableData"_tag, dicom::VR::OW)
		    .set_value_bytes(
		        std::vector<std::uint8_t>{0, 1, 100, 1, 3, 130, 2, 2, 0, 0, 0, 0});

		const auto lut = file.palette_lut();
		expect_true(lut.has_value(), "segmented palette LUT should be present");
		expect_eq(lut->first_mapped, std::int64_t{0}, "segmented palette LUT first mapped");
		expect_eq(lut->bits_per_entry, std::uint16_t{8}, "segmented palette LUT bits");
		expect_eq(lut->red_values.size(), std::size_t{8}, "segmented palette LUT entry count");
		expect_eq(lut->red_values[0], std::uint16_t{10}, "segmented palette LUT red[0]");
		expect_eq(lut->red_values[3], std::uint16_t{40}, "segmented palette LUT red[3]");
		expect_eq(lut->red_values[7], std::uint16_t{40}, "segmented palette LUT red[7]");
		expect_eq(lut->green_values[2], std::uint16_t{3}, "segmented palette LUT green[2]");
		expect_eq(lut->green_values[6], std::uint16_t{3}, "segmented palette LUT green[6]");
		expect_eq(lut->blue_values[1], std::uint16_t{110}, "segmented palette LUT blue[1]");
		expect_eq(lut->blue_values[5], std::uint16_t{110}, "segmented palette LUT blue[5]");
	}

	{
		// Supplemental palette metadata should stay separate from the classic
		// PALETTE COLOR accessor while still exposing the same root-level LUT payload.
		dicom::DicomFile file{};
		expect_true(
		    file.add_dataelement("PhotometricInterpretation"_tag, dicom::VR::CS)
		        .from_string_view("MONOCHROME2"),
		    "supplemental palette photometric write");
		expect_true(
		    file.add_dataelement("PixelPresentation"_tag, dicom::VR::CS)
		        .from_string_view("COLOR"),
		    "supplemental palette PixelPresentation write");
		const std::array<long, 3> descriptor_values{4, 0, 8};
		expect_true(file.set_value(
		                "RedPaletteColorLookupTableDescriptor"_tag, dicom::VR::US,
		                std::span<const long>(descriptor_values)),
		    "supplemental Red Palette LUT descriptor write");
		expect_true(file.set_value(
		                "GreenPaletteColorLookupTableDescriptor"_tag, dicom::VR::US,
		                std::span<const long>(descriptor_values)),
		    "supplemental Green Palette LUT descriptor write");
		expect_true(file.set_value(
		                "BluePaletteColorLookupTableDescriptor"_tag, dicom::VR::US,
		                std::span<const long>(descriptor_values)),
		    "supplemental Blue Palette LUT descriptor write");
		expect_true(file.set_value(
		                "AlphaPaletteColorLookupTableDescriptor"_tag, dicom::VR::US,
		                std::span<const long>(descriptor_values)),
		    "supplemental Alpha Palette LUT descriptor write");
		file.add_dataelement("RedPaletteColorLookupTableData"_tag, dicom::VR::OW)
		    .set_value_bytes(std::vector<std::uint8_t>{10, 20, 30, 40});
		file.add_dataelement("GreenPaletteColorLookupTableData"_tag, dicom::VR::OW)
		    .set_value_bytes(std::vector<std::uint8_t>{1, 2, 3, 4});
		file.add_dataelement("BluePaletteColorLookupTableData"_tag, dicom::VR::OW)
		    .set_value_bytes(std::vector<std::uint8_t>{100, 110, 120, 130});
		file.add_dataelement("AlphaPaletteColorLookupTableData"_tag, dicom::VR::OW)
		    .set_value_bytes(std::vector<std::uint8_t>{255, 200, 150, 100});
		auto* range_item = add_single_item_sequence(
		    file, "StoredValueColorRangeSequence"_tag, "Stored Value Color Range Sequence");
		expect_true(range_item->add_dataelement("MinimumStoredValueMapped"_tag, dicom::VR::FD)
		                .from_double(10.0),
		    "supplemental minimum stored value mapped write");
		expect_true(range_item->add_dataelement("MaximumStoredValueMapped"_tag, dicom::VR::FD)
		                .from_double(4095.0),
		    "supplemental maximum stored value mapped write");

		expect_true(!file.palette_lut().has_value(),
		    "classic palette accessor should not fold in supplemental palette metadata");
		const auto supplemental = file.supplemental_palette();
		expect_true(supplemental.has_value(), "supplemental palette metadata should be present");
		expect_eq(supplemental->pixel_presentation, dicom::pixel::PixelPresentation::color,
		    "supplemental palette PixelPresentation");
		expect_true(supplemental->has_stored_value_range,
		    "supplemental palette stored value range should be present");
		expect_eq(supplemental->palette.red_values[0], std::uint16_t{10},
		    "supplemental palette red[0]");
		expect_eq(supplemental->palette.blue_values[3], std::uint16_t{130},
		    "supplemental palette blue[3]");
		expect_eq(supplemental->palette.alpha_values[2], std::uint16_t{150},
		    "supplemental palette alpha[2]");
	}

	{
		// Enhanced palette metadata should remain a separate display-pipeline model.
		dicom::DicomFile file{};
		expect_true(
		    file.add_dataelement("PixelPresentation"_tag, dicom::VR::CS)
		        .from_string_view("COLOR"),
		    "enhanced palette PixelPresentation write");
		auto* assignment_item = add_single_item_sequence(
		    file, "DataFrameAssignmentSequence"_tag, "Data Frame Assignment Sequence");
		expect_true(assignment_item->add_dataelement("DataType"_tag, dicom::VR::CS)
		                .from_string_view("TISSUE"),
		    "enhanced palette DataType write");
		expect_true(assignment_item->add_dataelement("DataPathAssignment"_tag, dicom::VR::CS)
		                .from_string_view("PRIMARY_SINGLE"),
		    "enhanced palette DataPathAssignment write");
		expect_true(assignment_item->add_dataelement("BitsMappedToColorLookupTable"_tag, dicom::VR::US)
		                .from_int(12),
		    "enhanced palette BitsMappedToColorLookupTable write");
		auto* blending_item = add_single_item_sequence(
		    file, "BlendingLUT1Sequence"_tag, "Blending LUT 1 Sequence");
		expect_true(blending_item->add_dataelement("BlendingLUT1TransferFunction"_tag, dicom::VR::CS)
		                .from_string_view("CONSTANT"),
		    "enhanced palette blending transfer function write");
		expect_true(blending_item->add_dataelement("BlendingWeightConstant"_tag, dicom::VR::FD)
		                .from_double(0.75),
		    "enhanced palette blending weight constant write");
		auto* palette_item = add_single_item_sequence(
		    file, "EnhancedPaletteColorLookupTableSequence"_tag,
		    "Enhanced Palette Color Lookup Table Sequence");
		expect_true(palette_item->add_dataelement("DataPathID"_tag, dicom::VR::CS)
		                .from_string_view("PRIMARY"),
		    "enhanced palette DataPathID write");
		expect_true(palette_item->add_dataelement("RGBLUTTransferFunction"_tag, dicom::VR::CS)
		                .from_string_view("IDENTITY"),
		    "enhanced palette RGB transfer function write");
		const std::array<long, 3> descriptor_values{2, 0, 8};
		expect_true(palette_item->set_value(
		                "RedPaletteColorLookupTableDescriptor"_tag, dicom::VR::US,
		                std::span<const long>(descriptor_values)),
		    "enhanced palette red descriptor write");
		expect_true(palette_item->set_value(
		                "GreenPaletteColorLookupTableDescriptor"_tag, dicom::VR::US,
		                std::span<const long>(descriptor_values)),
		    "enhanced palette green descriptor write");
		expect_true(palette_item->set_value(
		                "BluePaletteColorLookupTableDescriptor"_tag, dicom::VR::US,
		                std::span<const long>(descriptor_values)),
		    "enhanced palette blue descriptor write");
		expect_true(palette_item->set_value(
		                "AlphaPaletteColorLookupTableDescriptor"_tag, dicom::VR::US,
		                std::span<const long>(descriptor_values)),
		    "enhanced palette alpha descriptor write");
		palette_item->add_dataelement("RedPaletteColorLookupTableData"_tag, dicom::VR::OW)
		    .set_value_bytes(std::vector<std::uint8_t>{9, 19});
		palette_item->add_dataelement("GreenPaletteColorLookupTableData"_tag, dicom::VR::OW)
		    .set_value_bytes(std::vector<std::uint8_t>{29, 39});
		palette_item->add_dataelement("BluePaletteColorLookupTableData"_tag, dicom::VR::OW)
		    .set_value_bytes(std::vector<std::uint8_t>{49, 59});
		palette_item->add_dataelement("AlphaPaletteColorLookupTableData"_tag, dicom::VR::OW)
		    .set_value_bytes(std::vector<std::uint8_t>{69, 79});
		expect_true(file.add_dataelement("ColorSpace"_tag, dicom::VR::CS)
		                .from_string_view("SRGB"),
		    "enhanced palette ColorSpace write");

		expect_true(!file.palette_lut().has_value(),
		    "classic palette accessor should not fold in enhanced palette metadata");
		const auto enhanced = file.enhanced_palette();
		expect_true(enhanced.has_value(), "enhanced palette metadata should be present");
		expect_eq(enhanced->pixel_presentation, dicom::pixel::PixelPresentation::color,
		    "enhanced palette PixelPresentation");
		expect_eq(enhanced->data_frame_assignments.size(), std::size_t{1},
		    "enhanced palette assignment count");
		expect_eq(enhanced->data_frame_assignments[0].data_path_assignment,
		    std::string{"PRIMARY_SINGLE"},
		    "enhanced palette DataPathAssignment");
		expect_true(enhanced->has_blending_lut_1,
		    "enhanced palette Blending LUT 1 should be present");
		expect_eq(enhanced->palette_items.size(), std::size_t{1},
		    "enhanced palette item count");
		expect_eq(enhanced->palette_items[0].data_path_id, std::string{"PRIMARY"},
		    "enhanced palette item DataPathID");
		expect_eq(enhanced->palette_items[0].palette.red_values[1], std::uint16_t{19},
		    "enhanced palette item red[1]");
		expect_eq(enhanced->palette_items[0].palette.alpha_values[1], std::uint16_t{79},
		    "enhanced palette item alpha[1]");
	}

	{
		// Root ModalityLUTSequence is shared across frames; frame-specific
		// modality-equivalent transforms are represented by rescale metadata.
		dicom::DicomFile file{};
		expect_true(file.add_dataelement("NumberOfFrames"_tag, dicom::VR::IS).from_long(2),
		    "modality LUT NumberOfFrames write");
		auto* item = add_single_item_sequence(
		    file, "ModalityLUTSequence"_tag, "Modality LUT Sequence");
		const std::array<long, 3> descriptor_values{3, 10, 8};
		expect_true(item->set_value(
		                "LUTDescriptor"_tag, dicom::VR::US,
		                std::span<const long>(descriptor_values)),
		    "Modality LUT descriptor write");
		item->add_dataelement("LUTData"_tag, dicom::VR::OW)
		    .set_value_bytes(std::vector<std::uint8_t>{11, 22, 33});

		const auto lut0 = file.modality_lut(0);
		const auto lut1 = file.modality_lut(1);
		const auto default_lut = file.modality_lut();
		expect_true(lut0.has_value(), "frame0 modality LUT should be present");
		expect_true(lut1.has_value(), "frame1 modality LUT should be present");
		expect_true(default_lut.has_value(), "default modality LUT should be present");
		expect_eq(lut0->first_mapped, std::int64_t{10}, "frame0 modality LUT first mapped");
		expect_eq(lut0->values[0], 11.0f, "frame0 modality LUT value[0]");
		expect_eq(lut1->values[2], 33.0f, "frame1 modality LUT value[2]");
		expect_eq(default_lut->values[1], 22.0f, "default modality LUT value[1]");
	}

	{
		// Frame VOI LUT window metadata should override shared entries, then root fallback.
		dicom::DicomFile file{};
		expect_true(file.add_dataelement("NumberOfFrames"_tag, dicom::VR::IS).from_long(2),
		    "window NumberOfFrames write");
		expect_true(file.add_dataelement("WindowCenter"_tag, dicom::VR::DS).from_double(10.0),
		    "root WindowCenter write");
		expect_true(file.add_dataelement("WindowWidth"_tag, dicom::VR::DS).from_double(100.0),
		    "root WindowWidth write");

		auto* shared_fg = add_single_item_sequence(
		    file, "SharedFunctionalGroupsSequence"_tag, "Shared Functional Groups");
		auto* shared_voi = add_single_item_sequence(
		    *shared_fg, "FrameVOILUTSequence"_tag, "Shared Frame VOI LUT");
		expect_true(shared_voi->add_dataelement("WindowCenter"_tag, dicom::VR::DS).from_double(20.0),
		    "shared WindowCenter write");
		expect_true(shared_voi->add_dataelement("WindowWidth"_tag, dicom::VR::DS).from_double(200.0),
		    "shared WindowWidth write");
		expect_true(shared_voi->add_dataelement("VOILUTFunction"_tag, dicom::VR::CS)
		                .from_string_view("SIGMOID"),
		    "shared VOILUTFunction write");

		auto& per_frame_elem =
		    file.add_dataelement("PerFrameFunctionalGroupsSequence"_tag, dicom::VR::SQ);
		auto* per_frame_seq = per_frame_elem.as_sequence();
		expect_true(per_frame_seq != nullptr,
		    "Per-Frame Functional Groups should create sequence storage");
		auto* frame0_fg = per_frame_seq->add_dataset();
		auto* frame1_fg = per_frame_seq->add_dataset();
		expect_true(frame0_fg != nullptr && frame1_fg != nullptr,
		    "Per-Frame Functional Groups should create two items");
		auto* frame0_voi = add_single_item_sequence(
		    *frame0_fg, "FrameVOILUTSequence"_tag, "Per-Frame VOI LUT");
		expect_true(frame0_voi->add_dataelement("WindowCenter"_tag, dicom::VR::DS).from_double(30.0),
		    "frame0 WindowCenter write");
		expect_true(frame0_voi->add_dataelement("WindowWidth"_tag, dicom::VR::DS).from_double(300.0),
		    "frame0 WindowWidth write");
		expect_true(frame0_voi->add_dataelement("VOILUTFunction"_tag, dicom::VR::CS)
		                .from_string_view("LINEAR_EXACT"),
		    "frame0 VOILUTFunction write");

		const auto tx0 = file.window_transform(0);
		const auto tx1 = file.window_transform(1);
		const auto default_tx = file.window_transform();
		expect_true(tx0.has_value(), "frame0 window transform should be present");
		expect_true(tx1.has_value(), "frame1 window transform should be present");
		expect_true(default_tx.has_value(), "default window transform should be present");
		expect_near(tx0->center, 30.0, 1e-6, "frame0 window center");
		expect_near(tx0->width, 300.0, 1e-6, "frame0 window width");
		expect_eq(tx0->function, dicom::pixel::VoiLutFunction::linear_exact,
		    "frame0 window function");
		expect_near(tx1->center, 20.0, 1e-6, "frame1 window center");
		expect_near(tx1->width, 200.0, 1e-6, "frame1 window width");
		expect_eq(tx1->function, dicom::pixel::VoiLutFunction::sigmoid,
		    "frame1 window function");
		expect_near(default_tx->center, 30.0, 1e-6, "default window center");
		expect_near(default_tx->width, 300.0, 1e-6, "default window width");
	}

	{
		// Frame VOI LUT entries should override shared entries, then root fallback.
		dicom::DicomFile file{};
		expect_true(file.add_dataelement("NumberOfFrames"_tag, dicom::VR::IS).from_long(2),
		    "VOI LUT NumberOfFrames write");
		auto* root_voi = add_single_item_sequence(file, "VOILUTSequence"_tag, "Root VOI LUT");
		const std::array<long, 3> root_descriptor{2, 0, 8};
		expect_true(root_voi->set_value("LUTDescriptor"_tag, dicom::VR::US, std::span<const long>(root_descriptor)),
		    "root LUTDescriptor write");
		root_voi->add_dataelement("LUTData"_tag, dicom::VR::OW)
		    .set_value_bytes(std::vector<std::uint8_t>{1, 2});

		auto* shared_fg = add_single_item_sequence(
		    file, "SharedFunctionalGroupsSequence"_tag, "Shared Functional Groups");
		auto* shared_voi = add_single_item_sequence(
		    *shared_fg, "FrameVOILUTSequence"_tag, "Shared Frame VOI LUT");
		auto* shared_lut = add_single_item_sequence(
		    *shared_voi, "VOILUTSequence"_tag, "Shared VOI LUT Sequence");
		const std::array<long, 3> shared_descriptor{2, 0, 8};
		expect_true(shared_lut->set_value("LUTDescriptor"_tag, dicom::VR::US, std::span<const long>(shared_descriptor)),
		    "shared LUTDescriptor write");
		shared_lut->add_dataelement("LUTData"_tag, dicom::VR::OW)
		    .set_value_bytes(std::vector<std::uint8_t>{3, 4});

		auto& per_frame_elem =
		    file.add_dataelement("PerFrameFunctionalGroupsSequence"_tag, dicom::VR::SQ);
		auto* per_frame_seq = per_frame_elem.as_sequence();
		expect_true(per_frame_seq != nullptr,
		    "Per-Frame Functional Groups should create sequence storage");
		auto* frame0_fg = per_frame_seq->add_dataset();
		auto* frame1_fg = per_frame_seq->add_dataset();
		expect_true(frame0_fg != nullptr && frame1_fg != nullptr,
		    "Per-Frame Functional Groups should create two items");
		auto* frame0_voi = add_single_item_sequence(
		    *frame0_fg, "FrameVOILUTSequence"_tag, "Per-Frame VOI LUT");
		auto* frame0_lut = add_single_item_sequence(
		    *frame0_voi, "VOILUTSequence"_tag, "Per-Frame VOI LUT Sequence");
		const std::array<long, 3> frame0_descriptor{2, 0, 8};
		expect_true(frame0_lut->set_value("LUTDescriptor"_tag, dicom::VR::US, std::span<const long>(frame0_descriptor)),
		    "frame0 LUTDescriptor write");
		frame0_lut->add_dataelement("LUTData"_tag, dicom::VR::OW)
		    .set_value_bytes(std::vector<std::uint8_t>{5, 6});

		const auto lut0 = file.voi_lut(0);
		const auto lut1 = file.voi_lut(1);
		const auto default_lut = file.voi_lut();
		expect_true(lut0.has_value(), "frame0 VOI LUT should be present");
		expect_true(lut1.has_value(), "frame1 VOI LUT should be present");
		expect_true(default_lut.has_value(), "default VOI LUT should be present");
		expect_eq(lut0->values[0], std::uint16_t{5}, "frame0 VOI LUT value[0]");
		expect_eq(lut0->values[1], std::uint16_t{6}, "frame0 VOI LUT value[1]");
		expect_eq(lut1->values[0], std::uint16_t{3}, "frame1 VOI LUT value[0]");
		expect_eq(lut1->values[1], std::uint16_t{4}, "frame1 VOI LUT value[1]");
		expect_eq(default_lut->values[0], std::uint16_t{5}, "default VOI LUT value[0]");
		expect_eq(default_lut->values[1], std::uint16_t{6}, "default VOI LUT value[1]");
	}

	return 0;
}
