#include <memory>

#include <pybind11/pybind11.h>

#include <dicom.h>

namespace py = pybind11;

using dicom::DicomFile;

PYBIND11_MODULE(_dicomsdl, m) {
	m.doc() = "pybind11 bindings for DicomFile";

	py::class_<DicomFile, std::unique_ptr<DicomFile>>(m, "DicomFile")
		.def_static("attach", &DicomFile::attach, py::arg("path"), "Attach to a DICOM file at the given path")
		.def_property_readonly("path", &DicomFile::path, "Return the stored file path");
}
