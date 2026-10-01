# 共享平台契约

当前仅以 `platformName()` 验证共享声明与双端实现的编译连接。

后续 `CaptureProvider`、`WindowCatalog`、`GlobalHotkey`、`DisplayTopology`、`PermissionService`、`PinWindowPolicy` 的接口也定义在此处，具体实现放入 `platform/macos/` 或 `platform/windows/`。本轮不提前虚构这些功能的方法或成功结果。

跨层数据必须明确显示器 ID、坐标空间、物理像素尺寸 / stride、色彩信息、捕获时间与布局版本。系统句柄与系统头文件只能存在于平台目录。
