#include "dicom.h"

#include <memory>
#include <array>
#include "dicom_endian.h"
#include "diagnostics.h"

using namespace dicom::literals;
namespace diag = dicom::diag;

namespace dicom {

Sequence::Sequence(DataSet* root_dataset)
    : root_dataset_(root_dataset ? root_dataset : nullptr),
      transfer_syntax_uid_(root_dataset ? root_dataset->transfer_syntax_uid() : uid::WellKnown{}) {}

Sequence::~Sequence() = default;

void Sequence::read_from_stream(InStream* stream) {
	std::array<std::uint8_t, 8> buf8{};

	bool little_endian = root_dataset_->is_little_endian();
	
	while (!stream->is_eof()) {
		if (stream->read_8bytes(buf8) != 8) {
			diag::error_and_throw(
			    "Sequence::read_from_stream stream={} offset=0x{:X} reason=failed to read 8-byte item header",
			    stream->identifier(), stream->tell());
		}

		const std::uint16_t gggg = endian::load_value<std::uint16_t>(buf8.data(), little_endian);
		const std::uint16_t eeee = endian::load_value<std::uint16_t>(buf8.data() + 2, little_endian);
		const Tag tag{gggg, eeee};

		// Sequence Delimitation Item
		if (tag == "(fffe,e0dd)"_tag)  // Seq. Delim. Tag (FFFE, E0DD)
			break;
			
		if (tag != "(fffe,e000)"_tag) { // Item Tag (FFFE, E000)
			stream->unread(8);
			diag::error("Sequence::read_from_stream stream={} offset=0x{:X} reason=expected (FFFE,E000) item tag but found ({:04X},{:04X}); aborting sequence parse",
			    stream->identifier(), stream->tell(), gggg, eeee);
			break;
		}

		// PS3.3 Table F.3-3. Directory Information Module Attributes
    	// This offset includes the File Preamble and the DICM Prefix.
    	size_t offset = stream->tell();
		size_t length = endian::load_value<std::uint32_t>(buf8.data() + 4, little_endian);

		if (length == 0xffffffff) length = stream->bytes_remaining();

		DataSet* dataset = add_dataset();
		if (length) {
			dataset->attach_to_substream(stream, length);
			dataset->set_offset(offset);
			auto &subs = dataset->stream();
			size_t offset_start, offset_end;
			offset_start = subs.tell();
			dataset->read_elements_until("ffff,ffff"_tag, &subs);
			offset_end = subs.tell();
			stream->skip(offset_end - offset_start);
		}
	}
}

DataSet* Sequence::add_dataset() {
	seq_.push_back(std::make_unique<DataSet>(root_dataset_));
	return seq_.back().get();
}

DataSet* Sequence::get_dataset(std::size_t index) {
	if (index >= seq_.size()) {
		return nullptr;
	}
	return seq_[index].get();
}

const DataSet* Sequence::get_dataset(std::size_t index) const {
	if (index >= seq_.size()) {
		return nullptr;
	}
	return seq_[index].get();
}

DataSet* Sequence::operator[](std::size_t index) {
	return get_dataset(index);
}

const DataSet* Sequence::operator[](std::size_t index) const {
	return get_dataset(index);
}

std::vector<std::unique_ptr<DataSet>>::iterator Sequence::begin() {
	return seq_.begin();
}

std::vector<std::unique_ptr<DataSet>>::iterator Sequence::end() {
	return seq_.end();
}

std::vector<std::unique_ptr<DataSet>>::const_iterator Sequence::begin() const {
	return seq_.begin();
}

std::vector<std::unique_ptr<DataSet>>::const_iterator Sequence::end() const {
	return seq_.end();
}

std::vector<std::unique_ptr<DataSet>>::const_iterator Sequence::cbegin() const {
	return seq_.cbegin();
}

std::vector<std::unique_ptr<DataSet>>::const_iterator Sequence::cend() const {
	return seq_.cend();
}

}  // namespace dicom
