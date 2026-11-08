#pragma once
#include "interfaces/qr_recognizer.h"
#include <QThread>
namespace waibusnap
{
class QrRecognitionTask final : public QThread
{
    Q_OBJECT
  public:
    QrRecognitionTask(std::shared_ptr<const QrRecognizer> recognizer, QImage image, quint64 token,
                      QObject* parent = nullptr);
    ~QrRecognitionTask() override;
  signals:
    void completed(quint64 token, waibusnap::QrRecognitionResult result);

  protected:
    void run() override;

  private:
    std::shared_ptr<const QrRecognizer> recognizer_;
    QImage image_;
    quint64 token_;
};
} // namespace waibusnap
