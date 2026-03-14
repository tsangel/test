#pragma once

#include "serialization/detail/write_core.hpp"

namespace dicom::write_detail {

void write_with_transfer_syntax_to_stream_writer(DicomFile& file, StreamWriter& writer,
    uid::WellKnown target_transfer_syntax, StreamingWriteEncodeMode encode_mode,
    std::span<const pixel::CodecOptionTextKv> codec_opt_override,
    const pixel::EncoderContext* encoder_ctx, const WriteOptions& options);

void write_with_transfer_syntax_to_vector_writer(DicomFile& file, VectorWriter& writer,
    uid::WellKnown target_transfer_syntax, StreamingWriteEncodeMode encode_mode,
    std::span<const pixel::CodecOptionTextKv> codec_opt_override,
    const pixel::EncoderContext* encoder_ctx, const WriteOptions& options);

}  // namespace dicom::write_detail
