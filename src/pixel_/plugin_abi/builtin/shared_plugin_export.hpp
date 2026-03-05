#pragma once

#if defined(_WIN32)
#if defined(DICOMSDL_CODEC_RUNTIME_EXPORTS)
#define DICOMSDL_CODEC_RUNTIME_API __declspec(dllexport)
#elif defined(DICOMSDL_CODEC_RUNTIME_IMPORTS)
#define DICOMSDL_CODEC_RUNTIME_API __declspec(dllimport)
#else
#define DICOMSDL_CODEC_RUNTIME_API
#endif
#else
#if defined(DICOMSDL_CODEC_RUNTIME_EXPORTS)
#define DICOMSDL_CODEC_RUNTIME_API __attribute__((visibility("default")))
#else
#define DICOMSDL_CODEC_RUNTIME_API
#endif
#endif

