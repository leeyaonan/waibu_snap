#include "app/quick_gunner.h"
#include "ui/quick_gunner_region_picker.h"
#include <QDir>
#include <QFileInfo>
#include <QTemporaryFile>
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
    : QObject(parent), settings_(std::move(settings)), displays_(displays), capture_(capture),
      pickerFactory_(std::move(pickerFactory)), preferences_(settings_.loadQuickGunnerPreferences())
{
    if (!pickerFactory_)
        pickerFactory_ = [](CaptureFrame frame, QWidget* owner)
        { return new QuickGunnerRegionPicker(std::move(frame), owner); };
}
QuickGunnerController::~QuickGunnerController()
{
    ++token_;
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
    emit preferencesChanged();
    return {};
}
void QuickGunnerController::finishFraming(const QString& notice)
{
    ++token_;
    if (picker_)
    {
        auto picker = picker_;
        picker_.clear();
        disconnect(picker, nullptr, this, nullptr);
        picker->close();
    }
    setState(State::Idle);
    if (!notice.isEmpty())
        emit noticeChanged(notice);
}
QString QuickGunnerController::beginFraming(QWidget* parent)
{
    if (state_ == State::Framing)
        return QStringLiteral("正在框选快枪手机位，请先确认或取消。");
    if (!capture_.hasPermission())
        return capture_.permissionExplanation();
    const auto display = displays_.cursorDisplay();
    if (!display.error.isEmpty())
        return display.error;
    if (quickGunnerPixelSize(display.target).isEmpty() || !displays_.stillMatches(display.target))
        return QStringLiteral("显示器信息无效或布局已变化，请重新框选机位。");
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
                            self->preferences_ = std::move(next);
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
void QuickGunnerController::deactivate() { finishFraming(); }
}
