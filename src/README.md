# 源码结构

| 目录 | 职责 | 初始化状态 |
| --- | --- | --- |
| `app/` | 应用装配、启动、退出 | 托盘生命周期、受控测试入口；`sticker_manager` 管理独立贴图、保存状态、全部隐藏 / 恢复、屏幕找回与退出销毁；`app_settings` 的 INI 存储与注册 / 落盘协调，`hotkey_rules` 的无平台校验 |
| `interfaces/` | 共享平台契约 | 事务式热键、显示器定位、单帧捕获与呈现观察；`window_enumerator` 的前到后全局逻辑外框列表；`platform_workarounds` 的幂等兼容绕行安装入口；`sticker_window_behavior` 的不抢焦点窗口配置契约；`tray_icon` 的内嵌菜单栏 / 托盘 QIcon 工厂 |
| `platform/macos/` | Objective-C++ / 后续 AppKit、ScreenCaptureKit | Carbon 热键映射与事务式替换、CGWindowList 窗口枚举、ScreenCaptureKit / AppKit 单帧原型及外部测量探针；`platform_workarounds` 的 NSEvent 安全 clickCount 绕行；贴图 NSPanel 非激活样式与不随应用失活隐藏 |
| `platform/windows/` | MSVC C++ / 后续 Win32、DXGI | 可编译桩，如实返回未实现；兼容绕行入口为空实现；贴图焦点行为沿用 Qt 属性，编辑前后通过 Win32 恢复原前台窗口 |
| `core/` | 图像与标注核心 | `annotation` 的八类标注（含实心遮盖与马赛克）、颜色 / 线宽 / 字号预设、箭头几何与 20 步撤销 / 重做；`sticker_geometry` 的缩放夹取 / 步进、物理转逻辑尺寸、锚点与屏外找回纯函数；半开物理像素框选、移动与八方向调整；`window_snapping` 的全局转屏内裁剪、前到后命中与像素边缘取整 |
| `session/` | 会话与文档状态 | 单会话锁、单调时钟、尺寸与时间 JSONL |
| `ui/` | Qt Widgets 视图与桌面入口 | 冻结画面悬停 / 单击吸附、框选 / 移动 / 调整；八工具 / 样式 / 撤销 / 重做 / 复制 / 保存 / 钉图 / 取消工具栏；`sticker_window` 的置顶、不抢焦点、拖动、缩放、悬停操作与原像素输出；`annotation_text_edit` 的多行纯文本、输入法预编辑与 Esc 分层；保存面板焦点管理；`settings_dialog` 的单组合输入、中文反馈和保存 / 取消；无默认主窗口 |
| `output/` | 输出与生命周期 | `annotation_renderer` 的共享绘制和冻结帧标注合成后裁剪、DPR=1 输出、Qt 剪贴板与 PNG 原子提交、文件名补后缀和冲突避让；无自动保存 |

平台相关源码只放 `platform/`，共享接口不暴露系统句柄。CMake 在平台目录选择实现，共享应用不使用平台宏判断。业务依赖方向为应用装配 → 共享模块 / 平台实现，平台实现 → 共享接口；不得从核心反向依赖视图或具体平台。

`createTrayIcon()` 在 macOS 返回 18 / 36px 黑版并设置模板标记，Windows 返回 16 / 32px 彩色版。四个确认 PNG 由平台 CMake 的 `qt_add_resources` 嵌入，显式加入 @2x 档位；`ApplicationController` 只装配返回的 QIcon，菜单与点击路径不变。资源与再生成说明见 [图标资源](../assets/icons/README.md)。

`OverlayActions` 提供复制、钉图与路径选择的行为缝，默认走真实 QClipboard 和原生 QFileDialog；`exportToPath` 分离路径导出与面板。输出由 `renderAnnotatedSelection(frame_.pixels, annotations, selection)` 合成后裁剪，不捕获 QWidget 显示层；保存成功保留会话，复制成功以结果码 10、钉图成功以结果码 11 结束。工具栏在选区成立后创建，文本编辑器仅在使用文本工具时创建；标注、撤销历史、工具栏和短暂反馈计时器归会话所有，空闲无隐藏预建选区或轮询。

窗口列表在 `captureCompleted` 成功后查询一次，经 `localWindowRects` 裁剪再注入覆盖层；悬停像素矩形同时用于绘制与只读断言。手势最大位移不超过 3 个逻辑像素才确认按下处窗口，手动拖选开始即清除高亮；已有选区外部单击只清除，下一次单击才能重新吸附。高亮、选区手柄与工具栏均不进入合成输出。

`HotkeySettings` 使用 `GlobalHotkey` 契约协调启动回退及改键，不依赖托盘或真实平台注册，可用替身验证失败保旧与成功后落盘。macOS 映射 Qt Control → Command、Meta → 物理 Control、Alt → Option、Shift → Shift，Carbon 独占注册新键后释放旧键；退出显式 `unregister`，析构再次调用安全。`SettingsDialog` 的校验和元数据可 offscreen 测试，设置会话互斥由控制器维护。

`Annotation` 中的起终点、画笔点序列和文本落点均为冻结帧绝对物理像素。选区变化只改变裁剪窗口，形状不会跟着移动或缩放。`AnnotationHistory` 保留全部已提交标注；超出 20 步只移除早期撤销资格，撤销后新增清除重做分叉，会话终结立即清空。`arrowHead` 统一提供随线宽变化的箭头头部几何。

`AnnotationType::Cover` 保持第七类（索引 6），`Mosaic` 追加在枚举末尾（索引 7），既有七类索引保持不变；可见性与矩形一致，任一维度为零时不入栈。`paintAnnotation` 将遮盖物理像素边界按 floor(left/top)、ceil(right/bottom) 向外取整，以不透明色填充半开矩形，禁用抗锯齿且无描边；按提交顺序绘制，后画遮盖替换其下所有内容。颜色沿用色板，线宽沿用文本禁用机制；覆盖层工具条扩大到最多 720 逻辑像素，宽度不足 640 时工具分三列；贴图按可用宽度最多八列，容不下时原折叠菜单保留全部八工具与样式入口。预览、草稿、保存和复制继续共用同一例程。

`Mosaic` 与 `Cover` 同用 first / last 矩形、零面积拒绝和提交顺序。`pixelateRegion(base, region)` 是纯函数，返回裁剪后的物理像素矩形与 DPR=1 的不透明补丁；`mosaicBlockSize` 使用向外取整后的整数区域短边，按 `clamp(round(短边 ÷ 12), 4, 48)` 求块边长。每个块先面积平均平滑缩为一个像素，再最近邻铺回原块；网格锚定未裁剪区域左上角，边界块只采样基图内像素，完全越界跳过。

渲染签名统一为 `paintAnnotation(painter, annotation, base)` 与 `paintAnnotations(painter, annotations, base, cache = nullptr)`；不可变 `base` 是覆盖层冻结帧或贴图基图，不能用已合成的标注层替代。马赛克补丁绘制禁用抗锯齿和平滑采样，opacity=1；实心遮盖继续按原色板纯色覆盖。两个 UI 持有 `AnnotationRenderCache`：按标注位置及几何缓存已提交补丁，基图 cacheKey 变化重算，撤销缩减缓存；草稿和导出直接调用相同纯函数，均走同一补丁绘制路径。缓存不保留基图，也不保存文件。马赛克不读取颜色 / 线宽 / 字号；UI 禁用这些选项（字号仍仅文本可用），首次激活使用现有状态区提示，提示标记保留至截图会话 / 贴图生命周期结束。

覆盖层按源屏 DPR 将物理坐标映射到逻辑画布，再调用 `paintAnnotations` 绘制已提交标注与 `paintAnnotation` 绘制当前手势草稿。导出缓冲只分配选区大小，复制冻结像素并用整数平移将相同绘制例程映射到裁剪窗口；文本用 Qt 纯文本排版和物理像素字体，不从输入框截取。添加、撤销、重做及裁剪窗口改变均标记未保存；保存成功保留会话并标记已保存。所有编辑快捷键都局限于当前图片的编辑态，文本输入期间不触发标注撤销 / 重做，候选 / 系统面板的临时焦点变化不提交文本。

图像坐标、色彩、输出与换栈条件沿用工作区技术选型第 7 节；贴图内编辑和未保存确认已接入；标注对象再编辑与完整双端实机验收留待后续。Windows 原生窗口枚举、热键及捕获保持桩，混合 DPI / 负坐标专项仍待实机验证。

`ApplicationController` 注入 `pinImage`，通过覆盖层只读的选区全局逻辑位置与 `isSelectionSaved()` 调用 `StickerManager::create`。合成图 DPR=1；正常输入保持 QImage 隐式共享，不额外复制整图。管理器区分活跃窗口与尚待延迟销毁的窗口，提供 `count()` / `windows()` / `hideAll()` / `restoreAll()` / `closeAll()`；控制器托盘退出、`aboutToQuit` 与析构共用清理。无常驻贴图历史、抓屏或屏幕轮询；布局找回只响应屏幕通知。

`StickerManager::hideAll()` 按 `QWidget::isVisible()` 遍历当前可见窗口，调用 `setEditing(false)` 提交文本并退出编辑，再 `hide()`；隐藏不触发关闭确认，不写 `saved_`。`restoreAll()` 仅对活跃列表中不可见的存活窗口调用 `show()`，沿用 WA_ShowWithoutActivating / DoesNotAcceptFocus。状态口径是实时可见性，没有额外的全局隐藏标记或待恢复快照；关闭后从活跃列表移除，故延迟销毁中的已关闭窗口也不能复活。混合状态下两项分别作用于可见 / 隐藏窗口；退出解析 resolvingQuit 与清理 closingAll 期间两 API 均为空操作。`recoverWindows()` 只调整屏幕及几何，不显示窗口；退出汇总遍历全部活跃窗口，隐藏未保存项仍需确认。

`ApplicationController::refreshStickerActions()` 遍历 `windows()` 及实时可见性，设置常驻 QAction `hideAllStickersAction` / `restoreAllStickersAction` 的可用性；菜单顺序为截图、设置、隐藏、恢复、退出。构造时、`QMenu::aboutToShow` 和两项执行后刷新，不触及截图会话、覆盖层或测量协议。

`StickerWindow` 使用 Frameless / StaysOnTop / Tool / DoesNotAcceptFocus 与 ShowWithoutActivating，子控件 NoFocus。Qt Tool 的失活隐藏通过 `WA_MacAlwaysShowToolWindow` 禁用；macOS 在显示前为 Qt 创建的 NSPanel 加 `NSWindowStyleMaskNonactivatingPanel` 并保持可见。Qt 的拒绝 key window 处理与 NSPanel 样式取舍依据 [Qt 6.11.2 Cocoa 窗口实现](https://github.com/qt/qtbase/blob/v6.11.2/src/plugins/platforms/cocoa/qcocoawindow.mm) 与 [QNSWindow](https://github.com/qt/qtbase/blob/v6.11.2/src/plugins/platforms/cocoa/qnswindow.mm)。处理集中在 `platform/macos/sticker_window_behavior.mm`，沿用已有呈现观察器的 NSView 桥接方式；Windows 编辑态允许前台输入并持有原前台窗口，退出编辑时通过同一行为会话恢复。offscreen 不转换原生句柄，真实浏览器 / 编辑器键盘焦点仍须人工核对。

缩放范围 25%–400%，每档 25 个百分点；纯几何无 QWidget 依赖。窗口逻辑尺寸为图像物理尺寸 × 缩放 ÷ 当前屏 DPR，向上取整至整数（最小 1）；100% 不平滑、不重采样原像素，非整除尺寸留不足 1 逻辑像素边缘。拖动跨屏按光标下屏幕切换，保持鼠标下图像位置；亚像素窗口位置避免连续缩放累计漂移。悬停控制条不影响图像窗口尺寸，极小图使用操作菜单与右键入口。

`StickerWindow` 的编辑工具条按需创建，两个编辑面均保留第七个遮盖按钮并追加第八个马赛克按钮；八工具、三色 / 三线宽 / 三字号、20 步历史和折叠菜单共用 `Annotation`、`AnnotationHistory`、`AnnotationTextEdit` 及样式常量。贴图标注坐标为基图物理像素，显示变换统一为缩放 ÷ 当前屏 DPR，基图、已提交标注和草稿使用相同变换；100% 的基图不重采样。输出调用全图 `renderAnnotatedSelection`，无标注时直接返回隐式共享基图，DPR=1。钉图前的标注已烘焙进基图，不进入新的历史。新增 / 撤销 / 重做 / 非空文本提交标记未保存，空点击 / 空拖不改变状态；完成编辑保留历史，复制不改变保存状态。

编辑态只禁止窗口移动，滚轮保留光标锚点；无工具时拖动无操作。输入框按缩放 / DPR 换算字体与落点，点击别处、切换工具、开始绘图、完成及输出前提交；IME 预编辑 → 丢弃文本 → 退出编辑的 Esc 分层由共享编辑器处理。按钮与组合框 NoFocus，标准撤销 / 重做快捷键只在编辑态启用，文本输入的 ShortcutOverride 优先于标注历史。

焦点取舍：保持 Qt Tool / 无边框 / 置顶，切换编辑时隐藏并重建原生窗口，编辑态移除 DoesNotAcceptFocus，退出时恢复；不在同一个 NSPanel 上运行中切换非激活样式，以免遗留 AppKit 的激活状态。保存目标屏、精确位置与尺寸，原生行为重新配置后显示，再恢复一次位置，处理 Cocoa 在菜单栏边界显示时调整位置的行为；重新连接 screenChanged，内容与历史属于 QWidget，不随原生窗口重建丢失。进入编辑通过 `activateStickerEditing` 持有 `StickerFocusSession`：macOS 记录原前台应用，在明确编辑入口用 activateIgnoringOtherApps 激活本应用并成为 key；退出时先 yieldActivation 再恢复原应用；Windows 记录原前台 HWND、允许前台输入。退出编辑 / 关闭时析构会话，仅在本应用仍处于前台时恢复原应用，用户已自行切换应用时不再抢焦点。平台句柄只出现在平台文件中，offscreen 跳过原生操作。依据 [Qt windowFlags 的隐藏行为](https://doc.qt.io/qt-6/qwidget.html#windowFlags-prop) 与 [Qt 6.11.2 Cocoa 键盘 / 输入法路径](https://github.com/qt/qtbase/blob/v6.11.2/src/plugins/platforms/cocoa/qnsview_keys.mm)；真实输入法和外部应用验证口径见 tests/README。

`StickerActions` 提供复制、路径选择、`confirmClose(QWidget*)` 与 `confirmQuit(count)` 行为缝，默认使用真实剪贴板、原生保存面板与中文三选一 QMessageBox；路径导出仍用 `image_output` 的 PNG 原子提交及命名工具。单张关闭先提交文本，已保存直接关闭，未保存选择取消 / 保存 / 放弃；面板取消或写入失败返回 false、提示并保留窗口。窗口以 confirmingClose / saveDialogOpen 保护嵌套循环，Qt 自身的 close 重入也以实际窗口生命周期与回调次数验证。

`StickerManager::resolveUnsavedForQuit()` 在清理前结束编辑并提交文本，按创建顺序收集未保存项；汇总取消或任一逐张保存取消 / 失败返回 false，已保存项保持已保存。汇总期间禁用贴图交互与新增，保存 / 关闭确认期间再退出直接返回 false。控制器单独用 resolvingQuit 保护托盘重入，只有返回 true 才进入既有 cleanup，故中止不销毁选区、不释放热键；期间不接受新的截图或设置。`closeAll()` 使用 forceClose，aboutToQuit 与析构仍兜底清理，不二次确认；管理器保留待删除窗口的所有权，面板嵌套循环中的删除用 QPointer 防护。应用冒烟的两张贴图显式 saved=true，确认决策由进程内用例覆盖。选区退出码 9、受控模式码 2、结果码 1–11 与测量协议不变。
