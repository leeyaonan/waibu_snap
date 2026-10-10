#include "ui/text_recognition_task.h"
#include <exception>
namespace waibusnap
{
TextRecognitionTask::TextRecognitionTask(std::shared_ptr<const TextRecognizer> recognizer,
                                         QImage image, quint64 token, QObject* parent)
    : QThread(parent), recognizer_(std::move(recognizer)), image_(std::move(image)), token_(token)
{
    image_.setDevicePixelRatio(1);
    qRegisterMetaType<TextRecognitionResult>();
}
TextRecognitionTask::~TextRecognitionTask()
{
    requestInterruption();
    // 系统识别没有取消接口；销毁时最多等待本次识别，不能遗留运行线程。
    wait();
}
void TextRecognitionTask::run()
{
    TextRecognitionResult result;
    try
    {
        result = recognizer_
                     ? recognizer_->recognize(image_)
                     : TextRecognitionResult{false, QStringLiteral("文字识别引擎不可用。"), {}};
    }
    catch (const std::exception&)
    {
        result = {false, QStringLiteral("文字识别引擎异常，请重试。"), {}};
    }
    catch (...)
    {
        result = {false, QStringLiteral("文字识别引擎发生未知异常，请重试。"), {}};
    }
    emit completed(token_, std::move(result));
}
}
