# CLI Tools

`dicomsdl` exposes three user-facing command-line tools:

- `dicomdump`: print a human-readable DICOM dump
- `dicomshow`: open one decoded frame through Pillow
- `dicomconv`: change transfer syntax and write a new file

## How to install the commands

### From a Python wheel

```bash
pip install dicomsdl
```

This installs the console scripts `dicomdump`, `dicomconv`, and `dicomshow`.

`dicomshow` depends on the Pillow preview path, so in practice you usually want:

```bash
pip install "dicomsdl[numpy,pil]"
```

### From a source build

If you build the C++ examples with `-DDICOM_BUILD_EXAMPLES=ON`, the build tree
contains `dicomdump` and `dicomconv` binaries:

```bash
cmake -S . -B build -DDICOM_BUILD_EXAMPLES=ON
cmake --build build

./build/dicomdump sample.dcm
./build/dicomconv in.dcm out.dcm ExplicitVRLittleEndian
```

There is no separate C++ build-tree `dicomshow` executable. `dicomshow` is the
Python console script entry point.

## `dicomdump`

Use `dicomdump` when you want a readable text dump of one or more DICOM files.
It loads each file and prints the result of `DicomFile.dump(...)`.

### `dicomdump` usage

```bash
dicomdump [--max-print-chars N] [--no-offset] [--with-filename] <file> [file...]
```

### `dicomdump` reference

Positional arguments:

| Argument | Meaning |
| --- | --- |
| `paths` | One or more input paths. Wildcard patterns such as `*.dcm` are expanded by the CLI. |

Options:

| Option | Meaning |
| --- | --- |
| `--max-print-chars N` | Truncate long printable values after `N` characters. Default: `80`. |
| `--no-offset` | Hide the `OFFSET` column. |
| `--with-filename` | Prefix each output line with `filename:`. This is already enabled by default when multiple inputs are given. |

### `dicomdump` examples

```bash
dicomdump sample.dcm
dicomdump a.dcm b.dcm
dicomdump --no-offset --max-print-chars 120 sample.dcm
dicomdump "*.dcm"
```

When multiple inputs are given, `dicomdump` prefixes each output line with the
source filename so mixed output stays readable.

## `dicomshow`

Use `dicomshow` for quick visual inspection from a shell. It reads one DICOM
file, converts one frame with `to_pil_image(frame=...)`, and calls
`Pillow.Image.show()`.

### `dicomshow` usage

```bash
dicomshow [--frame N] <input.dcm>
```

### `dicomshow` reference

Positional arguments:

| Argument | Meaning |
| --- | --- |
| `input` | Input DICOM file path. |

Options:

| Option | Meaning |
| --- | --- |
| `--frame N` | Zero-based frame index to preview. Default: `0`. |

### `dicomshow` notes

- `dicomshow` is a quick preview tool, not a diagnostic viewer.
- It depends on your local GUI/viewer association and may not work in headless
  environments.
- Install `dicomsdl[numpy,pil]` if Pillow or NumPy is not already available.

### `dicomshow` example

```bash
dicomshow sample.dcm
dicomshow --frame 5 multiframe.dcm
```

## `dicomconv`

Use `dicomconv` when you want a file-to-file transfer syntax change from a
shell script or terminal session.

Internally it reads the input file, applies `set_transfer_syntax(...)`, and
writes the result to a new path.

### `dicomconv` usage

```bash
dicomconv <input.dcm> <output.dcm> <transfer-syntax> [options]
```

`<transfer-syntax>` may be:

- a transfer syntax keyword such as `ExplicitVRLittleEndian`
- a dotted UID string such as `1.2.840.10008.1.2`
- a shortcut alias such as `jpeg`, `jpeg2k`, `htj2k-lossless`, or `jpegxl`

### `dicomconv` reference

Positional arguments:

| Argument | Meaning |
| --- | --- |
| `input` | Source DICOM file path. |
| `output` | Destination DICOM file path. |
| `transfer_syntax` | Target transfer syntax keyword, dotted UID string, or shortcut alias. |

Options:

| Option | Applies to | Meaning |
| --- | --- | --- |
| `--codec {auto,none,rle,jpeg,jpegls,j2k,htj2k,jpegxl}` | all | Force the option family instead of inferring it from the target transfer syntax. |
| `--quality N` | `jpeg` | JPEG quality in `[1, 100]`. |
| `--near-lossless-error N` | `jpegls` | JPEG-LS `NEAR` in `[0, 255]`. |
| `--target-psnr V` | `j2k`, `htj2k` | Target PSNR. |
| `--target-bpp V` | `j2k`, `htj2k` | Target bits-per-pixel. |
| `--threads N` | `j2k`, `htj2k`, `jpegxl` | Encoder thread setting. `-1` means auto, `0` means library default. |
| `--color-transform` | `j2k`, `htj2k` | Enable MCT color transform. |
| `--no-color-transform` | `j2k`, `htj2k` | Disable MCT color transform. |
| `--distance V` | `jpegxl` | JPEG-XL distance in `[0, 25]`. `0` means lossless. |
| `--effort N` | `jpegxl` | JPEG-XL effort in `[1, 10]`. |

Run `dicomconv -h` for the full help text, examples, and the current list of
supported target transfer syntaxes.

### `dicomconv` examples

```bash
dicomconv in.dcm out.dcm ExplicitVRLittleEndian
dicomconv in.dcm out.dcm 1.2.840.10008.1.2
dicomconv in.dcm out.dcm jpeg --quality 92
dicomconv in.dcm out.dcm jpegls-near-lossless --near-lossless-error 3
dicomconv in.dcm out.dcm jpeg2k --target-psnr 45 --threads -1
dicomconv in.dcm out.dcm htj2k-lossless --no-color-transform
dicomconv in.dcm out.dcm jpegxl --distance 1.5 --effort 7 --threads -1
```

## Exit status

All three commands return:

- `0` on success
- `1` if an input, parse, decode, encode, or write step fails

Errors are printed to standard error with a tool-specific prefix such as
`dicomdump:`, `dicomshow:`, or `dicomconv:`.

## Related docs

- [Installation](installation.md)
- [File I/O](file_io.md)
- [Pixel Decode](pixel_decode.md)
- [Pixel Encode](pixel_encode.md)
- [Pixel Encode Constraints](../reference/pixel_encode_constraints.md)
