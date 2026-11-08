#include "ui/qr_recognition_task.h"
#include <QPainter>
#include <exception>
namespace waibusnap
{
QrRecognitionTask::QrRecognitionTask(std::shared_ptr<const QrRecognizer> recognizer, QImage image,
                                     quint64 token, QObject* parent)
    : QThread(parent), recognizer_(std::move(recognizer)), image_(std::move(image)), token_(token)
{
    image_.setDevicePixelRatio(1);
    if (image_.hasAlphaChannel())
    {
        // 透明贴图 / 异形截图先合成到白底，保证双端引擎拿到同一份不透明输入。
        QImage flattened(image_.size(), QImage::Format_RGB32);
        flattened.fill(Qt::white);
        QPainter painter(&flattened);
        painter.drawImage(0, 0, image_);
        image_ = flattened;
    }
    qRegisterMetaType<QrRecognitionResult>();
}
QrRecognitionTask::~QrRecognitionTask()
{
    requestInterruption();
    // 系统识别没有取消接口；销毁时最多等待本次识别，不能遗留运行线程。
    wait();
}
void QrRecognitionTask::run()
{
    QrRecognitionResult result;
    try
    {
        result = recognizer_
                     ? recognizer_->recognize(image_)
                     : QrRecognitionResult{false, QStringLiteral("二维码识别引擎不可用。"), {}};
    }
    catch (const std::exception&)
    {
        result = {false, QStringLiteral("二维码识别引擎异常，请重试。"), {}};
    }
    catch (...)
    {
        result = {false, QStringLiteral("二维码识别引擎发生未知异常，请重试。"), {}};
    }
    emit completed(token_, std::move(result));
}
} // namespace waibusnap
