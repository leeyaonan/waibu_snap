#pragma once
#include <QImage>
#include <QMetaType>
#include <QRectF>
#include <QString>
#include <QVector>
#include <memory>
namespace waibusnap
{
struct RecognizedToken
{
    QString text;
    QRectF box;
    int offset = 0;
    int length = 0;
};
struct RecognizedLine
{
    QString text;
    QRectF box;
    QVector<RecognizedToken> tokens;
};
struct TextRecognitionResult
{
    bool ok = false;
    QString explanation;
    QVector<RecognizedLine> lines;
};
// 输入为选区合成图、本地物理像素、左上原点、DPR=1；仅允许工作线程调用。
class TextRecognizer
{
  public:
    virtual ~TextRecognizer() = default;
    virtual TextRecognitionResult recognize(const QImage& image) const = 0;
};
std::unique_ptr<TextRecognizer> createTextRecognizer();
}
Q_DECLARE_METATYPE(waibusnap::TextRecognitionResult)
