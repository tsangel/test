#include "pixel/decode/core/decode_codec_impl_detail.hpp"

namespace dicom {
namespace pixel::detail {

PlanarTransform select_planar_transform(Planar src_planar, Planar dst_planar) noexcept {
	if (src_planar == Planar::interleaved) {
		return (dst_planar == Planar::interleaved)
		           ? PlanarTransform::interleaved_to_interleaved
		           : PlanarTransform::interleaved_to_planar;
	}
	return (dst_planar == Planar::interleaved)
	           ? PlanarTransform::planar_to_interleaved
	           : PlanarTransform::planar_to_planar;
}

std::size_t sv_dtype_bytes(DataType sv_dtype) noexcept {
	switch (sv_dtype) {
	case DataType::u8:
	case DataType::s8:
		return 1;
	case DataType::u16:
	case DataType::s16:
		return 2;
	case DataType::u32:
	case DataType::s32:
	case DataType::f32:
		return 4;
	case DataType::f64:
		return 8;
	default:
		return 0;
	}
}

}  // namespace pixel::detail
}  // namespace dicom

