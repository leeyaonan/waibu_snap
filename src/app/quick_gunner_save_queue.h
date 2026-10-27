#pragma once
#include "app/app_settings.h"
#include "output/image_output.h"
#include <QObject>
#include <QThreadPool>
#include <QTimer>
#include <deque>
#include <functional>
namespace waibusnap
{
struct QuickGunnerSaveActions
{
    std::function<ImageFileResult(const QImage&, const QString&, ImageFormat, QDateTime)> write;
    std::function<QDateTime()> timestamp;
};
// GUI 线程持有队列；专用单线程池只接收当前一帧，避免线程池内部形成无界积压。
class QuickGunnerSaveQueue final : public QObject
{
    Q_OBJECT
  public:
    explicit QuickGunnerSaveQueue(AppSettings settings, QuickGunnerSaveActions actions = {},
                                  QObject* parent = nullptr);
    ~QuickGunnerSaveQueue() override;
    void setDirectory(QString directory);
    void enqueue(QImage image);
    void retryNow();
    int pendingCount() const { return int(waiting_.size() + retries_.size()) + int(saving_); }
    int retryCount() const { return int(retries_.size()); }
    qint64 pendingBytes() const { return pendingBytes_; }
  signals:
    void saved(QString path);
    void failed(QString error);
    void dropped(int count);

  private:
    struct Frame
    {
        QImage image;
        ImageFormat format;
        QDateTime timestamp;
        quint64 sequence;
        qint64 retryAt = 0;
    };
    void pump();
    void bound();
    AppSettings settings_;
    QuickGunnerSaveActions actions_;
    QString directory_;
    QThreadPool pool_;
    QTimer retryTimer_;
    std::deque<Frame> waiting_;
    std::deque<Frame> retries_;
    qint64 pendingBytes_ = 0;
    quint64 sequence_ = 0;
    bool saving_ = false;
    bool retryInFlight_ = false;
    bool failureEpisode_ = false;
};
}
