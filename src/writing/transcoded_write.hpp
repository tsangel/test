#pragma once

#include "writing/detail/write_sinks.hpp"

namespace dicom::write_detail {

void write_with_transfer_syntax_to_stream_writer(DicomFile& file, StreamWriter& writer,
    uid::WellKnown target_transfer_syntax, WriteEncoderConfigSource encode_mode,
    std::span<const pixel::CodecOptionTextKv> codec_opt_override,
    const pixel::EncoderContext* encoder_ctx, const WriteOptions& options);

void write_with_transfer_syntax_to_buffer_writer(DicomFile& file, BufferWriter& writer,
    uid::WellKnown target_transfer_syntax, WriteEncoderConfigSource encode_mode,
    std::span<const pixel::CodecOptionTextKv> codec_opt_override,
    const pixel::EncoderContext* encoder_ctx, const WriteOptions& options);

}  // namespace dicom::write_detail
