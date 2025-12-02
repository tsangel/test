# Pixel Decoder Design

## Goals
- Provide a common, low/zero-copy path to decode DICOM Pixel Data across codecs (RAW, JPEG, JPEG-LS, JPEG 2000/HTJ2K, JPEG XL, …).
- Let callers pre-allocate/reuse buffers and optionally apply rescale/VOI/windowing in one pass.
- Preserve codec extensibility via a registry/factory while keeping ergonomic `DataSet` member helpers.

## Core Types
- `FrameInfo`
  - `rows`, `cols`, `bits_allocated`, `samples_per_pixel`
  - `signed_samples`, `planar_config`, `lossless`
  - Helpers: 
    - `StrideInfo compute_strides(const DecodeOptions* opts = nullptr) const;` // uses opts->output_alignment and output_format to infer row/frame bytes
      - `row_bytes` : aligned row stride
      - `frame_bytes` : row_bytes * rows
- `DecodeStatus` = `{ ok, insufficient_buffer, unsupported_ts, invalid_frame, corrupt_stream }`
- `DecodeOptions`
  - `convert_to_rgb` (YBR→RGB), `keep_planar`, `output_stride`
  - `apply_rescale` (default `true`)  // use DataSet Rescale Slope/Intercept or Modality LUT
  - `apply_voi` (default `false`)     // use DataSet VOI LUT or Window
  - `output_format` (default `OutputPixelFormat::auto`)
      - auto ⇒ if rescale is applied → float32; else stored bits (uint8/uint16)
      - other options: uint8, int16, int32, float32 (single source of truth for final pixel type after rescale/VOI)
  - `output_alignment` (default `1`)   // row stride alignment in bytes

## Decoder Abstraction & Registry
```cpp
class PixelDecoder {
public:
  virtual ~PixelDecoder() = default;
  virtual FrameInfo frame_info(const DataSet&, size_t frame) const = 0; // probe-only
  virtual DecodeStatus decode_into(const DataSet&, std::span<std::byte> dst,
                                   size_t frame, const DecodeOptions&) const = 0;
};
using DecoderFactory = std::function<std::unique_ptr<PixelDecoder>()>;
void register_decoder(std::string_view ts_uid, DecoderFactory);
std::unique_ptr<PixelDecoder> make_decoder(const DataSet&);
```
- Each codec registers once (e.g., RAW LE, JPEG baseline, J2K, JLS, JXL).
- `make_decoder(ds)` selects by Transfer Syntax UID; callers use `DataSet` helpers.

## DataSet Member Helpers (thin wrappers)
```cpp
FrameInfo DataSet::frame_info(size_t frame = 0) const;                // delegates to decoder->frame_info
DecodeStatus DataSet::decode_into(std::span<std::byte> dst,
                                  size_t frame = 0,
                                  DecodeOptions opts = {}) const;     // fill caller buffer, no extra copy
std::vector<std::byte> DataSet::decode_pixels(size_t frame = 0,
                                              DecodeOptions opts = {}) const; // convenience allocating API
```
- Keeps call sites concise (`ds.frame_info(i)`) while implementation remains testable as free functions.

## Probe Behavior
- `frame_info()` must read the codestream header (first fragment) to obtain authoritative `rows/cols/bits/components/signed/lossless`.
- Compare against DataSet tags ((0028,0010)/(0011)/(0100)/(0103)/(0002)); on mismatch, warn or apply policy (codec-first vs tag-first).
- For multi-frame, probe the requested frame (defensive against vendor bugs).

## Buffer Strategy
- `compute_strides(const DecodeOptions* opts = nullptr)` returns `{row_bytes, frame_bytes}` using `align_up(effective_stride, output_alignment)`; default alignment=1 for tight packing. Pixel bytes are inferred from `opts` (or defaults): stored bits, auto rescale→float32, or explicit `output_format`.
- If `output_stride > 0`, required bytes become `required = output_stride * rows`; `decode_into` should validate against `max(effective_stride, output_stride) * rows`.
- `decode_into` writes directly into the provided span; returns `insufficient_buffer` if too small.
- `output_stride` allows padded rows (e.g., 8/16/32-byte alignment, GPU pitch). Default is tight packing. `output_alignment` can be used instead for stride calculation.

## Postprocess Pipeline (optional)
1) Decode raw samples.  
2) Apply Rescale (slope/intercept) if enabled.  
3) Apply VOI: window or LUT (values stay in current output type).  
4) Final pixel type = `output_format` (single source):  
   - auto: rescale on → float32; rescale off → stored bits (uint8/uint16).  
   - explicit: uint8 / int16 / int32 / float32.  
- `bytes_needed` and `stride()` use the chosen `output_format`.

### Postprocess defaults
- DICOMSDL는 DataSet에 있는 기본 Rescale/VOI만 적용한다.  
- 호출자가 다른 스케일/윈도우를 원하면 `apply_rescale=false` 또는 `apply_voi=false`로 끄고, raw/float 결과를 받아 외부에서 처리.  
- 동작
  - Rescale: Rescale Slope/Intercept (0028,1053/1052) 또는 Modality LUT Sequence(있으면 LUT 우선).  
  - VOI: VOI LUT Sequence(0028,3010) 우선, 없으면 Window Center/Width(0028,1050/1051).  
  - `to_uint8`: VOI 적용 시 기본 true(표시용 8-bit), 필요 없으면 false로 끔.  
  - `apply_rescale=false` & `apply_voi=false` → stored pixel 그대로.

## Python Binding Sketch
- `ds.frame_info(frame=0) -> FrameInfo`
- `ds.decode_into(memoryview, frame=0, convert_to_rgb=True, keep_planar=False, apply_rescale=True, apply_voi=True, to_uint8=None, output_stride=None)`
- `ds.decode_pixels(frame=0, ...) -> bytes`
- `FrameInfo` exposed as a simple dataclass-like object.

## Error Handling Guidelines
- Unsupported TS → `unsupported_ts`  
- Bad frame index → `invalid_frame`  
- Corrupt codestream/header → `corrupt_stream`  
- Buffer too small → `insufficient_buffer`  
- Prefer status enums; reserve exceptions for programmer errors (e.g., null span).

## Multi-Frame Notes
- Standard requires constant `Rows/Columns/BitsAllocated/SamplesPerPixel` per SOP, but real-world files may violate; keep per-frame probe as guard.  
- Per-Frame Functional Groups may change geometry/timing; decoder focuses on pixel layout, caller should read FG for spatial metadata.

## Extensibility / Open Points
- Consider `prepare(const DataSet&)` hook for future streaming optimizations.  
- Expose mismatch policy {warn-and-continue, prefer-tags, prefer-codestream, fail}.  
- Thread safety: registry must be thread-safe; decoder instances can be per-call.

## Usage Examples
- **1) Raw stored pixels (no postprocess)**
  ```cpp
  DecodeOptions opt;
  opt.apply_rescale = false;
  opt.apply_voi = false;
  ds.decode_into(buf, frame_idx, opt);
  ```

- **2) Default: DataSet Rescale on, VOI off**
  ```cpp
  DecodeOptions opt; // defaults: apply_rescale=true, apply_voi=false
  ds.decode_into(buf, frame_idx, opt);
  ```

- **2b) Default + padded stride (e.g., 16-byte alignment, float32 output)**
  ```cpp
  DecodeOptions opt; // apply_rescale=true, apply_voi=false (default)
  opt.output_format = OutputPixelFormat::float32; // default when rescale is on
  opt.output_alignment = 16;      // row alignment

  auto info = ds.frame_info(frame_idx);
  // compute_strides signature: compute_strides(const DecodeOptions* opts)
  auto s = info.compute_strides(&opt); // uses alignment and output_format from opts

  std::vector<std::byte> buf(s.frame_bytes);
  opt.output_stride = s.row_bytes;      // tell decoder about padded stride

  ds.decode_into(buf, frame_idx, opt);
  ```

- **3) DataSet Rescale + VOI (표시용)**
  ```cpp
  DecodeOptions opt;
  opt.apply_voi = true;      // use VOI LUT/Window from tags
  ds.decode_into(buf, frame_idx, opt);
  ```

### Python Example (NumPy buffer, padded stride)
```python
import dicomsdl as dicom
import numpy as np

ds = dicom.read_file("image.dcm")
opt = dicom.DecodeOptions()
# defaults: apply_rescale=True, apply_voi=False
opt.output_format = dicom.OutputPixelFormat.float32   # default when rescale is on
opt.output_alignment = 16                             # 16-byte row alignment

info = ds.frame_info(frame=0)
s = info.compute_strides(opt)         # uses output_format + alignment from opt

# allocate aligned buffer via numpy (shape will match interleaved layout)
buf = np.empty(s.frame_bytes, dtype=np.uint8)  # raw byte buffer
mv = memoryview(buf)

# output_stride can be left as 0; decoder may use s.row_bytes when zero
ds.decode_into(mv, frame=0, options=opt)

# reinterpret as float32 image (rows, cols, channels)
img = np.frombuffer(buf, dtype=np.float32)
img = img.reshape(info.rows, info.cols, info.samples_per_pixel)
```
