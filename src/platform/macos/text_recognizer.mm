#include "interfaces/text_recognizer.h"
#include "core/text_layout.h"
#import <CoreGraphics/CoreGraphics.h>
#include <QDebug>
#import <Vision/Vision.h>
#include <algorithm>
namespace waibusnap
{
namespace
{
QRectF pixelBox(CGRect box, const QSize& size)
{
    return {box.origin.x * size.width(), (1 - box.origin.y - box.size.height) * size.height(),
            box.size.width * size.width(), box.size.height * size.height()};
}
class VisionTextRecognizer final : public TextRecognizer
{
  public:
    TextRecognitionResult recognize(const QImage& image) const override
    {
        @autoreleasepool
        {
            if (image.isNull())
                return {false, QStringLiteral("没有可识别的图像。"), {}};
            VNRecognizeTextRequest* request = [[VNRecognizeTextRequest alloc] init];
            request.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
            request.usesLanguageCorrection = YES;
            NSError* error = nil;
            NSArray<NSString*>* supported =
                [request supportedRecognitionLanguagesAndReturnError:&error];
            if (!supported)
                return {false,
                        QStringLiteral("无法查询系统文字识别语言：%1")
                            .arg(error ? QString::fromNSString(error.localizedDescription)
                                       : QStringLiteral("系统未提供详细原因")),
                        {}};
            NSMutableArray<NSString*>* languages = [NSMutableArray array];
            for (NSString* language in @[ @"zh-Hans", @"en-US" ])
                if ([supported containsObject:language])
                    [languages addObject:language];
            if (languages.count == 0)
                return {false, QStringLiteral("系统文字识别不支持中文或英文。"), {}};
            request.recognitionLanguages = languages;
            QStringList actualLanguages;
            for (NSString* language in languages)
                actualLanguages.append(QString::fromNSString(language));
            qInfo().noquote() << QStringLiteral("Vision 实际识别语言：")
                              << actualLanguages.join(',');
            const QImage pixels = image.convertToFormat(QImage::Format_RGBA8888);
            CFDataRef data = CFDataCreate(nullptr, pixels.constBits(), pixels.sizeInBytes());
            CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
            CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
            CGImageRef cgImage =
                CGImageCreate(pixels.width(), pixels.height(), 8, 32, pixels.bytesPerLine(),
                              colorSpace, kCGImageAlphaLast | kCGBitmapByteOrder32Big, provider,
                              nullptr, false, kCGRenderingIntentDefault);
            CGColorSpaceRelease(colorSpace);
            CGDataProviderRelease(provider);
            CFRelease(data);
            if (!cgImage)
                return {false, QStringLiteral("无法转换文字识别图像。"), {}};
            VNImageRequestHandler* handler = [[VNImageRequestHandler alloc] initWithCGImage:cgImage
                                                                                    options:@{}];
            const bool ok = [handler performRequests:@[ request ] error:&error];
            CGImageRelease(cgImage);
            if (!ok)
                return {false,
                        QStringLiteral("系统文字识别失败：%1")
                            .arg(error ? QString::fromNSString(error.localizedDescription)
                                       : QStringLiteral("系统未提供详细原因")),
                        {}};
            TextRecognitionResult result{true, {}, {}};
            if (![languages containsObject:@"zh-Hans"])
                result.explanation = QStringLiteral("系统未提供简体中文识别支持，实际语言：%1")
                                         .arg(actualLanguages.join(','));
            for (VNRecognizedTextObservation* observation in request.results)
            {
                VNRecognizedText* candidate = [observation topCandidates:1].firstObject;
                if (!candidate || candidate.string.length == 0)
                    continue;
                RecognizedLine line{QString::fromNSString(candidate.string),
                                    pixelBox(observation.boundingBox, pixels.size()),
                                    {}};
                // 每行仅查询一次每个词范围；任一范围失败，整行回落，不猜测盒。
                for (const auto span : text_layout::tokenizeLine(line.text))
                {
                    NSError* boxError = nil;
                    VNRectangleObservation* tokenBox =
                        [candidate boundingBoxForRange:NSMakeRange(span.offset, span.length)
                                                 error:&boxError];
                    if (!tokenBox || boxError)
                    {
                        line.tokens.clear();
                        break;
                    }
                    line.tokens.append({line.text.mid(span.offset, span.length),
                                        pixelBox(tokenBox.boundingBox, pixels.size()), span.offset,
                                        span.length});
                }
                result.lines.append(std::move(line));
            }
            std::stable_sort(result.lines.begin(), result.lines.end(),
                             [](const RecognizedLine& a, const RecognizedLine& b)
                             {
                                 return a.box.center().y() == b.box.center().y()
                                            ? a.box.left() < b.box.left()
                                            : a.box.center().y() < b.box.center().y();
                             });
            return result;
        }
    }
};
}
std::unique_ptr<TextRecognizer> createTextRecognizer()
{
    return std::make_unique<VisionTextRecognizer>();
}
}
