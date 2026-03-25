# 生成UID

本文档解释了 DicomSDL 中当前的 UID 生成和附加流程。

## 1.范围

- C++：
  - `dicom::uid::try_generate_uid()`
  - `dicom::uid::generate_uid()`
  - `dicom::uid::Generated::try_append(component)`
  - `dicom::uid::Generated::append(component)`
- Python：
  - `dicom.try_generate_uid()`
  - `dicom.generate_uid()`
  - `dicom.try_append_uid(base_uid, component)`
  - `dicom.append_uid(base_uid, component)`

## 2.C++ API

### 2.1 生成基本UID

```cpp
auto uid_opt = dicom::uid::try_generate_uid(); // std::optional<Generated>
auto uid = dicom::uid::generate_uid();         // Generated (throws on failure)
```

- 前缀：`dicom::uid::uid_prefix()`
- `generate_uid()` 将 UID 构建为 `<uid_prefix>.<random_numeric_suffix>`。
后缀由进程级随机数+单调序列生成。
- 输出：严格有效的 UID 文本（最多 64 个字符）

### 2.2 追加一个组件

```cpp
auto study = dicom::uid::generate_uid();
auto series = study.append(23);
auto inst = series.append(34);
```

- `try_append(component)` 返回 `std::optional<Generated>`
- `append(component)` 失败时抛出 `std::runtime_error`

### 2.3 现有UID文本为`Generated`

```cpp
auto base = dicom::uid::make_generated("1.2.840.10008");
if (base) {
    auto extended = base->append(7);
}
```

## 3.Python API

```python
import dicomsdl as dicom

study = dicom.generate_uid()
series = dicom.append_uid(study, 23)
inst = dicom.append_uid(series, 34)

safe = dicom.try_append_uid("1.2.840.10008", 7)  # Optional[str]
```

- `append_uid(...)` 因无效输入/失败而引发
- `try_append_uid(...)` 失败时返回 `None`

## 4. 追加行为

对于 `append` / `try_append`：

1. 直接路径：
- 首先尝试 `<base_uid>.<component>`。
- 如果有效且 <= 64 个字符，则返回它。

2.后备路径（当直接追加不适合时）：
- 保留 `base_uid` 的前 30 个字符。
- 如果最后一个字符不是 `.`，则添加 `.`。
- 附加1个U96十进制块。
- 结果由严格的 UID 验证器重新验证。

### 重要提示

回退输出故意是不确定的：

- 仍然基于`component`和`base_uid`，
- 但也混合了进程级随机数 + 原子序列。

因此，对于相同的 `(base_uid, component)`，后备后缀在不同的调用中可能不同。

## 5. 失败模型总结

- `generate_uid()`：
- 失败时抛出
- `try_generate_uid()`：
- 失败时返回 `None` / `std::nullopt`
- `append_uid()` / `Generated::append()`：
- 失败时抛出
- `try_append_uid()` / `Generated::try_append()`：
- 失败时返回 `None` / `std::nullopt`

## 6.实用推荐

- 申请流程：
- 使用 `generate_uid()` 构建基础
- 通过 `append(...)` 派生子 UID
- 如果您的输入基础 UID 可能不受信任，请使用：
- Python 中的 `try_append_uid(...)`
- C++ 中的 `try_append(...)`
