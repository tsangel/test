# 常问问题

## 我应该从`DicomFile`开始还是`DataSet`开始？

当您关心文件/会话状态、像素解码或序列化时，请从 `DicomFile` 开始。当您专注于元数据读写时，从 `DataSet` 开始。

## 什么时候应该使用 `get_value()` 而不是 `ds[...]`？

使用 `get_value()` 进行一次性类型读取。当您还需要 `tag`、`vr`、`length` 或原始元素元数据时，请使用 `ds[...]`。

## 为什么缺失的元素并不总是引发？

dicomsdl 在很多地方都刻意让 `DataElement` 访问不会立刻抛出异常，这样您就可以安全地做真值判断并继续链式查找。

## 为什么零长度值看起来与缺失值不同？

因为零长度意味着“存在但为空”，而缺失则意味着该元素根本不存在。

## 我应该在哪里寻找嵌套序列路径规则？

请参见[序列和路径](sequence_and_paths.md)和[标签路径查找语义](../reference/tag_path_lookup.md)。

## 我应该在哪里寻找像素编码限制？

请参阅 [像素编码](pixel_encode.md) 了解概述，并参阅 [像素编码约束](../reference/pixel_encode_constraints.md) 了解确切的契约。
