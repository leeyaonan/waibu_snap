#include "app/quick_gunner_save_queue.h"
#include <QRunnable>
#include <algorithm>
namespace waibusnap
{
namespace
{
constexpr qint64 maximumBytes = qint64(256) * 1024 * 1024;
constexpr size_t maximumFrames = 8;
}
QuickGunnerSaveQueue::QuickGunnerSaveQueue(AppSettings settings, QuickGunnerSaveActions actions,
                                           QObject* parent)
    : QObject(parent), settings_(std::move(settings)), actions_(std::move(actions))
{
    if (!actions_.write)
        actions_.write = exportImageToNewPath;
    if (!actions_.timestamp)
        actions_.timestamp = [] { return QDateTime::currentDateTime(); };
    pool_.setMaxThreadCount(1);
    retryTimer_.setSingleShot(true);
    connect(&retryTimer_, &QTimer::timeout, this, &QuickGunnerSaveQueue::pump);
}
QuickGunnerSaveQueue::~QuickGunnerSaveQueue()
{
    retryTimer_.stop();
    // 当前原子写入结束后才释放交付上下文；排队的 GUI 回调随 QObject 一起销毁。
    pool_.waitForDone();
}
void QuickGunnerSaveQueue::setDirectory(QString directory)
{
    directory_ = std::move(directory);
    retryNow();
}
void QuickGunnerSaveQueue::enqueue(QImage image)
{
    if (image.isNull())
    {
        if (!failureEpisode_)
            emit failed(QStringLiteral("快枪手图片为空，无法保存。"));
        return;
    }
    if (image.sizeInBytes() > maximumBytes)
    {
        emit dropped(1);
        return;
    }
    pendingBytes_ += image.sizeInBytes();
    waiting_.push_back({std::move(image), settings_.loadSavePreferences().format,
                        actions_.timestamp(), ++sequence_});
    bound();
    pump();
}
void QuickGunnerSaveQueue::bound()
{
    int count = 0;
    // 新帧与重试帧共享预算；跨队列按原始拍摄顺序丢最旧，保留最新内容。
    while (waiting_.size() + retries_.size() > maximumFrames || pendingBytes_ > maximumBytes)
    {
        auto& oldest = retries_.empty() || (!waiting_.empty() &&
                                            waiting_.front().sequence < retries_.front().sequence)
                           ? waiting_
                           : retries_;
        pendingBytes_ -= oldest.front().image.sizeInBytes();
        oldest.pop_front();
        ++count;
    }
    if (retries_.empty() && !retryInFlight_)
        failureEpisode_ = false;
    if (count)
        emit dropped(count);
}
void QuickGunnerSaveQueue::retryNow()
{
    for (auto& frame : retries_)
        frame.retryAt = 0;
    pump();
}
void QuickGunnerSaveQueue::pump()
{
    if (saving_)
        return;
    retryTimer_.stop();
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto due = std::find_if(retries_.begin(), retries_.end(),
                                  [now](const Frame& item) { return item.retryAt <= now; });
    Frame frame;
    // 到期重试始终优先；新帧不等待未到期项，但也不能让失败帧长期饿死。
    if (due != retries_.end())
    {
        frame = std::move(*due);
        retries_.erase(due);
        retryInFlight_ = true;
    }
    else if (!waiting_.empty())
    {
        frame = std::move(waiting_.front());
        waiting_.pop_front();
        retryInFlight_ = false;
    }
    else
    {
        if (!retries_.empty())
        {
            const auto first = std::min_element(retries_.begin(), retries_.end(),
                                                [](const Frame& a, const Frame& b)
                                                { return a.retryAt < b.retryAt; });
            retryTimer_.start(int(std::min(first->retryAt - now, qint64(2000))));
        }
        return;
    }
    pendingBytes_ -= frame.image.sizeInBytes();
    saving_ = true;
    pool_.start(QRunnable::create(
        [this, frame = std::move(frame), directory = directory_, write = actions_.write]() mutable
        {
            auto result = write(frame.image, directory, frame.format, frame.timestamp);
            QMetaObject::invokeMethod(
                this,
                [this, frame = std::move(frame), result = std::move(result), directory]() mutable
                {
                    saving_ = false;
                    retryInFlight_ = false;
                    if (result.result.success)
                    {
                        if (retries_.empty())
                            failureEpisode_ = false;
                        emit saved(result.path);
                    }
                    else
                    {
                        // 更换目录期间失败的在途帧也立即重试；仍保留快门时的格式与时间。
                        frame.retryAt = directory == directory_
                                            ? QDateTime::currentMSecsSinceEpoch() + 2000
                                            : 0;
                        pendingBytes_ += frame.image.sizeInBytes();
                        const auto position =
                            std::lower_bound(retries_.begin(), retries_.end(), frame.sequence,
                                             [](const Frame& item, quint64 sequence)
                                             { return item.sequence < sequence; });
                        retries_.insert(position, std::move(frame));
                        const bool notify = !failureEpisode_;
                        failureEpisode_ = true;
                        bound();
                        if (notify)
                            emit failed(result.result.explanation);
                    }
                    pump();
                },
                Qt::QueuedConnection);
        }));
}
}
