# 最小 WebP 样本

`minimal.webp` 为本批用 Qt 6.11.2 官方 WebP 插件生成的 7×5 静态无损小图（42 字节），不来自用户图片或网络资源。初始化 `QImage::Format_ARGB32`，全图填充 `QColor(30, 80, 190, 120)`，左上角一像素设为纯红；通过 `QImageWriter(path, "webp")`、`setQuality(100)`、`write(image)` 写出。

正常插件测试验证可读、尺寸和两种颜色；插件缺失测试只把这份样本复制到临时历史根，再在独立进程移除图片插件搜索路径，验证真实缺失提示。运行时 PNG / JPEG / WebP 样本均在 `QTemporaryDir` 生成，无其他新增图片资源。
