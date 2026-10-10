#include "interfaces/text_recognizer.h"
#include <QBuffer>
#include <QDebug>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>
namespace waibusnap
{
namespace
{
using namespace winrt::Windows;
QString fromWinrt(const winrt::hstring& text)
{
    return QString::fromWCharArray(text.c_str(), int(text.size()));
}
struct Apartment
{
    Apartment() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    ~Apartment() { winrt::uninit_apartment(); }
};
class WindowsTextRecognizer final : public TextRecognizer
{
  public:
    TextRecognitionResult recognize(const QImage& image) const override
    {
        if (image.isNull())
            return {false, QStringLiteral("没有可识别的图像。"), {}};
        try
        {
            Apartment apartment;
            auto engine = Media::Ocr::OcrEngine::TryCreateFromUserProfileLanguages();
            if (!engine)
                engine = Media::Ocr::OcrEngine::TryCreateFromLanguage(
                    Globalization::Language(L"zh-Hans"));
            if (!engine)
                return {false,
                        QStringLiteral(
                            "系统文字识别不可用，请在 Windows 设置中安装中文或英文 OCR 语言包。"),
                        {}};
            qInfo().noquote() << QStringLiteral("Windows OCR 实际识别语言：")
                              << fromWinrt(engine.RecognizerLanguage().LanguageTag());
            if (image.width() > int(Media::Ocr::OcrEngine::MaxImageDimension()) ||
                image.height() > int(Media::Ocr::OcrEngine::MaxImageDimension()))
                return {
                    false, QStringLiteral("图像尺寸超过系统文字识别上限，请缩小截图选区。"), {}};
            QByteArray png;
            QBuffer buffer(&png);
            if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
                return {false, QStringLiteral("无法转换文字识别图像。"), {}};
            Storage::Streams::InMemoryRandomAccessStream stream;
            Storage::Streams::DataWriter writer(stream.GetOutputStreamAt(0));
            const auto* bytes = reinterpret_cast<const uint8_t*>(png.constData());
            writer.WriteBytes(winrt::array_view<const uint8_t>(bytes, bytes + png.size()));
            writer.StoreAsync().get();
            writer.FlushAsync().get();
            writer.DetachStream();
            stream.Seek(0);
            auto decoder = Graphics::Imaging::BitmapDecoder::CreateAsync(stream).get();
            auto bitmap =
                decoder
                    .GetSoftwareBitmapAsync(Graphics::Imaging::BitmapPixelFormat::Bgra8,
                                            Graphics::Imaging::BitmapAlphaMode::Premultiplied)
                    .get();
            const auto recognized = engine.RecognizeAsync(bitmap).get();
            TextRecognitionResult result{true, {}, {}};
            for (const auto& nativeLine : recognized.Lines())
            {
                RecognizedLine line{fromWinrt(nativeLine.Text()), {}, {}};
                int offset = 0;
                bool alignmentFailed = false;
                for (const auto& word : nativeLine.Words())
                {
                    const auto rect = word.BoundingRect();
                    const QRectF box(rect.X, rect.Y, rect.Width, rect.Height);
                    line.box = line.box.isEmpty() ? box : line.box.united(box);
                    const QString text = fromWinrt(word.Text());
                    const int found = int(line.text.indexOf(text, offset));
                    if (found < 0 || text.isEmpty())
                    {
                        alignmentFailed = true;
                        continue;
                    }
                    line.tokens.append({text, box, found, int(text.size())});
                    offset = found + int(text.size());
                }
                if (alignmentFailed)
                    line.tokens.clear();
                if (!line.text.isEmpty())
                    result.lines.append(std::move(line));
            }
            return result;
        }
        catch (const winrt::hresult_error& error)
        {
            return {false,
                    QStringLiteral("Windows 系统文字识别失败（错误码 %1），请检查 OCR 语言包：%2")
                        .arg(uint32_t(error.code()), 8, 16, QChar('0'))
                        .arg(fromWinrt(error.message())),
                    {}};
        }
    }
};
}
std::unique_ptr<TextRecognizer> createTextRecognizer()
{
    return std::make_unique<WindowsTextRecognizer>();
}
}
