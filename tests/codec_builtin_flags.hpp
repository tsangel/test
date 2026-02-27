#pragma once

namespace dicom::test {

#if defined(DICOMSDL_CODEC_JPEG_BUILTIN) && DICOMSDL_CODEC_JPEG_BUILTIN
inline constexpr bool kJpegBuiltin = true;
#else
inline constexpr bool kJpegBuiltin = false;
#endif

#if defined(DICOMSDL_CODEC_JPEGLS_BUILTIN) && DICOMSDL_CODEC_JPEGLS_BUILTIN
inline constexpr bool kJpegLsBuiltin = true;
#else
inline constexpr bool kJpegLsBuiltin = false;
#endif

#if defined(DICOMSDL_CODEC_JPEG2K_BUILTIN) && DICOMSDL_CODEC_JPEG2K_BUILTIN
inline constexpr bool kJpeg2kBuiltin = true;
#else
inline constexpr bool kJpeg2kBuiltin = false;
#endif

#if defined(DICOMSDL_CODEC_HTJ2K_BUILTIN) && DICOMSDL_CODEC_HTJ2K_BUILTIN
inline constexpr bool kHtj2kBuiltin = true;
#else
inline constexpr bool kHtj2kBuiltin = false;
#endif

#if defined(DICOMSDL_CODEC_JPEGXL_BUILTIN) && DICOMSDL_CODEC_JPEGXL_BUILTIN
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
