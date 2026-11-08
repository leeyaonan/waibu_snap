#pragma once
#include "interfaces/qr_recognizer.h"
#include <QDialog>
#include <QTimer>
#include <QUrl>
#include <functional>
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
namespace waibusnap
{
class QrResultDialog final : public QDialog
{
    Q_OBJECT
  public:
    explicit QrResultDialog(QWidget* parent = nullptr,
                            std::function<bool(const QUrl&)> openUrl = {});
    void setScanning();
    void setResult(const QrRecognitionResult& result);
  signals:
    void retryRequested();
    void copyRequested(const QString& text);

  private:
    void currentChanged(int row);
    void updateOpenState();
    QString currentText() const;
    static bool openable(const QString& text);
    QLabel* state_;
    QListWidget* list_;
    QPlainTextEdit* content_;
    QLabel* feedback_;
    QPushButton* retry_;
    QPushButton* copy_;
    QPushButton* open_;
    QPushButton* close_;
    QTimer feedbackTimeout_;
    std::function<bool(const QUrl&)> openUrl_;
    QVector<QrCode> codes_;
    bool scanning_ = true;
};
} // namespace waibusnap
