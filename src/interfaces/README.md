# 共享平台接口

`global_hotkey.h`：F1 注册结果与回调；`display_topology.h`：显示器 ID、逐屏逻辑矩形、DPR、布局版本与光标定位；`capture_provider.h`：权限、异步单帧、不可变源像素及数值错误；`presentation_observer.h`：目标窗口刷新代理与取消句柄。`QImage` 自带物理尺寸、stride、sRGB 与 DPR，`CaptureFrame` 记录捕获时刻。

接口只含 Qt / C++ 值类型；不暴露 CGImage、EventHotKeyRef、NSWindow 或 HWND。系统对象只在平台目录解析与释放。Windows 提供可编译桩并如实反馈未实现。

`tray_icon.h`：`createTrayIcon()` 从内嵌确认稿返回含 1x / 2x 档位的 QIcon。macOS 为黑色模板并带 mask 标记，Windows 为彩色非模板；平台选择与资源路径留在 `platform/`，共享装配层不识别系统或读取磁盘资源。
