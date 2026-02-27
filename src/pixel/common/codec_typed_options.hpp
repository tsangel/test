#pragma once

namespace dicom::pixel {

struct JpegOptions {
	int quality{90};
};

struct JpegLsOptions {
	int near_lossless_error{0};
};

struct J2kOptions {
	double target_bpp{0.0};
	double target_psnr{0.0};
	// Encoder thread hint:
	//  -1: auto(all CPUs) [default], 0: library default, >0: explicit thread count.
	int threads{-1};
	bool use_color_transform{true};
};

struct Htj2kOptions {
	double target_bpp{0.0};
	double target_psnr{0.0};
	// Encoder thread hint:
	//  -1: auto(all CPUs) [default], 0: library default, >0: explicit thread count.
	int threads{-1};
	bool use_color_transform{true};
};

struct JpegXlOptions {
	// Lossy distance target (0: mathematically lossless, recommended lossy range ~0.5..3.0).
	// For JPEGXL transfer syntax, this must be > 0.
	// For JPEGXLLossless transfer syntax, this must be 0.
	double distance{1.0};
	// Encoder effort/speed: 1(fastest) .. 10(slowest), default 7.
	int effort{7};
	// Encoder thread hint:
	//  -1: auto(all CPUs) [default], 0: library default, >0: explicit thread count.
	int threads{-1};
};

} // namespace dicom::pixel
