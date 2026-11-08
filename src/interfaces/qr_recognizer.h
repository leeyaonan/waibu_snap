#pragma once
#include <QImage>
#include <QMetaType>
#include <QRectF>
#include <QString>
#include <QVector>
#include <memory>
namespace waibusnap
{
struct QrCode
{
    QString text;
    // 识别框为输入图像本地物理像素、左上原点；仅用于排序与后续定位，不参与输出。
    QRectF box;
};
struct QrRecognitionResult
{
    bool ok = false;
    QString explanation;
    QVector<QrCode> codes;
};
// 输入为选区 / 贴图合成图、本地物理像素、左上原点、DPR=1；仅允许工作线程调用。
class QrRecognizer
{
  public:
    virtual ~QrRecognizer() = default;
    virtual QrRecognitionResult recognize(const QImage& image) const = 0;
};
std::unique_ptr<QrRecognizer> createQrRecognizer();
}
Q_DECLARE_METATYPE(waibusnap::QrRecognitionResult)
