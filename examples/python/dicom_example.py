import dicomsdl


def main() -> None:
    df = dicomsdl.DicomFile.attach("/tmp/sample.dcm")
    print(f"Dicom path: {df.path}")


if __name__ == "__main__":
    main()
