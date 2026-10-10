#pragma once
#include "interfaces/text_recognizer.h"
#include <QDialog>
#include <QTimer>
#include <functional>
class QMimeData;
class QTextEdit;
class QLabel;
class QPushButton;
class QTextCharFormat;
namespace waibusnap
{
class OcrDialog final : public QDialog
{
    Q_OBJECT
  public:
    explicit OcrDialog(QWidget* parent = nullptr,
                       std::function<void(const QMimeData&)> setClipboard = {});
    void setRecognizing();
    void setResult(const TextRecognitionResult& result);
  signals:
    void retryRequested();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void setEditing(bool editing);
    void applyFormat(const QTextCharFormat& format);
    void copyDocument(bool html);
    QTextEdit* editor_;
    QLabel* state_;
    QLabel* feedback_;
    QWidget* formats_;
    QPushButton* edit_;
    QPushButton* retry_;
    QPushButton* copy_;
    QPushButton* plainCopy_;
    bool editing_ = false;
    QTimer feedbackTimeout_;
    std::function<void(const QMimeData&)> setClipboard_;
};
}
