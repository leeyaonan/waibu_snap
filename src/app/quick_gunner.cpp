#include "app/quick_gunner.h"
#include "app/quick_gunner_save_queue.h"
#include "ui/quick_gunner_flash_window.h"
#include "ui/quick_gunner_region_picker.h"
#include <QDir>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QTimer>
namespace waibusnap
{
namespace
{
bool containsPath(const QString& parent, const QString& child)
{
    const auto relative = QDir(parent).relativeFilePath(child);
    return relative == QStringLiteral(".") ||
           (!QDir::isAbsolutePath(relative) && relative != QStringLiteral("..") &&
            !relative.startsWith(QStringLiteral("../")));
}
QString canonicalReservedDirectory(QString path)
{
    if (!QDir::isAbsolutePath(path) || path.contains(QChar::Null))
        return {};
    path = QDir::cleanPath(path);
    QStringList missing;
    QFileInfo info(path);
    // 历史关闭时目录可能尚未创建；用最近的现存父目录解析真实路径，再补回缺失层级。
    while (!info.exists())
    {
        if (info.isSymLink() || info.fileName().isEmpty())
            return {};
        missing.prepend(info.fileName());
        const auto parent = info.absolutePath();
        if (parent == path)
            return {};
        path = parent;
        info.setFile(path);
    }
    if (!info.isDir())
        return {};
    path = info.canonicalFilePath();
    if (path.isEmpty())
        return {};
    for (const auto& name : missing)
        path = QDir(path).filePath(name);
    return path;
}
QString validateDirectory(const QString& directory, const QStringList& reservedDirectories,
                          QString& cleaned)
{
    if (directory.isEmpty() || directory.contains(QChar::Null) || !QDir::isAbsolutePath(directory))
        return QStringLiteral("请选择快枪手的绝对目录路径。");
    cleaned = QDir::cleanPath(directory);
    const QFileInfo info(cleaned);
    if (info.isSymLink())
        return QStringLiteral("快枪手目录不能是符号链接。");
    if (!info.exists())
        return QStringLiteral("快枪手目录不存在。");
    if (!info.isDir())
        return QStringLiteral("快枪手路径不是目录。");
    const QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty())
        return QStringLiteral("无法解析快枪手目录的真实路径。");
    for (const auto& reserved : reservedDirectories)
    {
        if (reserved.isEmpty())
            continue;
        const QString other = canonicalReservedDirectory(reserved);
        if (other.isEmpty())
            return QStringLiteral("无法解析保留目录，请确认历史根目录和分组目录后重试。");
        if (containsPath(canonical, other) || containsPath(other, canonical))
            return QStringLiteral("快枪手目录不能与历史根目录或分组目录相同，也不能互相包含。");
    }
    QTemporaryFile probe(
        QDir(canonical).filePath(QStringLiteral(".waibusnap-quick-gunner-XXXXXX")));
    if (!info.isWritable() || !probe.open() || probe.write("probe", 5) != 5 || !probe.flush())
        return QStringLiteral("快枪手目录不可写，请选择有写入权限的目录。");
    if (!probe.remove())
        return QStringLiteral("快枪手目录写入探针清理失败，请检查目录权限。");
    return {};
}
bool sameDisplay(const DisplayTarget& first, const DisplayTarget& second)
{
    return first.id == second.id && first.logicalGeometry == second.logicalGeometry &&
           first.devicePixelRatio == second.devicePixelRatio &&
           first.layoutVersion == second.layoutVersion &&
           first.availableLogicalGeometry == second.availableLogicalGeometry;
}
}
QuickGunnerController::QuickGunnerController(AppSettings settings, DisplayTopology& displays,
                                             CaptureProvider& capture,
                                             RegionPickerFactory pickerFactory, QObject* parent)
    : QuickGunnerController(std::move(settings), displays, capture, std::move(pickerFactory),
                            QuickGunnerDependencies{}, parent)
{
}
QuickGunnerController::QuickGunnerController(AppSettings settings, DisplayTopology& displays,
                                             CaptureProvider& capture,
                                             RegionPickerFactory pickerFactory,
                                             QuickGunnerDependencies dependencies, QObject* parent)
    : QObject(parent), settings_(std::move(settings)), displays_(displays), capture_(capture),
      pickerFactory_(std::move(pickerFactory)), preferences_(settings_.loadQuickGunnerPreferences())
{
    captureFactory_ = dependencies.captureFactory ? std::move(dependencies.captureFactory)
                                                  : createRegionCaptureOnce;
    watermark_ = dependencies.watermark ? std::move(dependencies.watermark)
                                        : [](const QImage& image) { return image; };
    saveQueue_ = dependencies.saveQueue ? std::move(dependencies.saveQueue)
                                        : std::make_unique<QuickGunnerSaveQueue>(settings_);
    saveQueue_->setDirectory(preferences_.directory);
    connect(saveQueue_.get(), &QuickGunnerSaveQueue::failed, this,
            &QuickGunnerController::shotFailed);
    connect(saveQueue_.get(), &QuickGunnerSaveQueue::dropped, this,
            &QuickGunnerController::shotsDropped);
    if (!pickerFactory_)
        pickerFactory_ = [](CaptureFrame frame, QWidget* owner)
        { return new QuickGunnerRegionPicker(std::move(frame), owner); };
}
QuickGunnerController::~QuickGunnerController()
{
    ++token_;
    ++captureToken_;
    once_.reset();
    if (picker_)
    {
        disconnect(picker_, nullptr, this, nullptr);
        picker_->close();
    }
}
void QuickGunnerController::setState(State state)
{
    if (state_ == state)
        return;
    state_ = state;
    emit stateChanged(state_);
}
QString QuickGunnerController::setDirectory(const QString& directory,
                                            const QStringList& reservedDirectories)
{
    QString cleaned;
    auto error = validateDirectory(directory, reservedDirectories, cleaned);
    if (!error.isEmpty())
        return error;
    auto next = preferences_;
    next.directory = cleaned;
    error = settings_.saveQuickGunnerPreferences(next);
    if (!error.isEmpty())
        return error;
    preferences_ = std::move(next);
    saveQueue_->setDirectory(preferences_.directory);
    emit preferencesChanged();
    return {};
}
void QuickGunnerController::finishFraming(const QString& notice)
{
    const auto token = ++token_;
    if (picker_)
    {
        auto picker = picker_;
        picker_.clear();
        disconnect(picker, nullptr, this, nullptr);
        picker->close();
    }
    // 关闭窗口期间若发生显式停用或新会话，旧结束路径不能覆盖新状态。
    if (token_ != token)
        return;
    QString finalNotice = notice;
    if (resumeActive_ && (!preferences_.region || !preferences_.region->isValid() ||
                          !displays_.stillMatches(preferences_.region->display)))
    {
        if (!finalNotice.isEmpty())
            finalNotice += QLatin1Char('\n');
        finalNotice += QStringLiteral("机位无效或显示器布局已变化，请重新框选机位。");
    }
    const State next =
        resumeActive_ || (activateAfter_ && framingAccepted_) ? State::Active : State::Idle;
    resumeActive_ = false;
    activateAfter_ = false;
    framingAccepted_ = false;
    setState(next);
    QTimer::singleShot(0, this, &QuickGunnerController::pumpCapture);
    if (!finalNotice.isEmpty())
        emit noticeChanged(finalNotice);
}
QString QuickGunnerController::beginFraming(QWidget* parent, bool activateAfterSelection)
{
    if (state_ == State::Framing)
        return QStringLiteral("正在框选快枪手机位，请先确认或取消。");
    if (!capture_.hasPermission())
    {
        capture_.requestPermission();
        if (!capture_.hasPermission())
            return capture_.permissionExplanation();
    }
    const auto display = displays_.cursorDisplay();
    if (!display.error.isEmpty())
        return display.error;
    if (quickGunnerPixelSize(display.target).isEmpty() || !displays_.stillMatches(display.target))
        return QStringLiteral("显示器信息无效或布局已变化，请重新框选机位。");
    if (capturing_)
        return QStringLiteral("正在拍摄，请稍后调整机位。");
    resumeActive_ = active();
    activateAfter_ = activateAfterSelection;
    framingAccepted_ = false;
    if (flash_)
        flash_->hide();
    const auto token = ++token_;
    setState(State::Framing);
    QPointer<QuickGunnerController> self(this);
    QPointer<QWidget> owner(parent);
    capture_.capture(
        display.target,
        [self, owner, requiresParent = parent != nullptr, token,
         target = display.target](CaptureResult result)
        {
            if (!self || self->token_ != token || self->state_ != State::Framing || self->picker_)
                return;
            if (requiresParent && !owner)
            {
                self->finishFraming();
                return;
            }
            if (result.error != CaptureError::None)
            {
                self->finishFraming(result.explanation.isEmpty()
                                        ? QStringLiteral("快枪手机位冻结帧获取失败，请重试。")
                                        : result.explanation);
                return;
            }
            if (!self->displays_.stillMatches(target) ||
                !sameDisplay(target, result.frame.display) || result.frame.pixels.isNull() ||
                result.frame.pixels.size() != quickGunnerPixelSize(target))
            {
                self->finishFraming(QStringLiteral("冻结帧与显示器不匹配，请重新框选机位。"));
                return;
            }
            auto* picker = self->pickerFactory_(std::move(result.frame), owner);
            if (!picker)
            {
                self->finishFraming(QStringLiteral("无法创建快枪手机位选择窗口。"));
                return;
            }
            self->picker_ = picker;
            connect(picker, &QuickGunnerRegionPicker::regionSelected, self,
                    [self, token, target](QRect pixels)
                    {
                        if (!self || self->token_ != token || self->state_ != State::Framing)
                            return;
                        const QuickGunnerRegion region{target, pixels};
                        if (!region.isValid() || !self->displays_.stillMatches(target))
                        {
                            self->finishFraming(
                                QStringLiteral("机位无效或显示器布局已变化，请重新框选。"));
                            return;
                        }
                        auto next = self->preferences_;
                        next.region = region;
                        const auto error = self->settings_.saveQuickGunnerPreferences(next);
                        if (error.isEmpty())
                        {
                            self->preferences_ = std::move(next);
                            self->framingAccepted_ = true;
                        }
                        self->finishFraming(error);
                        if (self && error.isEmpty())
                            emit self->preferencesChanged();
                    });
            connect(picker, &QuickGunnerRegionPicker::cancelled, self,
                    [self, token]
                    {
                        if (self && self->token_ == token && self->state_ == State::Framing)
                            self->finishFraming();
                    });
            connect(picker, &QObject::destroyed, self,
                    [self, token]
                    {
                        if (self && self->token_ == token && self->state_ == State::Framing)
                            self->finishFraming();
                    });
            picker->show();
            picker->raise();
            picker->activateWindow();
            picker->setFocus();
        });
    return {};
}
QString QuickGunnerController::activate(const QStringList& reservedDirectories)
{
    if (state_ == State::Framing)
        return QStringLiteral("请先确认或取消机位框选。");
    QString cleaned;
    const auto error = validateDirectory(preferences_.directory, reservedDirectories, cleaned);
    if (!error.isEmpty())
        return error;
    if (!preferences_.region || !preferences_.region->isValid())
        return QStringLiteral("请先框选有效机位（至少 16×16 物理像素）。");
    if (!displays_.stillMatches(preferences_.region->display))
        return QStringLiteral("显示器布局已变化，请重新框选机位。");
    setState(State::Active);
    return {};
}
void QuickGunnerController::deactivate()
{
    resumeActive_ = false;
    activateAfter_ = false;
    framingAccepted_ = false;
    finishFraming();
}
int QuickGunnerController::pendingSaveCount() const { return saveQueue_->pendingCount(); }
QString QuickGunnerController::shutter()
{
    if (state_ == State::Framing)
        return QStringLiteral("正在框选快枪手机位，请先确认或取消。");
    if (preferences_.directory.isEmpty())
        return QStringLiteral("请先选择快枪手目录。");
    if (!preferences_.region || !preferences_.region->isValid())
        return QStringLiteral("请先框选有效机位（至少 16×16 物理像素）。");
    if (!displays_.stillMatches(preferences_.region->display))
        return QStringLiteral("显示器布局已变化，请重新框选机位。");
    if (!capture_.hasPermission())
        return capture_.permissionExplanation();
    if (!flash_)
        flash_ = std::make_unique<QuickGunnerFlashWindow>();
    RegionCaptureRequest request{preferences_.region->display, preferences_.region->pixelRect, {}};
    const auto id = regionCaptureWindowId(flash_->windowHandle());
    if (id)
        request.excludedWindowIds.append(id);
    if (pending_.size() == 8)
    {
        pending_.pop_front();
        emit shotsDropped(1);
    }
    pending_.push_back(std::move(request));
    pumpCapture();
    return {};
}
void QuickGunnerController::pumpCapture()
{
    if (capturing_ || pending_.empty() || state_ == State::Framing)
        return;
    const auto request = std::move(pending_.front());
    pending_.pop_front();
    if (flash_)
        flash_->hide();
    capturing_ = true;
    const auto token = ++captureToken_;
    once_ = captureFactory_();
    QPointer<QuickGunnerController> self(this);
    const auto completed = [self, token, request](QImage image, QString error)
    {
        if (!self)
            return;
        // 即使注入实现同步完成，也延迟到 GUI 队列交付，避免在 capture 栈内析构实现。
        QMetaObject::invokeMethod(
            self,
            [self, token, request, image = std::move(image), error]() mutable
            {
                if (!self || !self->capturing_ || self->captureToken_ != token)
                    return;
                self->once_.reset();
                self->capturing_ = false;
                if (error.isEmpty() && (!self->displays_.stillMatches(request.display) ||
                                        image.size() != request.pixelRect.size()))
                    error = QStringLiteral("拍摄结果与机位不匹配，请重新框选机位。");
                if (image.isNull() && error.isEmpty())
                    error = QStringLiteral("快枪手未获取到图片，请重试。");
                if (!error.isEmpty())
                    emit self->shotFailed(error);
                else
                {
                    image.setDevicePixelRatio(1);
                    self->saveQueue_->enqueue(self->watermark_(image));
                    const auto dpr = request.display.devicePixelRatio;
                    const auto rect = request.pixelRect;
                    const QRect logical(
                        request.display.logicalGeometry.topLeft() +
                            QPoint(qRound(rect.x() / dpr), qRound(rect.y() / dpr)),
                        QSize(qRound(rect.width() / dpr), qRound(rect.height() / dpr)));
                    emit self->shotCaptured(logical);
                    if (self->flash_)
                        self->flash_->flash(logical);
                }
                if (self)
                    self->pumpCapture();
            },
            Qt::QueuedConnection);
    };
    if (!displays_.stillMatches(request.display) || !capture_.hasPermission())
        completed({}, QStringLiteral("显示器布局或屏幕录制权限已变化，请检查后重试。"));
    else if (!once_)
        completed({}, QStringLiteral("无法创建快枪手单帧采集器。"));
    else
        once_->capture(request, completed);
}

}
