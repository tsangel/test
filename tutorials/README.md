# Tutorials

This directory carries forward the old `dicomsdl` tutorial-style workflow with
the current API surface.

Included sample files:

- `CT1_UNC`
- `CT2_JLSN`

Included notebooks:

- `basic1.ipynb`
  - metadata access, pixel decode, quick image preview, and dataset dump
- `timeit_test.ipynb`
  - simple `dicomsdl`, `pydicom`, and `gdcm` decode timing sketch

To run the notebooks locally, install at least:

```bash
pip install "dicomsdl[numpy,pil]" jupyter
```

For the timing notebook, also install:

```bash
pip install pydicom python-gdcm
```

The sample DICOM files come from the older `dicomsdl` tutorial set and are kept
here so the notebooks can be opened and tried directly from a source checkout.
