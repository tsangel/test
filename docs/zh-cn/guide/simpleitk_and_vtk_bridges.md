# SimpleITK 与 VTK 桥接

DicomSDL 可以先解码 DICOM 像素数据，再把结果交给 SimpleITK 或 VTK 对象，而不需要你大幅改写现有的 Python 工作流。

桥接模块有两个：

- `dicomsdl.simpleitk_bridge`
- `dicomsdl.vtk_bridge`

当你希望由 DicomSDL 负责 DICOM 解析、切片排序、像素解码和 rescale 处理，而后续处理继续放在 SimpleITK 或 VTK 中时，可以使用这些模块。

## 这些桥接模块做什么

这两个桥接模块共享同一套内部 volume 构建流程，只在最后的转换步骤上不同。

它们可以：

- 读取单文件 2D 图像，或基于文件的 DICOM 序列
- 读取可按 stack 处理的单文件 multiframe grayscale volume
- 对切片进行排序并组装为 volume
- 从 DICOM geometry 中映射 `spacing`、`origin` 和 `direction`
- 在 `to_modality_value=True` 时应用逐帧 modality rescale
- 将解码后的 volume 转换成 `SimpleITK.Image` 或 `vtkImageData`

它们并不是 `sitk.ReadImage(...)` 或 `vtkDICOMImageReader` 的完整 drop-in replacement。
更准确地说，它们是建立在 DicomSDL 之上的轻量桥接入口。

## 安装

只安装你需要的 toolkit 即可。

```bash
pip install "dicomsdl[numpy]" SimpleITK vtk
```

如果你只使用其中一个：

```bash
pip install "dicomsdl[numpy]" SimpleITK
pip install "dicomsdl[numpy]" vtk
```

## 共享行为

### 输入

可以传入以下任意一种：

- 包含基于文件的 DICOM 序列的目录
- 包含 2D 图像的单个 DICOM 文件
- 包含可按 stack 处理的 multiframe grayscale volume 的单个 DICOM 文件

本指南和示例主要针对单文件 2D 图像、可按 stack 处理的 multiframe grayscale volume，以及 file-per-slice 序列。
如果你的工作流依赖非 stack 型 multiframe localizer，或更复杂的 enhanced multi-frame 布局，建议单独验证。

### Geometry

桥接模块会保留以下 geometry 信息：

- `spacing`
- `origin`
- `direction`

在可用时，这些值会根据 DICOM 的 orientation 和 position metadata 计算得到。

对于可按 stack 处理的 3D 输入，桥接模块会按物理顺序对 volume geometry 进行规范化。

- slice 和 frame 会按它们在 base slice normal 上的投影排序
- 输出中的第一张 slice 是 projected position 最小的那一张
- `spacing[2]` 会始终保持为正值
- stack 方向由 `direction` 表达

如果文件型 series 中混入了不同 orientation 的切片，则只使用 dominant orientation group，
并忽略 localizer 一类的 outlier。

### 输出 dtype 策略

桥接模块不会把所有输出一律提升为 float，而是尽量保留更实用的 scalar type。

- 没有 rescale：保留 stored dtype
- `slope ~= 1` 且 intercept 为整数：保留整数 dtype
- fractional rescale：提升为 `float32`

因此，与各 toolkit 自带的 reader 相比，输出 dtype 可能不同。
这是因为原生 reader 可能采用不同的 scalar type 策略。

### Modality value

两个公开 API 默认都使用 `to_modality_value=True`。

这对 PET 尤其重要，因为逐切片 rescale 往往会影响结果。
如果你需要原始存储值，请传入 `to_modality_value=False`。

### 2D 图像

单文件 2D 图像会按单切片图像处理。
RGB 等 vector pixel data 会保留 component 轴。

### Multiframe volume

可按 stack 处理的单文件 multiframe grayscale volume 会以 `(z, y, x)` 的形式处理。
这里也会应用与 file-per-slice series 相同的物理顺序 canonicalization。
如果 frame position 不能对应到单一的 stack 方向，桥接模块目前不会猜测 geometry，而是抛出 `NotImplementedError`。

## SimpleITK

公开 API：

- `read_series_image(path, *, to_modality_value=True) -> SimpleITK.Image`
- `to_simpleitk_image(volume) -> SimpleITK.Image`

示例：

```python
from dicomsdl.simpleitk_bridge import read_series_image

image = read_series_image(
    r"..\sample\PT\00013 Torso PET AC OSEM",
    to_modality_value=True,
)

print(image.GetSize())
print(image.GetSpacing())
```

如果你想先拿到中间的解码 volume：

```python
from dicomsdl.simpleitk_bridge import read_series_volume, to_simpleitk_image

volume = read_series_volume(r"..\sample\PT\00013 Torso PET AC OSEM")
image = to_simpleitk_image(volume)
```

返回值是普通的 `SimpleITK.Image`，因此可以直接交给 SimpleITK 的 filter 和 pipeline 使用。

## VTK

公开 API：

- `read_series_image_data(path, *, to_modality_value=True, copy=False) -> vtkImageData`
- `to_vtk_image_data(volume, *, copy=False) -> vtkImageData`

示例：

```python
from dicomsdl.vtk_bridge import read_series_image_data

image = read_series_image_data(
    r"..\sample\PT\00013 Torso PET AC OSEM",
    to_modality_value=True,
    copy=False,
)

print(image.GetDimensions())
print(image.GetSpacing())
```

如果你想先拿到中间的解码 volume：

```python
from dicomsdl.vtk_bridge import read_series_volume, to_vtk_image_data

volume = read_series_volume(r"..\sample\PT\00013 Torso PET AC OSEM")
image = to_vtk_image_data(volume, copy=False)
```

`copy=False` 是优先使用 zero-copy 的快速路径。
如果你需要一个与原始 NumPy bridge 对象完全独立、由 VTK 拥有的副本，请使用 `copy=True`。

## 原生 reader 与桥接模块的取舍

如果你需要完整的 reader 类 API 兼容性，更适合使用 toolkit 自带的 reader。

如果你更在意下面这些点，则更适合使用 DicomSDL bridge：

- 希望由 DicomSDL 控制 decode 和 rescale 行为
- 希望在两个 toolkit 之间保持一致的 geometry 和 dtype 策略
- 希望得到 slice spacing 为正的 canonical physical-order 3D volume
- 希望在示例或 benchmark 中方便地比较 DicomSDL 与原生 reader

实务上可以记住：

- 常见的 SimpleITK DICOM series 路径本身就已经使用 GDCM
- 在 VTK 中，你可能会根据环境与 `vtkDICOMImageReader` 或 `vtkgdcm` 做比较
- bridge 会在暴露 volume 之前先 canonicalize stack order，
  因而 geometry 可能会与 native reader 的保存顺序表示有意不同

## 示例与笔记本

这个仓库已经包含可直接运行的示例和笔记本：

- `examples/python/itk_vtk/dicomsdl_to_simpleitk_pet_volume.py`
- `examples/python/itk_vtk/dicomsdl_to_vtk_pet_volume.py`
- `tutorials/basic_vtk3d.ipynb`
- `tutorials/timeit_itk.ipynb`
- `tutorials/timeit_vtk.ipynb`
- `benchmarks/python/benchmark_pet_volume_readers.py`
- `benchmarks/python/benchmark_wg04_readers.py`
