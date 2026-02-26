import argparse

import dicomsdl as dicom


def main() -> None:
	parser = argparse.ArgumentParser(
	    description="Re-encode DICOM files with a reusable encoder context.")
	parser.add_argument("input", help="input DICOM path")
	parser.add_argument("output", help="output DICOM path")
	parser.add_argument(
	    "--transfer-syntax",
	    default="JPEG2000Lossless",
	    help="target transfer syntax keyword or UID")
	args = parser.parse_args()

	ctx = dicom.create_encoder_context(args.transfer_syntax)
	df = dicom.read_file(args.input)
	df.set_transfer_syntax(args.transfer_syntax, encoder_context=ctx)
	df.write_file(args.output)


if __name__ == "__main__":
	main()
