#pragma once
#include "interfaces/text_recognizer.h"
#include <QThread>
namespace waibusnap
{
class TextRecognitionTask final : public QThread
{
    Q_OBJECT
  public:
    TextRecognitionTask(std::shared_ptr<const TextRecognizer> recognizer, QImage image,
                        quint64 token, QObject* parent = nullptr);
    ~TextRecognitionTask() override;
  signals:
    void completed(quint64 token, waibusnap::TextRecognitionResult result);

  protected:
    void run() override;

  private:
    std::shared_ptr<const TextRecognizer> recognizer_;
    QImage image_;
    quint64 token_;
};
}
