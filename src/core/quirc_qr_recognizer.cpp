#include "core/quirc_qr_recognizer.h"
#include <QByteArray>
#include <QImage>
#include <algorithm>
#include <memory>
extern "C"
{
#include <quirc.h>
}
namespace waibusnap
{
namespace
{
struct QuircDeleter
{
    void operator()(quirc* code) const { quirc_destroy(code); }
};
struct PassResult
{
    QVector<QrCode> codes;
};
QRectF codeBox(const quirc_code& code)
{
    int left = code.corners[0].x;
    int top = code.corners[0].y;
    int right = left;
    int bottom = top;
    for (int index = 1; index < 4; ++index)
    {
        left = std::min(left, code.corners[index].x);
        top = std::min(top, code.corners[index].y);
        right = std::max(right, code.corners[index].x);
        bottom = std::max(bottom, code.corners[index].y);
    }
    return QRectF(QPointF(left, top), QPointF(right + 1, bottom + 1));
}
// 把灰度图像送入 quirc；反色兜底只改变写入缓冲，不复制整张图像。
PassResult runPass(const QImage& gray, bool inverted)
{
    PassResult result;
    std::unique_ptr<quirc, QuircDeleter> decoder(quirc_new());
    if (!decoder || quirc_resize(decoder.get(), gray.width(), gray.height()) != 0)
        return result;
    uint8_t* buffer = quirc_begin(decoder.get(), nullptr, nullptr);
    if (!buffer)
        return result;
    const int width = gray.width();
    for (int y = 0; y < gray.height(); ++y)
    {
        const uchar* source = gray.constScanLine(y);
        uint8_t* target = buffer + y * width;
        if (inverted)
            for (int x = 0; x < width; ++x)
                target[x] = uint8_t(255 - source[x]);
        else
            std::copy(source, source + width, target);
    }
    quirc_end(decoder.get());
    const int count = quirc_count(decoder.get());
    for (int index = 0; index < count; ++index)
    {
        quirc_code code;
        quirc_extract(decoder.get(), index, &code);
        quirc_data data;
        quirc_decode_error_t error = quirc_decode(&code, &data);
        if (error != QUIRC_SUCCESS)
        {
            // 镜像二维码按 ISO 18004 允许翻转后重试；其他错误直接跳过。
            quirc_code flipped = code;
            quirc_flip(&flipped);
            error = quirc_decode(&flipped, &data);
        }
        if (error != QUIRC_SUCCESS || data.payload_len <= 0)
            continue;
        const QByteArray payload(reinterpret_cast<const char*>(data.payload), data.payload_len);
        result.codes.append({QString::fromUtf8(payload), codeBox(code)});
    }
    return result;
}
} // namespace
class QuircQrRecognizer final : public QrRecognizer
{
  public:
    QrRecognitionResult recognize(const QImage& image) const override
    {
        if (image.isNull())
            return {false, QStringLiteral("没有可识别的图像。"), {}};
        const QImage gray = image.convertToFormat(QImage::Format_Grayscale8);
        if (gray.isNull())
            return {false, QStringLiteral("无法转换二维码识别图像。"), {}};
        PassResult pass = runPass(gray, false);
        if (pass.codes.isEmpty())
        {
            // 深色背景上的白色二维码；只在正向没有任何可解码结果时尝试。
            PassResult inverted = runPass(gray, true);
            if (!inverted.codes.isEmpty())
                pass = std::move(inverted);
        }
        std::stable_sort(pass.codes.begin(), pass.codes.end(),
                         [](const QrCode& a, const QrCode& b) {
                             return a.box.top() == b.box.top() ? a.box.left() < b.box.left()
                                                               : a.box.top() < b.box.top();
                         });
        // quirc 偶尔会把纹理误判成定位图案；同一位置同一内容只保留一条。
        QVector<QrCode> unique;
        for (const auto& code : pass.codes)
        {
            const bool duplicate = std::any_of(
                unique.cbegin(), unique.cend(), [&code](const QrCode& existing)
                { return existing.text == code.text && existing.box.intersects(code.box); });
            if (!duplicate)
                unique.append(code);
        }
        return {true, {}, std::move(unique)};
    }
};
std::unique_ptr<QrRecognizer> createQuircQrRecognizer()
{
    return std::make_unique<QuircQrRecognizer>();
}
} // namespace waibusnap
