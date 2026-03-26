# Tutorials

This directory carries forward the old `dicomsdl` tutorial-style workflow with
the current API surface.

Included sample files:

- `CT1_UNC`
- `CT2_JLSN`

Included notebooks:

- `basic1.ipynb`
  - metadata access, pixel decode, quick image preview, and dataset dump
- `basic_vtk3d.ipynb`
  - load a 3D series directory or a single multiframe file with `vtk`,
    `vtkgdcm`, and `dicomsdl.vtk_bridge`, then compare geometry and array layout
- `timeit_test.ipynb`
  - simple `dicomsdl`, `pydicom`, and `gdcm` decode timing sketch
- `timeit_itk.ipynb`
  - quick `%timeit` codelets for `SimpleITK` and `dicomsdl.simpleitk_bridge`
- `timeit_vtk.ipynb`
  - quick `%timeit` codelets for `vtk`, `vtkgdcm`, and `dicomsdl.vtk_bridge`

To run the notebooks locally, install at least:

```bash
pip install "dicomsdl[numpy,pil]" jupyter
```

For the timing notebook, also install:

```bash
pip install pydicom python-gdcm
```

For the VTK notebooks, install:

```bash
pip install vtk "dicomsdl[numpy]"
```

To run the `vtkgdcm` cells, use a Python environment that already has
`vtkgdcm` available, such as the local `dicom-bench` conda environment.

For the ITK timing notebook, install:

```bash
pip install SimpleITK "dicomsdl[numpy]"
```

The sample DICOM files come from the older `dicomsdl` tutorial set and are kept
here so the notebooks can be opened and tried directly from a source checkout.
