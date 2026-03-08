// Source of truth for DICOMSDL-wide compile-time constants.
#pragma once

#include <string_view>

#define DICOMSDL_VERSION "0.1.26"
#define DICOM_STANDARD_VERSION "2026a"
#define DICOMSDL_UID_PREFIX "1.3.6.1.4.1.56559"
#define DICOMSDL_IMPLEMENTATION_CLASS_UID "1.3.6.1.4.1.56559.1"
#define DICOMSDL_IMPLEMENTATION_VERSION_NAME "DICOMSDL 2026FEB"

namespace dicom {

inline constexpr std::string_view kDicomsdlVersion = DICOMSDL_VERSION;
inline constexpr std::string_view kDicomStandardVersion = DICOM_STANDARD_VERSION;
inline constexpr std::string_view kUidPrefix = DICOMSDL_UID_PREFIX;
inline constexpr std::string_view kImplementationClassUid = DICOMSDL_IMPLEMENTATION_CLASS_UID;
inline constexpr std::string_view kImplementationVersionName = DICOMSDL_IMPLEMENTATION_VERSION_NAME;

} // namespace dicom
