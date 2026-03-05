#pragma once

namespace dicom::test {

#if defined(DICOMSDL_PIXEL_JPEG_STATIC_PLUGIN_ENABLED) && DICOMSDL_PIXEL_JPEG_STATIC_PLUGIN_ENABLED
inline constexpr bool kJpegBuiltin = true;
#else
inline constexpr bool kJpegBuiltin = false;
#endif

#if defined(DICOMSDL_PIXEL_JPEGLS_STATIC_PLUGIN_ENABLED) && DICOMSDL_PIXEL_JPEGLS_STATIC_PLUGIN_ENABLED
inline constexpr bool kJpegLsBuiltin = true;
#else
inline constexpr bool kJpegLsBuiltin = false;
#endif

#if defined(DICOMSDL_PIXEL_OPENJPEG_STATIC_PLUGIN_ENABLED) && DICOMSDL_PIXEL_OPENJPEG_STATIC_PLUGIN_ENABLED
inline constexpr bool kJpeg2kBuiltin = true;
#else
inline constexpr bool kJpeg2kBuiltin = false;
#endif

#if defined(DICOMSDL_PIXEL_HTJ2K_STATIC_PLUGIN_ENABLED) && DICOMSDL_PIXEL_HTJ2K_STATIC_PLUGIN_ENABLED
inline constexpr bool kHtj2kBuiltin = true;
#else
inline constexpr bool kHtj2kBuiltin = false;
#endif

#if defined(DICOMSDL_PIXEL_JPEGXL_STATIC_PLUGIN_ENABLED) && DICOMSDL_PIXEL_JPEGXL_STATIC_PLUGIN_ENABLED
inline constexpr bool kJpegXlBuiltin = true;
#else
inline constexpr bool kJpegXlBuiltin = false;
#endif

#if defined(DICOMSDL_HAS_OPENJPH) && DICOMSDL_HAS_OPENJPH
inline constexpr bool kHasOpenJphBackend = true;
#else
inline constexpr bool kHasOpenJphBackend = false;
#endif

#if defined(DICOMSDL_HAS_JPEGXL) && DICOMSDL_HAS_JPEGXL
inline constexpr bool kHasJpegXlBackend = true;
#else
inline constexpr bool kHasJpegXlBackend = false;
#endif

} // namespace dicom::test
