import dicomsdl as dicom


def main() -> None:
	df = dicom.DicomFile.attach("/tmp/sample.dcm")
    print(f"Dicom path: {df.path}")


if __name__ == "__main__":
    main()
