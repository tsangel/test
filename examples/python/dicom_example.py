import dicomsdl as dicom


def main() -> None:
	dataset = dicom.read_file("/tmp/sample.dcm")
	print(f"Dicom path: {dataset.path}")


if __name__ == "__main__":
	main()
