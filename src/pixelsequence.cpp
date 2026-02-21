#include "dicom.h"
#include "diagnostics.h"
#include "dicom_endian.h"

#include <utility>

using namespace dicom::literals;

namespace dicom {

inline bool ends_with_ffd9(std::span<const std::uint8_t> s) {
	std::size_t tail = s.size();
	while (tail > 0 && s[tail - 1] == 0x00) {
		--tail;
	}
	return tail >= 2 && s[tail - 2] == 0xFF && s[tail - 1] == 0xD9;
}

// Detects whether the given fragment likely starts a new frame for transfer
// syntaxes that do NOT rely on an FFD9/EOC trailer (e.g., H.264/HEVC/MPEG-2,
// JPEG XL). FFD9-terminated codecs are handled separately.
inline bool fragment_starts_new_frame_nonffd9(std::span<const std::uint8_t> s, uid::WellKnown ts) {
	if (s.empty()) {
		return false;
	}

	const auto ts_mask = uid::detail::ts_mask(ts.raw_index());

	// Limit how far we scan into a fragment for start codes to avoid O(n) walks
	// on very large fragments. Most codecs place a start code near the front.
	constexpr std::size_t kMaxStartCodeProbe = 16 * 1024;  // bytes
	const auto limited_size = std::min(s.size(), kMaxStartCodeProbe);

	auto find_start_code = [&](std::size_t& off, std::size_t& code_len) -> bool {
		for (std::size_t i = 0; i + 3 < limited_size; ++i) {
			if (s[i] == 0x00 && s[i + 1] == 0x00 && s[i + 2] == 0x01) {
				off = i + 3;
				code_len = 3;
				return true;
			}
			if (i + 4 < limited_size && s[i] == 0x00 && s[i + 1] == 0x00 && s[i + 2] == 0x00 && s[i + 3] == 0x01) {
				off = i + 4;
				code_len = 4;
				return true;
			}
		}
		return false;
	};

	// H.264 Annex B: start code 00 00 01 or 00 00 00 01 followed by slice/AUD.
	if (ts_mask & uid::detail::kTSH264) {
		std::size_t off = 0;
		std::size_t code_len = 0;
		if (find_start_code(off, code_len)) {
			if (off < s.size()) {
				const std::uint8_t nal_type = s[off] & 0x1F;
				if (nal_type == 1 || nal_type == 5 || nal_type == 9) {
					return true;
				}
			}
		}
	}

	// HEVC Annex B: start code followed by slice/AUD. nal_type is 6-bit (bits 1-6).
	if (ts_mask & uid::detail::kTSHevc) {
		std::size_t off = 0;
		std::size_t code_len = 0;
		if (find_start_code(off, code_len)) {
			if (off < s.size()) {
				const std::uint8_t nal_type = (s[off] >> 1) & 0x3F;
				if (nal_type <= 31 || nal_type == 35) {  // slice segments or AUD
					return true;
				}
			}
		}
	}

	// JPEG XL codestream starts with FF 0A signature.
	if (ts_mask & uid::detail::kTSJpegXL) {
		return s.size() >= 2 && s[0] == 0xFF && s[1] == 0x0A;
	}

	// MPEG-1/2 Video ES: picture start or sequence/GOP headers.
	if (ts_mask & uid::detail::kTSMpeg2) {
		std::size_t off = 0;
		std::size_t code_len = 0;
		if (find_start_code(off, code_len) && off <= s.size()) {
			// For MPEG-2, the byte right after start code is the start code value.
			const std::uint8_t code = s[off];
			if (code == 0x00 || code == 0xB3 || code == 0xB8) {
				return true;
			}
		}
	}

	return false;
}

PixelFrame::PixelFrame() = default;
PixelFrame::~PixelFrame() = default;

void PixelFrame::set_encoded_data(std::vector<std::uint8_t> data) {
	encoded_data_ = std::move(data);
}

std::span<const std::uint8_t> PixelFrame::encoded_data_view() const noexcept {
	return std::span<const std::uint8_t>{encoded_data_};
}

Tag PixelFrame::read_from_stream(InStream* stream, std::size_t frame_length, uid::WellKnown ts, bool length_from_bot) {
	std::array<std::uint8_t, 8> buf8{};

	const std::size_t frame_end = stream->tell() + frame_length;

	if (encoded_data_size() != 0) {
		diag::error_and_throw("PixelFrame::read_from_stream reason=encoded_data already populated");
	}

	Tag tag{};
	std::size_t frag_length = 0;
	while (true) {
		if (stream->read_8bytes(buf8) != 8) {
			diag::error_and_throw(
			    "PixelFrame::read_from_stream stream={} offset=0x{:X} reason=failed to read 8-byte fragment header",
			    stream->identifier(), stream->tell());
		}

		tag = endian::load_tag_le(buf8.data());
		frag_length = endian::load_le<std::uint32_t>(buf8.data() + 4);

		if (tag == "fffe,e0dd"_tag) {
			// This Sequence of Items is terminated by a Sequence Delimiter Item
			// with the Tag (FFFE,E0DD).
			break;
		}

		// Assert Tag is 'Item Tag'
		if (tag != "fffe,e000"_tag) {
			diag::error_and_throw(
				"PixelFrame::read_from_stream stream={} offset=0x{:X} reason=expected (FFFE,E000) item tag but found ({:04X},{:04X})",
				stream->identifier(), stream->tell() - 8, tag.group(), tag.element());
		}

		size_t frag_offset = stream->tell();

		if (!fragments_.empty() && !length_from_bot) {
			if (ts.ends_with_ffd9_marker()) {
				const auto& last = fragments_.back();
				auto frag_span = stream->get_span(last.offset, last.length);
				if (ends_with_ffd9(frag_span)) {
					stream->unread(8);
					break;
				}
			} else {
				auto frag_span = stream->get_span(frag_offset, frag_length);
				if (fragment_starts_new_frame_nonffd9(frag_span, ts)) {
					stream->unread(8);
					break;
				}
			}
		}

		if (stream->skip(frag_length) != frag_length) {
			diag::error_and_throw(
			    "PixelFrame::read_from_stream stream={} offset=0x{:X} length={} reason=failed to skip fragment bytes",
			    stream->identifier(), stream->tell(), frag_length);
		}

		fragments_.push_back(PixelFragment{frag_offset, frag_length});

		// I have no more bytes to read.
		if (stream->tell() >= frame_end)
			break;
	}

	return tag; // (FFFE,E000) or (FFFE,E0DD)
}

void PixelFrame::discard_encoded_data() {
	encoded_data_.clear();
	encoded_data_.shrink_to_fit();
}

std::vector<std::span<const std::uint8_t>> PixelFrame::fragment_views(const InStream& seq_stream) const {
	std::vector<std::span<const std::uint8_t>> views;
	views.reserve(fragments_.size());
	for (const auto& frag : fragments_) {
		views.emplace_back(seq_stream.get_span(frag.offset, frag.length));
	}
	return views;
}

std::vector<std::uint8_t> PixelFrame::coalesce_encoded_data(const InStream& seq_stream) const {
	std::vector<std::uint8_t> buffer;
	if (fragments_.empty()) {
		return buffer;
	}
	std::size_t total = 0;
	for (const auto& frag : fragments_) {
		total += frag.length;
	}
	buffer.reserve(total);
	for (const auto& frag : fragments_) {
		auto span = seq_stream.get_span(frag.offset, frag.length);
		buffer.insert(buffer.end(), span.begin(), span.end());
	}
	return buffer;
}

PixelSequence::PixelSequence(DataSet* root_dataset, uid::WellKnown transfer_syntax)
	: root_dataset_(root_dataset ? root_dataset->root_dataset() : nullptr),
	  transfer_syntax_(transfer_syntax.valid()
	          ? transfer_syntax
	          : (root_dataset_ ? root_dataset_->transfer_syntax_uid() : uid::WellKnown{})) {}

PixelSequence::~PixelSequence() = default;

PixelSequence::PixelSequence(PixelSequence&&) noexcept = default;
PixelSequence& PixelSequence::operator=(PixelSequence&&) noexcept = default;

PixelFrame* PixelSequence::add_frame() {
	frames_.push_back(std::make_unique<PixelFrame>());
	return frames_.back().get();
}

PixelFrame* PixelSequence::frame(std::size_t index) {
	if (index >= frames_.size()) {
		return nullptr;
	}
	return frames_[index].get();
}

const PixelFrame* PixelSequence::frame(std::size_t index) const {
	if (index >= frames_.size()) {
		return nullptr;
	}
	return frames_[index].get();
}

std::span<const std::uint8_t> PixelSequence::frame_encoded_span(std::size_t index) {
	PixelFrame* f = frame(index);
	if (!f) {
		return {};
	}

	// 1) Reuse already materialized buffer if present.
	if (f->encoded_data_size() != 0) {
		return f->encoded_data_view();
	}

	const auto& frags = f->fragments();
	if (frags.empty()) {
		return {};
	}

	if (!stream_) {
		diag::error_and_throw("PixelSequence::frame_encoded_span reason=null stream");
	}
	InStream* stream = stream_.get();

	// 2) Single fragment can be returned as a view without copy.
	if (frags.size() == 1) {
		const auto frag = frags.front();
		if (frag.offset + frag.length > stream->endoffset()) {
			diag::error_and_throw(
			    "PixelSequence::frame_encoded_span stream={} offset=0x{:X} length={} reason=fragment exceeds stream bounds",
			    stream->identifier(), frag.offset, frag.length);
		}
		return stream->get_span(frag.offset, frag.length);
	}

	// 3) Multiple fragments: coalesce once, then reuse.
	std::size_t total = 0;
	for (const auto& frag : frags) {
		if (frag.length > stream->endoffset()) {
			diag::error_and_throw("PixelSequence::frame_encoded_span reason=invalid fragment length");
		}
		total += frag.length;
	}
	std::vector<std::uint8_t> buffer;
	buffer.reserve(total);
	for (const auto& frag : frags) {
		if (frag.offset + frag.length > stream->endoffset()) {
			diag::error_and_throw(
			    "PixelSequence::frame_encoded_span stream={} offset=0x{:X} length={} reason=fragment exceeds stream bounds",
			    stream->identifier(), frag.offset, frag.length);
		}
		auto span = stream->get_span(frag.offset, frag.length);
		buffer.insert(buffer.end(), span.begin(), span.end());
	}
	f->set_encoded_data(std::move(buffer));
	return f->encoded_data_view();
}

void PixelSequence::clear_frame_encoded_data(std::size_t index) {
	if (auto* f = frame(index)) {
		f->discard_encoded_data();
	}
}

void PixelSequence::attach_to_stream(InStream* basestream, std::size_t size) {
	if (!basestream) {
		diag::error_and_throw("PixelSequence::attach_to_stream reason=null basestream");
	}
	stream_ = std::make_unique<InSubStream>(basestream, size);
}

void PixelSequence::read_attached_stream() {
	if (!stream_) {
		diag::error_and_throw("PixelSequence::read_attached_stream reason=null stream");
	}

	std::array<std::uint8_t, 8> buf8{};
	
	InStream *stream = stream_.get();

	if (stream->read_8bytes(buf8) != 8) {
		diag::error_and_throw(
		    "PixelSequence::read_attached_stream stream={} offset=0x{:X} reason=failed to read 8-byte item header",
		    stream->identifier(), stream->tell());
	}
	const Tag tag = endian::load_tag_le(buf8.data());
	const size_t length = endian::load_le<std::uint32_t>(buf8.data() + 4);

	// Assert Tag is 'Item Tag'
	if (tag != "fffe,e000"_tag) {
		diag::error_and_throw(
		    "PixelSequence::read_attached_stream stream={} offset=0x{:X} reason=expected (FFFE,E000) item tag but found ({:04X},{:04X})",
		    stream->identifier(), stream->tell() - 8, tag.group(), tag.element());
	}

	if (length >= stream->bytes_remaining()) {
		diag::error_and_throw(
		    "PixelSequence::read_attached_stream stream={} offset=0x{:X} length={} reason=basic offset table length exceeds remaining bytes ({})",
		    stream->identifier(), stream->tell(), length, stream->bytes_remaining());
	}
	
	if (length) {
		// This pixel sequence has 'Basic Offset Table'.
		// Table A.4-2. Examples of Elements for an Encoded Two-Frame Image
		// Defined as a Sequence of Three Fragments with Basic Table Item Values

		basic_offset_table_count_ = length / 4;
		std::vector<uint32_t> offset_table(basic_offset_table_count_);

		// Read offset table bytes into host memory.
		basic_offset_table_offset_ = stream->tell();
		const auto bytes_read = stream->read_into(offset_table.data(), length);
		if (bytes_read != length) {
			diag::error_and_throw(
			    "PixelSequence::read_attached_stream stream={} offset=0x{:X} length={} reason=failed to read basic offset table bytes; remaining={} bytes",
			    stream->identifier(), basic_offset_table_offset_, length, stream->bytes_remaining());
		}

		// First frame starts immediately after the Basic Offset Table.
		base_offset_ = stream->tell();

		struct OffsetEntry {
			std::size_t index;
			std::size_t offset;
			std::size_t length;
		};

		std::vector<OffsetEntry> entries(basic_offset_table_count_);
		for (std::size_t i = 0; i < basic_offset_table_count_; ++i) {
			entries[i] = OffsetEntry{i, offset_table[i], 0};
		}

		// Sort by offset to derive frame lengths, then restore original order.
		std::sort(entries.begin(), entries.end(), [](const OffsetEntry& a, const OffsetEntry& b) {
			return a.offset < b.offset;
		});

		const std::size_t end_limit = stream->endoffset();
		for (std::size_t i = 0; i < entries.size(); ++i) {
			const auto current_offset = entries[i].offset;
			const auto next_offset = (i + 1 < entries.size())
			    ? entries[i + 1].offset
			    : (end_limit - base_offset_);
			if (next_offset < current_offset) {
				diag::error_and_throw(
				    "PixelSequence::read_attached_stream stream={} reason=offset table not non-decreasing (offset[{}]=0x{:X}, next=0x{:X})",
				    stream->identifier(), i, current_offset, next_offset);
			}
			entries[i].length = next_offset - current_offset;
		}

		std::sort(entries.begin(), entries.end(), [](const OffsetEntry& a, const OffsetEntry& b) {
			return a.index < b.index;
		});

		size_t last_frame_end = 0;

		PixelFrame* frame = add_frame();

		for (const auto& entry : entries) {
			const std::size_t frame_start = base_offset_ + entry.offset;
			const std::size_t frame_end = frame_start + entry.length;

			if (frame_end > end_limit) {
				diag::error_and_throw(
				    "PixelSequence::read_attached_stream stream={} offset=0x{:X} reason=frame bounds out of range (start=0x{:X}, end=0x{:X}, limit=0x{:X})",
				    stream->identifier(), stream->tell(), frame_start, frame_end, end_limit);
			}		

			// frame->startoffset_ <- base_offset_ + startpos
      		stream->seek(frame_start);
			frame->read_from_stream(stream, entry.length, transfer_syntax_, true /*length_from_bot*/);
			if (stream->tell() > last_frame_end)
				last_frame_end = stream->tell();

			frame = add_frame();
		}

		// stream->tell() is located just after item with tag (fffe,e0dd).
    	// ; frame->load already ate that item.
    	stream->seek(last_frame_end);
	} else {
		// Basic Offset Table with NO Item Value
		// Table A.4-1. Example for Elements of an Encoded Single-frame Image
		// Defined as a Sequence of Three Fragments Without Basic Offset Table Item Value

		// First frame starts immediately after the Basic Offset Table.
		base_offset_ = stream->tell();

		PixelFrame* frame = add_frame();

		while (true) {
			auto last_tag = frame->read_from_stream(stream, stream->bytes_remaining(), transfer_syntax_, false /*length_from_bot*/);
			if (last_tag == "fffe,e0dd"_tag)
				break;
			if (stream->bytes_remaining() < 8) // a fragment takes a least 8 bytes
				break;

			frame = add_frame();
		}
	}

	// If the last frame collected no fragments, discard it.
	if (!frames_.empty()) {
		const auto& last = frames_.back();
		if (last && last->fragments().empty()) {
			frames_.pop_back();
		}
	}
}

}  // namespace dicom
