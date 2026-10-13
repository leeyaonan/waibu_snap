#pragma once
#include "core/scroll_stitcher.h"
#include <QTimer>
#include <QWidget>
class QLabel;
class QPushButton;
namespace waibusnap
{
QRect placeScrollBar(QRect region, QSize size, QRect available);
class ScrollBorderWindow final : public QWidget
{
  public:
    explicit ScrollBorderWindow(QRect region);

  protected:
    void paintEvent(QPaintEvent*) override;
};
class ScrollControlBar final : public QWidget
{
    Q_OBJECT
  public:
    ScrollControlBar(QRect region, QRect available);
    void setStatistics(ScrollStitchStats stats, qint64 limit);
    void setNotice(const QString& text);
    void setAutomatic(bool enabled);
    void setOptions(int speed, int direction);
    bool explainAccessibilityPermission();
  signals:
    void automaticRequested(bool enabled);
    void speedRequested();
    void directionRequested();
    void finishRequested();
    void cancelRequested();
    void placementChanged(QRect rectangle);

  protected:
    void keyPressEvent(QKeyEvent*) override;

  private:
    void place();
    void refreshStatus();
    QRect region_;
    QRect available_;
    QLabel* status_ = nullptr;
    QPushButton* autoButton_ = nullptr;
    QPushButton* speedButton_ = nullptr;
    QPushButton* directionButton_ = nullptr;
    int speed_ = 1;
    int direction_ = 0;
    QString statistics_;
    QString notice_;
    QTimer noticeTimeout_;
};
}
