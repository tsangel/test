from __future__ import annotations

import argparse
import sys

import dicomsdl as dicom

TRANSFER_SYNTAX_ALIASES = {
    "implicit": "ImplicitVRLittleEndian",
    "implicitle": "ImplicitVRLittleEndian",
    "implicitvrlittleendian": "ImplicitVRLittleEndian",
    "explicit": "ExplicitVRLittleEndian",
    "explicitle": "ExplicitVRLittleEndian",
    "explicitvrlittleendian": "ExplicitVRLittleEndian",
    "native": "ExplicitVRLittleEndian",
    "raw": "ExplicitVRLittleEndian",
    "uncompressed": "ExplicitVRLittleEndian",
    "deflate": "DeflatedExplicitVRLittleEndian",
    "deflatedexplicitvrlittleendian": "DeflatedExplicitVRLittleEndian",
    "encapsulateduncompressed": "EncapsulatedUncompressedExplicitVRLittleEndian",
    "encapraw": "EncapsulatedUncompressedExplicitVRLittleEndian",
    "encapuncompressed": "EncapsulatedUncompressedExplicitVRLittleEndian",
    "encapsulateduncompressedexplicitvrlittleendian": "EncapsulatedUncompressedExplicitVRLittleEndian",
    "rle": "RLELossless",
    "rlelossless": "RLELossless",
    "jpeg": "JPEGBaseline8Bit",
    "jpegbaseline": "JPEGBaseline8Bit",
    "jpegbaseline8bit": "JPEGBaseline8Bit",
    "jpeglossless": "JPEGLosslessSV1",
    "jpeglosslesssv1": "JPEGLosslessSV1",
    "jpegls": "JPEGLSLossless",
    "jls": "JPEGLSLossless",
    "jpeglslossless": "JPEGLSLossless",
    "jpeglsnear": "JPEGLSNearLossless",
    "jpeglsnearlossless": "JPEGLSNearLossless",
    "jlsnl": "JPEGLSNearLossless",
    "j2k": "JPEG2000",
    "jpeg2k": "JPEG2000",
    "jpeg2000": "JPEG2000",
    "j2klossless": "JPEG2000Lossless",
    "jpeg2klossless": "JPEG2000Lossless",
    "jpeg2000lossless": "JPEG2000Lossless",
    "j2kmc": "JPEG2000MC",
    "jpeg2kmc": "JPEG2000MC",
    "jpeg2000mc": "JPEG2000MC",
    "j2kmclossless": "JPEG2000MCLossless",
    "jpeg2kmclossless": "JPEG2000MCLossless",
    "jpeg2000mclossless": "JPEG2000MCLossless",
    "htj2k": "HTJ2K",
    "htj2klossless": "HTJ2KLossless",
    "htj2klosslessrpcl": "HTJ2KLosslessRPCL",
}

HTJ2K_TRANSFER_SYNTAX_KEYWORDS = {
    "htj2k",
    "htj2klossless",
    "htj2klosslessrpcl",
}

HTJ2K_TRANSFER_SYNTAX_UID_VALUES = {
    "1.2.840.10008.1.2.4.201",
    "1.2.840.10008.1.2.4.202",
    "1.2.840.10008.1.2.4.203",
}


def normalize_token(text: str) -> str:
    return "".join(ch.lower() for ch in text if ch not in "_- \t")


def canonical_codec_name(codec_name: str) -> str:
    normalized = normalize_token(codec_name)
    if normalized == "" or normalized == "auto":
        return "auto"
    if normalized in {"none", "nocompression", "native", "uncompressed"}:
        return "none"
    if normalized in {"rle", "rlelossless"}:
        return "rle"
    if normalized in {"jpeg", "jpegbaseline", "jpeglossless", "jpegoptions"}:
        return "jpeg"
    if normalized in {"jpegls", "jls", "jpeglsoptions"}:
        return "jpegls"
    if normalized in {"j2k", "jpeg2k", "jpeg2000", "j2koptions"}:
        return "j2k"
    if normalized in {"htj2k", "htj2koptions"}:
        return "htj2k"
    raise ValueError("codec must be one of: auto, none, rle, jpeg, jpegls, j2k, htj2k")


def is_htj2k_transfer_syntax(uid: dicom.Uid) -> bool:
    keyword = uid.keyword or ""
    if isinstance(keyword, str):
        if normalize_token(keyword) in HTJ2K_TRANSFER_SYNTAX_KEYWORDS:
            return True
    return uid.value in HTJ2K_TRANSFER_SYNTAX_UID_VALUES


def transfer_syntax_help_epilog() -> str:
    lines = [
        "Examples:",
        "  dicomconv in.dcm out.dcm jpeg --quality 92",
        "  dicomconv in.dcm out.dcm jpegls-near-lossless --near-lossless-error 3",
        "  dicomconv in.dcm out.dcm jpeg2k --target-psnr 45 --threads -1",
        "  dicomconv in.dcm out.dcm htj2k-lossless --codec htj2k --no-color-transform",
        "",
        "Transfer syntax shortcuts:",
        "  jpeg -> JPEGBaseline8Bit",
        "  jpeglossless -> JPEGLosslessSV1",
        "  jpegls / jpegls-near-lossless",
        "  jpeg2k / jpeg2k-lossless / jpeg2k-mc",
        "  htj2k / htj2k-lossless",
        "  rle -> RLELossless",
        "",
        "Available Transfer Syntax UIDs (keyword = UID):",
    ]
    for uid in dicom.transfer_syntax_uids():
        keyword = uid.keyword or "-"
        lines.append(f"  {keyword} = {uid.value}")
    return "\n".join(lines)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="dicomconv",
        description="Change DICOM Transfer Syntax UID and write the result.",
        epilog=transfer_syntax_help_epilog(),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("input", help="Input DICOM file path")
    parser.add_argument("output", help="Output DICOM file path")
    parser.add_argument(
        "transfer_syntax",
        help="Transfer syntax keyword/UID value, or shortcut alias (jpeg, jpeg2k, htj2k, ...)",
    )
    parser.add_argument(
        "--codec",
        help="Codec option type: auto|none|rle|jpeg|jpegls|j2k|htj2k (default: auto)",
    )
    parser.add_argument("--quality", type=int, help="JPEG quality [1,100]")
    parser.add_argument("--near-lossless-error", type=int, help="JPEG-LS NEAR [0,255]")
    parser.add_argument("--target-psnr", type=float, help="JPEG2000/HTJ2K target PSNR")
    parser.add_argument("--target-bpp", type=float, help="JPEG2000/HTJ2K target bits-per-pixel")
    parser.add_argument(
        "--threads",
        type=int,
        help="JPEG2000/HTJ2K encoder threads (-1:auto, 0:library default, >0:count)",
    )
    color_transform_group = parser.add_mutually_exclusive_group()
    color_transform_group.add_argument(
        "--color-transform",
        dest="color_transform",
        action="store_true",
        help="Enable JPEG2000/HTJ2K MCT color transform",
    )
    color_transform_group.add_argument(
        "--no-color-transform",
        dest="color_transform",
        action="store_false",
        help="Disable JPEG2000/HTJ2K MCT color transform",
    )
    parser.set_defaults(color_transform=None)
    return parser


def resolve_transfer_syntax_uid(text: str) -> dicom.Uid:
    lookup_text = TRANSFER_SYNTAX_ALIASES.get(normalize_token(text), text)
    uid = dicom.lookup_uid(lookup_text)
    if uid is None:
        raise ValueError(f"Unknown DICOM UID: {text}")
    if uid.type != "Transfer Syntax":
        raise ValueError(f"UID is not a Transfer Syntax UID: {text}")
    return uid


def build_codec_opt(args: argparse.Namespace, transfer_syntax: dicom.Uid) -> dict | None:
    has_j2k_fields = (
        args.target_bpp is not None
        or args.target_psnr is not None
        or args.threads is not None
        or args.color_transform is not None
    )
    has_jpeg_fields = args.quality is not None
    has_jpegls_fields = args.near_lossless_error is not None
    has_any_fields = has_j2k_fields or has_jpeg_fields or has_jpegls_fields

    if args.target_bpp is not None and args.target_bpp < 0:
        raise ValueError("--target-bpp must be >= 0")
    if args.target_psnr is not None and args.target_psnr < 0:
        raise ValueError("--target-psnr must be >= 0")
    if args.threads is not None and args.threads < -1:
        raise ValueError("--threads must be -1, 0, or positive")
    if args.quality is not None and (args.quality < 1 or args.quality > 100):
        raise ValueError("--quality must be in [1, 100]")
    if args.near_lossless_error is not None and (
        args.near_lossless_error < 0 or args.near_lossless_error > 255
    ):
        raise ValueError("--near-lossless-error must be in [0, 255]")

    if args.codec:
        codec_type = canonical_codec_name(args.codec)
    elif has_jpeg_fields:
        codec_type = "jpeg"
    elif has_jpegls_fields:
        codec_type = "jpegls"
    elif has_j2k_fields:
        codec_type = "htj2k" if is_htj2k_transfer_syntax(transfer_syntax) else "j2k"
    else:
        codec_type = "auto"

    if codec_type == "auto":
        if has_any_fields:
            raise ValueError("codec auto does not accept extra option fields")
        return None

    codec_opt: dict[str, object] = {"type": codec_type}
    if args.quality is not None:
        codec_opt["quality"] = args.quality
    if args.near_lossless_error is not None:
        codec_opt["near_lossless_error"] = args.near_lossless_error
    if args.target_psnr is not None:
        codec_opt["target_psnr"] = args.target_psnr
    if args.target_bpp is not None:
        codec_opt["target_bpp"] = args.target_bpp
    if args.threads is not None:
        codec_opt["threads"] = args.threads
    if args.color_transform is not None:
        codec_opt["color_transform"] = args.color_transform
    return codec_opt


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)

    try:
        transfer_syntax = resolve_transfer_syntax_uid(args.transfer_syntax)
        df = dicom.read_file(args.input)
        codec_opt = build_codec_opt(args, transfer_syntax)
        if codec_opt is None:
            df.set_transfer_syntax(transfer_syntax)
        else:
            df.set_transfer_syntax(transfer_syntax, codec_opt=codec_opt)
        df.write_file(args.output)
        return 0
    except Exception as exc:
        print(f"dicomconv: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
