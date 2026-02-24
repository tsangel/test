#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <dicom.h>

namespace {

struct CliOptions {
	std::string input_path{};
	std::string output_path{};
	std::string transfer_syntax_text{};
	std::optional<std::string> codec_type{};
	std::optional<double> target_bpp{};
	std::optional<double> target_psnr{};
	std::optional<double> distance{};
	std::optional<int> effort{};
	std::optional<int> threads{};
	std::optional<bool> color_transform{};
	std::optional<int> quality{};
	std::optional<int> near_lossless_error{};
};

void print_transfer_syntax_list() {
	std::cerr << "\nAvailable Transfer Syntax UIDs (keyword = UID):\n";
	for (const auto& entry : dicom::kUidRegistry) {
		if (entry.uid_type != dicom::UidType::TransferSyntax) {
			continue;
		}
		if (entry.keyword.empty()) {
			std::cerr << "  " << entry.value << '\n';
			continue;
		}
		std::cerr << "  " << entry.keyword << " = " << entry.value << '\n';
	}
}

std::string normalize_token(std::string_view text) {
	std::string normalized{};
	normalized.reserve(text.size());
	for (const unsigned char ch : text) {
		if (ch == '_' || ch == '-' || ch == ' ' || ch == '\t') {
			continue;
		}
		normalized.push_back(static_cast<char>(std::tolower(ch)));
	}
	return normalized;
}

bool parse_int_value(std::string_view text, int& out_value) {
	const std::string copied{text};
	if (copied.empty()) {
		return false;
	}
	char* end = nullptr;
	errno = 0;
	const long parsed = std::strtol(copied.c_str(), &end, 10);
	if (errno != 0 || end == nullptr || *end != '\0') {
		return false;
	}
	if (parsed < std::numeric_limits<int>::min() || parsed > std::numeric_limits<int>::max()) {
		return false;
	}
	out_value = static_cast<int>(parsed);
	return true;
}

bool parse_double_value(std::string_view text, double& out_value) {
	const std::string copied{text};
	if (copied.empty()) {
		return false;
	}
	char* end = nullptr;
	errno = 0;
	const double parsed = std::strtod(copied.c_str(), &end);
	if (errno != 0 || end == nullptr || *end != '\0' || !std::isfinite(parsed)) {
		return false;
	}
	out_value = parsed;
	return true;
}

void print_usage(const char* prog, bool include_transfer_syntax_list) {
	std::cerr
	    << "Usage: " << prog << " <input.dcm> <output.dcm> <transfer-syntax> [options]\n"
	    << "\n"
	    << "Transfer syntax:\n"
	    << "  - DICOM keyword or UID value (for example: JPEG2000, 1.2.840.10008.1.2.4.90)\n"
	    << "  - Short aliases: jpeg, jpeglossless, jpegls, jpegls-near-lossless,\n"
	    << "                   jpeg2k, jpeg2k-lossless, jpeg2k-mc, htj2k, htj2k-lossless,\n"
	    << "                   jpegxl, jpegxl-lossless, jpegxl-jpeg-recompression, rle\n"
	    << "\n"
	    << "Options:\n"
	    << "  --codec TYPE              auto|none|rle|jpeg|jpegls|j2k|htj2k|jpegxl (default: auto)\n"
	    << "  --quality N               JPEG quality [1,100]\n"
	    << "  --near-lossless-error N   JPEG-LS NEAR [0,255]\n"
	    << "  --target-psnr V           JPEG2000/HTJ2K lossy target PSNR\n"
	    << "  --target-bpp V            JPEG2000/HTJ2K lossy target bits-per-pixel\n"
	    << "  --distance V              JPEG-XL distance [0,25] (0: lossless)\n"
	    << "  --effort N                JPEG-XL effort [1,10]\n"
	    << "  --threads N               JPEG2000/HTJ2K/JPEG-XL encoder threads (-1:auto,0:library,>0:count)\n"
	    << "  --color-transform         Enable JPEG2000/HTJ2K MCT (RGB->YBR_*)\n"
	    << "  --no-color-transform      Disable JPEG2000/HTJ2K MCT\n"
	    << "  -h, --help                Show this help\n"
	    << "\n"
	    << "Examples:\n"
	    << "  " << prog << " in.dcm out.dcm ExplicitVRLittleEndian\n"
	    << "  " << prog << " in.dcm out.dcm jpeg --quality 92\n"
	    << "  " << prog << " in.dcm out.dcm jpegls-near-lossless --near-lossless-error 3\n"
	    << "  " << prog << " in.dcm out.dcm jpeg2k --target-psnr 45 --threads -1\n"
	    << "  " << prog << " in.dcm out.dcm htj2k-lossless --codec htj2k --no-color-transform\n"
	    << "  " << prog << " in.dcm out.dcm jpegxl --distance 1.5 --effort 7 --threads -1\n"
	    << "  " << prog << " in.dcm out.dcm jpegxl-lossless --distance 0\n"
	    << "\n"
	    << "The tool calls DicomFile::set_transfer_syntax(..., codec_opt) internally.\n";
	if (include_transfer_syntax_list) {
		print_transfer_syntax_list();
	}
}

bool parse_args(int argc, char** argv, CliOptions& opts, bool& help_shown) {
	help_shown = false;
	std::vector<std::string> positional{};
	positional.reserve(3);

	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "-h" || arg == "--help") {
			print_usage(argv[0], true);
			help_shown = true;
			return false;
		}

		const auto read_option_value = [&](std::string_view option_name) -> std::optional<std::string> {
			if (i + 1 >= argc) {
				std::cerr << "Missing value for " << option_name << '\n';
				print_usage(argv[0], false);
				return std::nullopt;
			}
			++i;
			return std::string(argv[i]);
		};

		if (arg == "--codec") {
			auto value = read_option_value("--codec");
			if (!value) {
				return false;
			}
			opts.codec_type = *value;
			continue;
		}
		if (arg == "--quality") {
			auto value = read_option_value("--quality");
			if (!value) {
				return false;
			}
			int parsed = 0;
			if (!parse_int_value(*value, parsed)) {
				std::cerr << "Invalid --quality value: " << *value << '\n';
				print_usage(argv[0], false);
				return false;
			}
			opts.quality = parsed;
			continue;
		}
		if (arg == "--near-lossless-error") {
			auto value = read_option_value("--near-lossless-error");
			if (!value) {
				return false;
			}
			int parsed = 0;
			if (!parse_int_value(*value, parsed)) {
				std::cerr << "Invalid --near-lossless-error value: " << *value << '\n';
				print_usage(argv[0], false);
				return false;
			}
			opts.near_lossless_error = parsed;
			continue;
		}
		if (arg == "--target-psnr") {
			auto value = read_option_value("--target-psnr");
			if (!value) {
				return false;
			}
			double parsed = 0.0;
			if (!parse_double_value(*value, parsed)) {
				std::cerr << "Invalid --target-psnr value: " << *value << '\n';
				print_usage(argv[0], false);
				return false;
			}
			opts.target_psnr = parsed;
			continue;
		}
		if (arg == "--target-bpp") {
			auto value = read_option_value("--target-bpp");
			if (!value) {
				return false;
			}
			double parsed = 0.0;
			if (!parse_double_value(*value, parsed)) {
				std::cerr << "Invalid --target-bpp value: " << *value << '\n';
				print_usage(argv[0], false);
				return false;
			}
			opts.target_bpp = parsed;
			continue;
		}
		if (arg == "--threads") {
			auto value = read_option_value("--threads");
			if (!value) {
				return false;
			}
			int parsed = 0;
			if (!parse_int_value(*value, parsed)) {
				std::cerr << "Invalid --threads value: " << *value << '\n';
				print_usage(argv[0], false);
				return false;
			}
			opts.threads = parsed;
			continue;
		}
		if (arg == "--distance") {
			auto value = read_option_value("--distance");
			if (!value) {
				return false;
			}
			double parsed = 0.0;
			if (!parse_double_value(*value, parsed)) {
				std::cerr << "Invalid --distance value: " << *value << '\n';
				print_usage(argv[0], false);
				return false;
			}
			opts.distance = parsed;
			continue;
		}
		if (arg == "--effort") {
			auto value = read_option_value("--effort");
			if (!value) {
				return false;
			}
			int parsed = 0;
			if (!parse_int_value(*value, parsed)) {
				std::cerr << "Invalid --effort value: " << *value << '\n';
				print_usage(argv[0], false);
				return false;
			}
			opts.effort = parsed;
			continue;
		}
		if (arg == "--color-transform") {
			opts.color_transform = true;
			continue;
		}
		if (arg == "--no-color-transform") {
			opts.color_transform = false;
			continue;
		}
		if (!arg.empty() && arg[0] == '-') {
			std::cerr << "Unknown option: " << arg << '\n';
			print_usage(argv[0], false);
			return false;
		}
		positional.push_back(arg);
	}

	if (positional.size() != 3) {
		print_usage(argv[0], false);
		return false;
	}

	opts.input_path = positional[0];
	opts.output_path = positional[1];
	opts.transfer_syntax_text = positional[2];
	return true;
}

std::optional<dicom::uid::WellKnown> resolve_transfer_syntax_alias(std::string_view text) {
	using namespace dicom::literals;
	const auto normalized = normalize_token(text);

	if (normalized == "implicit" || normalized == "implicitle" ||
	    normalized == "implicitvrlittleendian") {
		return "ImplicitVRLittleEndian"_uid;
	}
	if (normalized == "explicit" || normalized == "explicitle" ||
	    normalized == "explicitvrlittleendian" || normalized == "native" ||
	    normalized == "raw" || normalized == "uncompressed") {
		return "ExplicitVRLittleEndian"_uid;
	}
	if (normalized == "deflate" || normalized == "deflatedexplicitvrlittleendian") {
		return "DeflatedExplicitVRLittleEndian"_uid;
	}
	if (normalized == "encapsulateduncompressed" || normalized == "encapraw" ||
	    normalized == "encapuncompressed" ||
	    normalized == "encapsulateduncompressedexplicitvrlittleendian") {
		return "EncapsulatedUncompressedExplicitVRLittleEndian"_uid;
	}
	if (normalized == "rle" || normalized == "rlelossless") {
		return "RLELossless"_uid;
	}
	if (normalized == "jpeg" || normalized == "jpegbaseline" ||
	    normalized == "jpegbaseline8bit") {
		return "JPEGBaseline8Bit"_uid;
	}
	if (normalized == "jpeglossless" || normalized == "jpeglosslesssv1") {
		return "JPEGLosslessSV1"_uid;
	}
	if (normalized == "jpegls" || normalized == "jls" ||
	    normalized == "jpeglslossless") {
		return "JPEGLSLossless"_uid;
	}
	if (normalized == "jpeglsnear" || normalized == "jpeglsnearlossless" ||
	    normalized == "jlsnl") {
		return "JPEGLSNearLossless"_uid;
	}
	if (normalized == "j2k" || normalized == "jpeg2k" || normalized == "jpeg2000") {
		return "JPEG2000"_uid;
	}
	if (normalized == "j2klossless" || normalized == "jpeg2klossless" ||
	    normalized == "jpeg2000lossless") {
		return "JPEG2000Lossless"_uid;
	}
	if (normalized == "j2kmc" || normalized == "jpeg2kmc" ||
	    normalized == "jpeg2000mc") {
		return "JPEG2000MC"_uid;
	}
	if (normalized == "j2kmclossless" || normalized == "jpeg2kmclossless" ||
	    normalized == "jpeg2000mclossless") {
		return "JPEG2000MCLossless"_uid;
	}
	if (normalized == "htj2k") {
		return "HTJ2K"_uid;
	}
	if (normalized == "htj2klossless") {
		return "HTJ2KLossless"_uid;
	}
	if (normalized == "htj2klosslessrpcl") {
		return "HTJ2KLosslessRPCL"_uid;
	}
	if (normalized == "jpegxl" || normalized == "jxl") {
		return "JPEGXL"_uid;
	}
	if (normalized == "jpegxllossless" || normalized == "jxllossless") {
		return "JPEGXLLossless"_uid;
	}
	if (normalized == "jpegxljpegrecompression" ||
	    normalized == "jpegxlrecompression" ||
	    normalized == "jxljpegrecompression") {
		return "JPEGXLJPEGRecompression"_uid;
	}

	return std::nullopt;
}

dicom::uid::WellKnown resolve_transfer_syntax(const std::string& text) {
	if (const auto alias = resolve_transfer_syntax_alias(text)) {
		return *alias;
	}
	const auto uid = dicom::uid::lookup(text);
	if (!uid) {
		throw std::invalid_argument("Unknown UID: " + text);
	}
	if (uid->uid_type() != dicom::UidType::TransferSyntax) {
		throw std::invalid_argument("UID is not a Transfer Syntax: " + text);
	}
	return *uid;
}

dicom::pixel::CodecOptions build_codec_options(
    const CliOptions& opts, dicom::uid::WellKnown transfer_syntax) {
	using dicom::pixel::AutoCodecOptions;
	using dicom::pixel::CodecOptions;
	using dicom::pixel::Htj2kOptions;
	using dicom::pixel::J2kOptions;
	using dicom::pixel::JpegLsOptions;
	using dicom::pixel::JpegOptions;
	using dicom::pixel::JpegXlOptions;
	using dicom::pixel::NoCompression;
	using dicom::pixel::RleOptions;
	using namespace dicom::literals;

	const bool has_j2k_fields = opts.target_bpp.has_value() ||
	    opts.target_psnr.has_value() || opts.color_transform.has_value();
	const bool has_thread_field = opts.threads.has_value();
	const bool has_jpegxl_fields = opts.distance.has_value() || opts.effort.has_value();
	const bool has_jpeg_fields = opts.quality.has_value();
	const bool has_jpegls_fields = opts.near_lossless_error.has_value();
	const bool has_any_fields = has_j2k_fields || has_thread_field ||
	    has_jpegxl_fields || has_jpeg_fields || has_jpegls_fields;

	std::string codec = opts.codec_type ? normalize_token(*opts.codec_type) : std::string{};
	if (codec.empty()) {
		if (has_jpeg_fields) {
			codec = "jpeg";
		} else if (has_jpegls_fields) {
			codec = "jpegls";
		} else if (has_jpegxl_fields) {
			codec = "jpegxl";
		} else if (has_j2k_fields) {
			if (transfer_syntax.is_jpegxl()) {
				codec = "jpegxl";
			} else {
				codec = transfer_syntax.is_htj2k() ? "htj2k" : "j2k";
			}
		} else if (has_thread_field) {
			if (transfer_syntax.is_jpegxl()) {
				codec = "jpegxl";
			} else {
				codec = transfer_syntax.is_htj2k() ? "htj2k" : "j2k";
			}
		} else {
			codec = "auto";
		}
	}

	const auto ensure_no_fields = [&](std::string_view codec_name) {
		if (has_any_fields) {
			throw std::invalid_argument(
			    std::string(codec_name) + " codec does not accept extra option fields");
		}
	};

	if (codec == "auto") {
		ensure_no_fields("auto");
		return CodecOptions{AutoCodecOptions{}};
	}
	if (codec == "none" || codec == "nocompression" ||
	    codec == "native" || codec == "uncompressed") {
		ensure_no_fields("none");
		return CodecOptions{NoCompression{}};
	}
	if (codec == "rle" || codec == "rlelossless") {
		ensure_no_fields("rle");
		return CodecOptions{RleOptions{}};
	}
	if (codec == "j2k" || codec == "jpeg2k" || codec == "jpeg2000" ||
	    codec == "j2koptions") {
		if (has_jpeg_fields || has_jpegls_fields || has_jpegxl_fields) {
			throw std::invalid_argument(
			    "j2k codec does not accept quality/near-lossless-error/distance/effort");
		}
		J2kOptions options{};
		if (opts.target_bpp) {
			options.target_bpp = *opts.target_bpp;
		}
		if (opts.target_psnr) {
			options.target_psnr = *opts.target_psnr;
		}
		if (opts.threads) {
			options.threads = *opts.threads;
		}
		if (opts.color_transform) {
			options.use_color_transform = *opts.color_transform;
		}
		if (options.target_bpp < 0.0 || options.target_psnr < 0.0) {
			throw std::invalid_argument("target-bpp/target-psnr must be >= 0");
		}
		if (options.threads < -1) {
			throw std::invalid_argument("threads must be -1, 0, or positive");
		}
		return CodecOptions{options};
	}
	if (codec == "htj2k" || codec == "htj2koptions") {
		if (has_jpeg_fields || has_jpegls_fields || has_jpegxl_fields) {
			throw std::invalid_argument(
			    "htj2k codec does not accept quality/near-lossless-error/distance/effort");
		}
		Htj2kOptions options{};
		if (opts.target_bpp) {
			options.target_bpp = *opts.target_bpp;
		}
		if (opts.target_psnr) {
			options.target_psnr = *opts.target_psnr;
		}
		if (opts.threads) {
			options.threads = *opts.threads;
		}
		if (opts.color_transform) {
			options.use_color_transform = *opts.color_transform;
		}
		if (options.target_bpp < 0.0 || options.target_psnr < 0.0) {
			throw std::invalid_argument("target-bpp/target-psnr must be >= 0");
		}
		if (options.threads < -1) {
			throw std::invalid_argument("threads must be -1, 0, or positive");
		}
		return CodecOptions{options};
	}
	if (codec == "jpegls" || codec == "jls" || codec == "jpeglsoptions") {
		if (has_j2k_fields || has_thread_field || has_jpeg_fields || has_jpegxl_fields) {
			throw std::invalid_argument(
			    "jpegls codec does not accept target-bpp/target-psnr/threads/color-transform/quality/distance/effort");
		}
		JpegLsOptions options{};
		if (opts.near_lossless_error) {
			options.near_lossless_error = *opts.near_lossless_error;
		}
		if (options.near_lossless_error < 0 || options.near_lossless_error > 255) {
			throw std::invalid_argument("near-lossless-error must be in [0, 255]");
		}
		return CodecOptions{options};
	}
	if (codec == "jpeg" || codec == "jpegbaseline" ||
	    codec == "jpeglossless" || codec == "jpegoptions") {
		if (has_j2k_fields || has_thread_field || has_jpegls_fields || has_jpegxl_fields) {
			throw std::invalid_argument(
			    "jpeg codec does not accept target-bpp/target-psnr/threads/color-transform/near-lossless-error/distance/effort");
		}
		JpegOptions options{};
		if (opts.quality) {
			options.quality = *opts.quality;
		}
		if (options.quality < 1 || options.quality > 100) {
			throw std::invalid_argument("quality must be in [1, 100]");
		}
		return CodecOptions{options};
	}
	if (codec == "jpegxl" || codec == "jxl" || codec == "jpegxloptions") {
		if (has_j2k_fields || has_jpeg_fields || has_jpegls_fields) {
			throw std::invalid_argument(
			    "jpegxl codec does not accept j2k/jpeg/jpegls option fields");
		}
		JpegXlOptions options{};
		if (transfer_syntax == "JPEGXLLossless"_uid) {
			options.distance = 0.0;
		}
		if (opts.distance) {
			options.distance = *opts.distance;
		}
		if (opts.effort) {
			options.effort = *opts.effort;
		}
		if (opts.threads) {
			options.threads = *opts.threads;
		}
		if (options.threads < -1) {
			throw std::invalid_argument("threads must be -1, 0, or positive");
		}
		if (!std::isfinite(options.distance) || options.distance < 0.0 ||
		    options.distance > 25.0) {
			throw std::invalid_argument("distance must be in [0, 25]");
		}
		if (options.effort < 1 || options.effort > 10) {
			throw std::invalid_argument("effort must be in [1, 10]");
		}
		if (transfer_syntax == "JPEGXLJPEGRecompression"_uid) {
			throw std::invalid_argument(
			    "JPEGXLJPEGRecompression transfer syntax is decode-only and not supported for encoding");
		}
		if (transfer_syntax == "JPEGXLLossless"_uid && options.distance != 0.0) {
			throw std::invalid_argument(
			    "JPEGXLLossless requires distance=0");
		}
		if (transfer_syntax == "JPEGXL"_uid && options.distance <= 0.0) {
			throw std::invalid_argument("JPEGXL requires distance > 0");
		}
		return CodecOptions{options};
	}

	throw std::invalid_argument(
	    "Unknown codec type; expected one of: auto, none, rle, jpeg, jpegls, j2k, htj2k, jpegxl");
}

} // namespace

int main(int argc, char** argv) {
	CliOptions opts{};
	bool help_shown = false;
	if (!parse_args(argc, argv, opts, help_shown)) {
		return help_shown ? 0 : 1;
	}

	try {
		auto file = dicom::read_file(opts.input_path);
		if (!file) {
			std::cerr << "dicomconv: failed to read input file: " << opts.input_path << '\n';
			return 1;
		}

		const auto transfer_syntax = resolve_transfer_syntax(opts.transfer_syntax_text);
		const auto codec_opt = build_codec_options(opts, transfer_syntax);
		file->set_transfer_syntax(transfer_syntax, codec_opt);
		file->write_file(opts.output_path);
		return 0;
	} catch (const std::exception& ex) {
		std::cerr << "dicomconv: " << ex.what() << '\n';
		return 1;
	}
}
