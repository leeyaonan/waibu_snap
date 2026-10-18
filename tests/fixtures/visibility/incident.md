# WaibuSnap 截图会话假死事件（2026-10-18）证据摘要

## 现象
快捷键 F1 被拒（日志「截图触发被忽略：active= true」×3），托盘菜单项置灰；
用户看不到任何覆盖层，截图功能整体不可用，持续约 1 小时 43 分钟。

## 时间线（已对齐）
- 13:22:16  会话 A（BenQ 3840×2160）用户取消，outcome=2，用时 22.8s（写入 sessions.jsonl seq58）
- 13:44:43  会话 B 由 hotkey 触发（捕获目标=内置屏 3024×1964）；131ms 捕获完成，
           「覆盖层首帧完成」「覆盖层可交互就绪（停止超时保护）」→ 此后无任何日志；
           覆盖层窗口（layer=25）存在于内置屏几何 (-1512,98 1512×982)，但从未显示给用户
- 15:19:xx  用户连按 F1 ×3 → 「截图触发被忽略」×3（会话 B 仍 active）
- 15:28:21  退出应用（osascript quit）→ 会话 B 以 outcome=2 清理，
           用时 6217866ms ≈ 1h43m38s；应用退出、无数据丢失
- 15:28:53  重启应用（新 PID），恢复正常

## 关键证据
1. CGWindowList（OnScreenOnly）：WaibuSnap 无任何在屏窗口；
   同屏 ChatGPT/Sublime/微信 窗口均正常在屏。覆盖层窗口在 All 列表中存在
   （win#67471, layer=25, alpha=1.00, bounds=(-1512,98 1512×982)）。
2. 激活实验：NSRunningApplication activate → 覆盖层立刻进入 OnScreenOnly 列表
   （onscreen=1）；数分钟后再次消失（反复隐藏）。说明窗口本体完好，
   属系统可见性/驻留层面的异常。
3. sample（3s）：主线程正常等事件（无死锁）；全部线程空闲。
4. sessions.jsonl：会话 B 在退出前无记录（未 finish）；退出时补记 outcome=2。
5. 显示配置：BenQ 4K@2x 主屏 (0,0) + 内置 3024×1964@2x (-1512,98)；
   内置屏当时只有一个 Space；Stage Manager 关闭。
6. 内置屏采集分布：227 条记录中内置屏 33 条，此前均正常；会话 B 是近期
   首个内置屏采集（此前连续多日为 BenQ 4K）。

## 代码缺口（main d75ef82）
- checkSessionWatchdog：`overlay_ && overlay_->isVisible()` 即视为“正常等待用户”，
  只信 Qt 层可见性 → 本状态永不触发自动复位（码 12）。
- 「覆盖层可交互就绪」判定（observePresentation 回调）只检查 Qt isVisible +
  isActiveWindow + 显示链采样，未校验窗口真实在屏 → 提前停止 15s 捕获超时保护。
- 无任何 Space / 唤醒 / 显示重配 / occlusion 事件处理可重新 assert 覆盖层。
- 「触发被忽略」日志不含可见性/窗口信息，现场诊断成本高。

## 复现候选（未定死）
内置屏负坐标非主屏创建、锁屏-解锁、睡眠-唤醒、显示器重配、Mission Control /
Space 切换、合盖-开盖；建议按此清单逐项尝试并记录证据。

## 归档文件
- waibusnap-stuck-forensics-1018.log（退出前完整 stderr）
- waibusnap-stuck-forensics-1018-final.log（含退出清理行）
