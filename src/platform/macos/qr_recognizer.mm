#include "interfaces/qr_recognizer.h"
#import <CoreGraphics/CoreGraphics.h>
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
class VisionQrRecognizer final : public QrRecognizer
{
  public:
    QrRecognitionResult recognize(const QImage& image) const override
    {
        @autoreleasepool
        {
            if (image.isNull())
                return {false, QStringLiteral("没有可识别的图像。"), {}};
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
                return {false, QStringLiteral("无法转换二维码识别图像。"), {}};
            VNDetectBarcodesRequest* request = [[VNDetectBarcodesRequest alloc] init];
            request.symbologies = @[ VNBarcodeSymbologyQR ];
            VNImageRequestHandler* handler = [[VNImageRequestHandler alloc] initWithCGImage:cgImage
                                                                                    options:@{}];
            NSError* error = nil;
            const bool ok = [handler performRequests:@[ request ] error:&error];
            CGImageRelease(cgImage);
            if (!ok)
                return {false,
                        QStringLiteral("系统二维码识别失败：%1")
                            .arg(error ? QString::fromNSString(error.localizedDescription)
                                       : QStringLiteral("系统未提供详细原因")),
                        {}};
            QrRecognitionResult result{true, {}, {}};
            for (VNBarcodeObservation* observation in request.results)
            {
                NSString* payload = observation.payloadStringValue;
                if (!payload)
                {
                    NSData* bytes = observation.payloadData;
                    if (bytes)
                        payload = [[NSString alloc] initWithData:bytes
                                                        encoding:NSUTF8StringEncoding];
                }
                if (!payload || payload.length == 0)
                    continue;
                result.codes.append({QString::fromNSString(payload),
                                     pixelBox(observation.boundingBox, pixels.size())});
            }
            std::stable_sort(result.codes.begin(), result.codes.end(),
                             [](const QrCode& a, const QrCode& b) {
                                 return a.box.top() == b.box.top() ? a.box.left() < b.box.left()
                                                                   : a.box.top() < b.box.top();
                             });
            return result;
        }
    }
};
} // namespace
std::unique_ptr<QrRecognizer> createQrRecognizer()
{
    return std::make_unique<VisionQrRecognizer>();
}
} // namespace waibusnap
