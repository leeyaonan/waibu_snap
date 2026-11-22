#include "app/multi_display_capture.h"
#include "core/quick_gunner.h"
#include <QPointer>
#include <QTimer>
#include <algorithm>
namespace waibusnap
{
namespace
{
QString displayLabel(int index) { return QStringLiteral("第 %1 屏").arg(index + 1); }
QString mergedReasons(const QStringList& failures)
{
    // 相同原因合并屏列表，避免三屏同因时重复刷屏。
    QStringList order;
    QHash<QString, QStringList> grouped;
    for (int index = 0; index < failures.size(); ++index)
    {
        const QString reason = failures[index].trimmed();
        if (reason.isEmpty())
            continue;
        if (!grouped.contains(reason))
            order.append(reason);
        grouped[reason].append(displayLabel(index));
    }
    QStringList parts;
    for (const auto& reason : order)
        parts.append(
            QStringLiteral("%1：%2").arg(grouped.value(reason).join(QStringLiteral("、")), reason));
    return parts.join(QStringLiteral("；"));
}
}
MultiDisplayCaptureController::MultiDisplayCaptureController(
    AppSettings settings, DisplayTopology& displays, CaptureProvider& capture,
    MultiDisplayCaptureDependencies dependencies, QObject* parent)
    : QObject(parent), settings_(std::move(settings)), displays_(displays), capture_(capture),
      dependencies_(std::move(dependencies))
{
    if (!dependencies_.saveGroup)
        dependencies_.saveGroup = [](const MultiDisplaySaveRequest& request)
        { return saveMultiDisplayGroup(request); };
    if (!dependencies_.stitchEnabled)
        dependencies_.stitchEnabled = [this]
        { return settings_.loadSavePreferences().multiDisplayStitch; };
    if (!dependencies_.reservedDirectories)
        dependencies_.reservedDirectories = [] { return QStringList(); };
    if (!dependencies_.flash)
        dependencies_.flash = [](QRect, int) {};
}
MultiDisplayCaptureController::~MultiDisplayCaptureController() { ++token_; }
void MultiDisplayCaptureController::setBusy(bool busy)
{
    if (busy_ == busy)
        return;
    busy_ = busy;
    emit busyChanged(busy_);
}
QString MultiDisplayCaptureController::start()
{
    if (busy_)
        return QStringLiteral("正在截取全部显示器，请稍后。");
    if (!dependencies_.supported())
        return QStringLiteral("多显示器截图暂不支持当前平台（Windows 后补）。");
    const auto preferences = settings_.loadQuickGunnerPreferences();
    if (preferences.directory.isEmpty())
        return QStringLiteral("请先选择快枪手目录。");
    QString cleaned;
    const auto directoryError = validateCaptureDirectory(
        preferences.directory, dependencies_.reservedDirectories(), cleaned);
    if (!directoryError.isEmpty())
        return directoryError;
    const auto displays = displays_.allDisplays();
    if (displays.isEmpty())
        return QStringLiteral("未获取到可用显示器，请检查显示器连接后重试。");
    for (const auto& display : displays)
    {
        if (!display.error.isEmpty())
            return display.error;
        if (!displays_.stillMatches(display.target))
            return QStringLiteral("显示器布局已变化，请重试。");
    }
    if (!capture_.hasPermission())
        return capture_.permissionExplanation();
    auto pending = std::make_shared<Pending>();
    pending->token = ++token_;
    for (const auto& display : displays)
        pending->displays.append(display.target);
    pending->frames.resize(pending->displays.size());
    pending->done.fill(false, pending->displays.size());
    pending->failures.resize(pending->displays.size());
    pending_ = pending->displays.size();
    setBusy(true);
    emit started();
    // 推迟一拍：让托盘菜单弹层先关闭，避免菜单进入画面。
    QPointer<MultiDisplayCaptureController> self(this);
    QTimer::singleShot(0, this,
                       [self, pending]
                       {
                           if (self)
                               self->collect(pending);
                       });
    return {};
}
void MultiDisplayCaptureController::collect(const std::shared_ptr<Pending>& pending)
{
    if (!pending || pending->token != token_)
        return;
    QPointer<MultiDisplayCaptureController> self(this);
    for (int index = 0; index < pending->displays.size(); ++index)
    {
        const auto target = pending->displays[index];
        const auto token = pending->token;
        capture_.capture(
            target,
            [self, pending, index, target, token](CaptureResult result)
            {
                if (!self || pending->token != token || pending->token != self->token_)
                    return;
                if (pending->done[index])
                    return;
                pending->done[index] = true;
                QString failure;
                if (result.error != CaptureError::None)
                    failure = result.explanation.isEmpty()
                                  ? QStringLiteral("采集失败（错误码 %1）。").arg(int(result.error))
                                  : result.explanation;
                else if (result.frame.pixels.isNull())
                    failure = QStringLiteral("未获取到画面。");
                else if (result.frame.pixels.size() != quickGunnerPixelSize(target))
                    failure = QStringLiteral("捕获像素与显示器尺寸不匹配。");
                else if (!self->displays_.stillMatches(target))
                    failure = QStringLiteral("显示器布局已变化。");
                else
                {
                    QImage pixels = result.frame.pixels;
                    pixels.setDevicePixelRatio(1);
                    pending->frames[index] = std::move(pixels);
                }
                if (!failure.isEmpty())
                    pending->failures[index] = failure;
                const bool allDone = std::all_of(pending->done.begin(), pending->done.end(),
                                                 [](bool done) { return done; });
                if (allDone)
                    QTimer::singleShot(0, self,
                                       [self, pending]
                                       {
                                           if (self)
                                               self->conclude(pending);
                                       });
            });
    }
}
void MultiDisplayCaptureController::conclude(const std::shared_ptr<Pending>& pending)
{
    if (!pending || pending->token != token_)
        return;
    pending_ = 0;
    const auto prefs = settings_.loadSavePreferences();
    const bool stitch =
        dependencies_.stitchEnabled ? dependencies_.stitchEnabled() : prefs.multiDisplayStitch;
    // 汇总失败：任一屏失败即整组放弃，不落盘、不闪光。
    QStringList failures;
    failures.resize(pending->failures.size());
    bool hasFailure = false;
    for (int index = 0; index < pending->failures.size(); ++index)
    {
        if (!pending->failures[index].isEmpty())
        {
            failures[index] = pending->failures[index];
            hasFailure = true;
        }
    }
    if (hasFailure)
    {
        int failedCount = 0;
        for (const auto& failure : pending->failures)
            if (!failure.isEmpty())
                ++failedCount;
        const auto reasons = mergedReasons(failures);
        setBusy(false);
        const QString message = QStringLiteral("已取消本次多屏截图（%1/%2 屏失败）：%3")
                                    .arg(failedCount)
                                    .arg(pending->displays.size())
                                    .arg(reasons);
        emit failed(message);
        if (dependencies_.notice)
            dependencies_.notice(message);
        return;
    }
    const auto layout = buildMultiDisplayLayout(pending->displays);
    if (!layout.isValid())
    {
        setBusy(false);
        const QString message = QStringLiteral("多显示器布局无效，已取消本次截图。");
        emit failed(message);
        if (dependencies_.notice)
            dependencies_.notice(message);
        return;
    }
    if (stitch && layout.stitchExceedsLimit)
    {
        setBusy(false);
        emit failed(layout.limitReason);
        if (dependencies_.notice)
            dependencies_.notice(layout.limitReason);
        return;
    }
    MultiDisplaySaveRequest request;
    request.directory = settings_.loadQuickGunnerPreferences().directory;
    request.format = prefs.format;
    request.stitch = stitch;
    request.frames.reserve(layout.ordered.size());
    QVector<QImage> rawFrames;
    rawFrames.reserve(layout.ordered.size());
    for (const auto& entry : layout.ordered)
    {
        // 按显示器 ID 取帧：allDisplays() 的枚举顺序与排布顺序不同，不能按下标。
        const auto position = std::find_if(pending->displays.begin(), pending->displays.end(),
                                           [&entry](const DisplayTarget& target)
                                           { return target.id == entry.target.id; });
        const int index = int(position - pending->displays.begin());
        if (position == pending->displays.end() || index >= pending->frames.size() ||
            pending->frames[index].isNull())
        {
            setBusy(false);
            const QString message = QStringLiteral("显示器画面与布局不匹配，已取消本次截图。");
            emit failed(message);
            if (dependencies_.notice)
                dependencies_.notice(message);
            return;
        }
        rawFrames.append(pending->frames[index]);
        auto image = pending->frames[index];
        if (dependencies_.watermark)
            image = dependencies_.watermark(image);
        request.frames.append(image);
    }
    if (stitch)
    {
        MultiDisplayStitchRequest stitchRequest;
        stitchRequest.layout = layout;
        stitchRequest.frames = rawFrames;
        stitchRequest.format = prefs.format;
        auto stitchImage = composeMultiDisplayStitch(stitchRequest);
        if (stitchImage.isNull())
        {
            setBusy(false);
            const QString message = QStringLiteral("拼接图生成失败，已取消本次截图。");
            emit failed(message);
            if (dependencies_.notice)
                dependencies_.notice(message);
            return;
        }
        if (dependencies_.watermark)
            stitchImage = dependencies_.watermark(stitchImage);
        request.stitchImage = std::move(stitchImage);
    }
    const auto saved = dependencies_.saveGroup(request);
    setBusy(false);
    if (!saved.success)
    {
        emit failed(saved.explanation);
        if (dependencies_.notice)
            dependencies_.notice(saved.explanation);
        return;
    }
    const QString summary =
        request.stitch ? QStringLiteral("已保存 %1 张截图 + 1 张拼接图").arg(request.frames.size())
                       : QStringLiteral("已保存 %1 张截图").arg(request.frames.size());
    emit finished(summary);
    if (dependencies_.notice)
        dependencies_.notice(summary);
    if (dependencies_.flash)
    {
        int index = 0;
        for (const auto& entry : layout.ordered)
            dependencies_.flash(entry.target.logicalGeometry, index++);
    }
}
}
