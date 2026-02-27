#include "diagnostics.h"

namespace dicom::diag {

std::shared_ptr<Reporter>& thread_reporter_slot() noexcept {
	// Keep the TLS object local to this TU to avoid cross-object TLS init duplication.
	thread_local std::shared_ptr<Reporter> thread_reporter{nullptr};
	return thread_reporter;
}

}  // namespace dicom::diag
