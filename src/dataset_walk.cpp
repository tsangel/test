#include "dicom.h"

#include <cstdio>

namespace dicom {

namespace {

[[nodiscard]] std::string packed_tag_string(Tag tag) {
	char buffer[9];
	std::snprintf(buffer, sizeof(buffer), "%08X", tag.value());
	return std::string(buffer);
}

}  // namespace

bool WalkPathRef::empty() const noexcept {
	return size() == 0;
}

std::size_t WalkPathRef::size() const noexcept {
	if (owner_ == nullptr || owner_->path_revision_ != revision_) {
		return 0;
	}
	return depth_;
}

bool WalkPathRef::contains_sequence(Tag sequence_tag) const noexcept {
	if (owner_ == nullptr || owner_->path_revision_ != revision_) {
		return false;
	}
	for (std::size_t i = 1; i <= depth_; ++i) {
		if (owner_->stack_[i].parent_sequence_tag == sequence_tag) {
			return true;
		}
	}
	return false;
}

std::string WalkPathRef::to_string() const {
	if (owner_ == nullptr || owner_->path_revision_ != revision_ || depth_ == 0) {
		return {};
	}

	std::string out;
	out.reserve(depth_ * 16);
	for (std::size_t i = 0; i < depth_; ++i) {
		if (i != 0) {
			out.push_back('.');
		}
		out += packed_tag_string(owner_->stack_[i + 1].parent_sequence_tag);
		out.push_back('.');
		out += std::to_string(owner_->stack_[i + 1].item_index);
	}
	return out;
}

void DataSetWalkEntry::skip_sequence() const noexcept {
	if (owner_ != nullptr && owner_->path_revision_ == revision_) {
		owner_->skip_sequence();
	}
}

void DataSetWalkEntry::skip_current_dataset() const noexcept {
	if (owner_ != nullptr && owner_->path_revision_ == revision_) {
		owner_->skip_current_dataset();
	}
}

DataSetWalkIterator::DataSetWalkIterator(DataSet& dataset) {
	initialize_root(dataset);
}

DataSetWalkIterator::DataSetWalkIterator(const DataSetWalkIterator& other)
    : stack_(other.stack_),
      current_(other.current_),
      path_revision_(other.path_revision_),
      skip_sequence_on_increment_(other.skip_sequence_on_increment_),
      skip_current_dataset_on_increment_(other.skip_current_dataset_on_increment_) {}

DataSetWalkIterator& DataSetWalkIterator::operator=(const DataSetWalkIterator& other) {
	if (this != &other) {
		stack_ = other.stack_;
		current_ = other.current_;
		path_revision_ = other.path_revision_;
		skip_sequence_on_increment_ = other.skip_sequence_on_increment_;
		skip_current_dataset_on_increment_ = other.skip_current_dataset_on_increment_;
		arrow_entry_.reset();
	}
	return *this;
}

DataSetWalkIterator::DataSetWalkIterator(DataSetWalkIterator&& other) noexcept
    : stack_(std::move(other.stack_)),
      current_(other.current_),
      path_revision_(other.path_revision_),
      skip_sequence_on_increment_(other.skip_sequence_on_increment_),
      skip_current_dataset_on_increment_(other.skip_current_dataset_on_increment_) {}

DataSetWalkIterator& DataSetWalkIterator::operator=(DataSetWalkIterator&& other) noexcept {
	if (this != &other) {
		stack_ = std::move(other.stack_);
		current_ = other.current_;
		path_revision_ = other.path_revision_;
		skip_sequence_on_increment_ = other.skip_sequence_on_increment_;
		skip_current_dataset_on_increment_ = other.skip_current_dataset_on_increment_;
		arrow_entry_.reset();
	}
	return *this;
}

DataSetWalkEntry DataSetWalkIterator::operator*() const {
	return DataSetWalkEntry(
	    WalkPathRef(this, stack_.size() > 1 ? stack_.size() - 1 : 0, path_revision_),
	    *current_, const_cast<DataSetWalkIterator*>(this), path_revision_);
}

const DataSetWalkIterator::value_type* DataSetWalkIterator::operator->() const {
	arrow_entry_.emplace(operator*());
	return &*arrow_entry_;
}

DataSetWalkIterator& DataSetWalkIterator::operator++() {
	if (current_ == nullptr || stack_.empty()) {
		return *this;
	}

	arrow_entry_.reset();
	StackEntry& stack_entry = stack_.back();
	DataElement& yielded = *stack_entry.current;
	Sequence* yielded_sequence =
	    (!skip_sequence_on_increment_ && !skip_current_dataset_on_increment_ &&
	                             yielded.vr().is_sequence())
	                                ? yielded.as_sequence()
	                                : nullptr;
	skip_sequence_on_increment_ = false;
	if (skip_current_dataset_on_increment_) {
		stack_entry.current = stack_entry.end;
		skip_current_dataset_on_increment_ = false;
	} else {
		++stack_entry.current;
	}
	if (yielded_sequence != nullptr && yielded_sequence->size() > 0) {
		push_sequence_item_stack_entry(*yielded_sequence, yielded.tag(), 0);
	}
	advance_to_next_available();
	++path_revision_;
	arrow_entry_.reset();
	return *this;
}

DataSetWalkIterator DataSetWalkIterator::operator++(int) {
	auto copy = *this;
	++(*this);
	return copy;
}

void DataSetWalkIterator::skip_sequence() noexcept {
	if (current_ != nullptr && current_->vr().is_sequence()) {
		skip_sequence_on_increment_ = true;
	}
}

void DataSetWalkIterator::skip_current_dataset() noexcept {
	if (current_ != nullptr && !stack_.empty()) {
		skip_sequence_on_increment_ = false;
		skip_current_dataset_on_increment_ = true;
	}
}

bool operator==(const DataSetWalkIterator& lhs, const DataSetWalkIterator& rhs) noexcept {
	if (lhs.current_ == nullptr && rhs.current_ == nullptr) {
		return true;
	}
	return lhs.current_ == rhs.current_ && lhs.stack_.size() == rhs.stack_.size() &&
	       lhs.skip_sequence_on_increment_ == rhs.skip_sequence_on_increment_ &&
	       lhs.skip_current_dataset_on_increment_ ==
	           rhs.skip_current_dataset_on_increment_;
}

void DataSetWalkIterator::initialize_root(DataSet& dataset) {
	stack_.clear();
	skip_sequence_on_increment_ = false;
	skip_current_dataset_on_increment_ = false;
	current_ = nullptr;
	path_revision_ = 0;
	arrow_entry_.reset();

	stack_.push_back(StackEntry{
	    .current = dataset.begin(),
	    .end = dataset.end(),
	});
	advance_to_next_available();
	if (current_ != nullptr) {
		++path_revision_;
	}
}

void DataSetWalkIterator::push_sequence_item_stack_entry(
    Sequence& sequence,
    Tag sequence_tag,
    std::uint32_t item_index) {
	arrow_entry_.reset();
	DataSet* item_dataset = sequence.get_dataset(item_index);
	if (item_dataset != nullptr) {
		stack_.push_back(StackEntry{
		    .current = item_dataset->begin(),
		    .end = item_dataset->end(),
		    .parent_sequence = &sequence,
		    .parent_sequence_tag = sequence_tag,
		    .item_index = item_index,
		});
		return;
	}

	stack_.push_back(StackEntry{
	    .parent_sequence = &sequence,
	    .parent_sequence_tag = sequence_tag,
	    .item_index = item_index,
	});
}

void DataSetWalkIterator::advance_to_next_available() {
	while (!stack_.empty()) {
		StackEntry& stack_entry = stack_.back();
		if (stack_entry.current != stack_entry.end) {
			current_ = &(*stack_entry.current);
			return;
		}

		if (stack_.size() == 1) {
			stack_.clear();
			current_ = nullptr;
			return;
		}

		const StackEntry finished_entry = stack_.back();
		stack_.pop_back();

		if (finished_entry.parent_sequence == nullptr) {
			continue;
		}

		const auto next_item_index = static_cast<std::size_t>(finished_entry.item_index) + 1;
		if (next_item_index < static_cast<std::size_t>(finished_entry.parent_sequence->size())) {
			DataSet* next_dataset = finished_entry.parent_sequence->get_dataset(next_item_index);
			if (next_dataset != nullptr) {
				stack_.push_back(StackEntry{
				    .current = next_dataset->begin(),
				    .end = next_dataset->end(),
				    .parent_sequence = finished_entry.parent_sequence,
				    .parent_sequence_tag = finished_entry.parent_sequence_tag,
				    .item_index = static_cast<std::uint32_t>(next_item_index),
				});
			} else {
				stack_.push_back(StackEntry{
				    .parent_sequence = finished_entry.parent_sequence,
				    .parent_sequence_tag = finished_entry.parent_sequence_tag,
				    .item_index = static_cast<std::uint32_t>(next_item_index),
				});
			}
			continue;
		}
	}

	current_ = nullptr;
}

DataSetWalkIterator DataSetWalker::begin() const {
	if (dataset_ == nullptr) {
		return DataSetWalkIterator();
	}
	return DataSetWalkIterator(*dataset_);
}

DataSetWalker DataSet::walk() {
	return DataSetWalker(*this);
}

DataSetWalker DicomFile::walk() {
	return DataSetWalker(dataset());
}

}  // namespace dicom
