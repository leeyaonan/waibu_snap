# 源码结构

| 目录 | 职责 | 初始化状态 |
| --- | --- | --- |
| `app/` | 应用装配、启动、退出 | 托盘生命周期、受控测试入口；`app_settings` 的 INI 存储与注册 / 落盘协调，`hotkey_rules` 的无平台校验 |
| `interfaces/` | 共享平台契约 | 事务式热键、显示器定位、单帧捕获与呈现观察；`window_enumerator` 的前到后全局逻辑外框列表 |
| `platform/macos/` | Objective-C++ / 后续 AppKit、ScreenCaptureKit | Carbon 热键映射与事务式替换、CGWindowList 窗口枚举、ScreenCaptureKit / AppKit 单帧原型及外部测量探针 |
| `platform/windows/` | MSVC C++ / 后续 Win32、DXGI | 可编译桩，如实返回未实现 |
| `core/` | 图像与标注核心 | 半开物理像素框选、移动与八方向调整；`window_snapping` 的全局转屏内裁剪、前到后命中与像素边缘取整 |
| `session/` | 会话与文档状态 | 单会话锁、单调时钟、尺寸与时间 JSONL |
| `ui/` | Qt Widgets 视图与桌面入口 | 冻结画面悬停 / 单击吸附、框选 / 移动 / 调整；复制 / 保存 / 取消工具栏与保存面板焦点管理；`settings_dialog` 的单组合输入、中文反馈和保存 / 取消；无默认主窗口 |
| `output/` | 输出与生命周期 | 冻结帧裁剪、DPR=1 输出、Qt 剪贴板与 PNG 原子提交、文件名补后缀和冲突避让；无自动保存 |

平台相关源码只放 `platform/`，共享接口不暴露系统句柄。CMake 在平台目录选择实现，共享应用不使用平台宏判断。业务依赖方向为应用装配 → 共享模块 / 平台实现，平台实现 → 共享接口；不得从核心反向依赖视图或具体平台。

`OverlayActions` 提供复制与路径选择的行为缝，默认走真实 QClipboard 和原生 QFileDialog；`exportToPath` 分离路径导出与面板。输出只裁剪 `frame_.pixels`，不捕获 QWidget 显示层；保存成功保留会话，复制成功以结果码 10 结束。工具栏和短暂反馈计时器归会话所有，空闲无隐藏预建选区或轮询。

窗口列表在 `captureCompleted` 成功后查询一次，经 `localWindowRects` 裁剪再注入覆盖层；悬停像素矩形同时用于绘制与只读断言。手势最大位移不超过 3 个逻辑像素才确认按下处窗口，手动拖选开始即清除高亮；已有选区外部单击只清除，下一次单击才能重新吸附。高亮、选区与工具栏均不进入冻结帧输出。

`HotkeySettings` 使用 `GlobalHotkey` 契约协调启动回退及改键，不依赖托盘或真实平台注册，可用替身验证失败保旧与成功后落盘。macOS 映射 Qt Control → Command、Meta → 物理 Control、Alt → Option、Shift → Shift，Carbon 独占注册新键后释放旧键；退出显式 `unregister`，析构再次调用安全。`SettingsDialog` 的校验和元数据可 offscreen 测试，设置会话互斥由控制器维护。

图像坐标、色彩、输出与换栈条件沿用工作区技术选型第 7 节；标注、贴图与完整双端契约验收留待后续。Windows 原生窗口枚举、热键及捕获保持桩，混合 DPI / 负坐标专项仍待实机验证。
