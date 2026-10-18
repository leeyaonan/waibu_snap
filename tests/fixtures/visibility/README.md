# 截图真实在屏验证原始记录

日期：2026-10-18。本目录只保存技术日志、测试样本状态与计时，不保存截屏图像。

- `incident.md` / `incident.stderr.log`：用户指定临时归档的原始证据与应用 stderr，未据缺失字段倒推确定根因。
- `regression-before.txt`：仅接入可注入查询但尚未修复看门狗时的失败用例。
- `native-first.txt`：真实 Cocoa orderOut → Qt 仍可见 → 系统列表缺失 → 原生恢复；候选 collectionBehavior 的当前 Space 检查。
- `ocr-first.txt`：首轮完整 CTest 中真实 Vision 样本超时记录；不属于本次改动，也不隐去失败。

最终验证及人工步骤见 ../../README.md。

- `regression-after.txt` / `smoke-ctest.txt`：最终共享控制器回归与合成压力测试。
- `native-final.txt` / `native-ctest.txt`：最终真实原生测试输出与 CTest 结论。
- `ctest-core.txt` / `ocr-retry.txt` / `ocr-last-test.txt`：其余 CTest 与 OCR 重试原始记录。
- `internal.*` / `external.*`：实现阶段各 10 次真实采集，非最终提交正式性能验收。
- `manual.*`：人工持续窗口、occlusion、自主取消与用户拒绝录屏后的终态 7。
