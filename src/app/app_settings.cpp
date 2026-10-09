#include "app/app_settings.h"
#include "app/hotkey_rules.h"
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
namespace waibusnap
{
namespace
{
const QString sequenceKey = QStringLiteral("hotkey/sequence");
}
AppSettings::AppSettings(QString filePath) : filePath_(std::move(filePath))
{
    if (filePath_.isEmpty())
        filePath_ = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                        .filePath(QStringLiteral("settings.ini"));
}
LoadedHotkey AppSettings::loadHotkey() const
{
    QSettings settings(filePath_, QSettings::IniFormat);
    const QString text = settings.value(sequenceKey, QStringLiteral("F1")).toString().trimmed();
    const auto sequence = QKeySequence::fromString(text, QKeySequence::PortableText);
    const QString error = hotkeyValidationError(sequence);
    // 完整往返避免解析器容忍多余文本或截断长序列，把损坏配置误判为合法。
    if (settings.status() != QSettings::NoError || !error.isEmpty() ||
        sequence.toString(QKeySequence::PortableText).compare(text, Qt::CaseInsensitive) != 0)
        return {
            defaultScreenshotHotkey(),
            QStringLiteral(
                "存储的截图键无效或设置文件无法读取，已回退默认 F1；启用状态以实际注册结果为准。%1")
                .arg(error)};
    return {sequence, {}};
}
SavePreferences AppSettings::loadSavePreferences() const
{
    QSettings settings(filePath_, QSettings::IniFormat);
    SavePreferences preferences;
    const QString directory = settings.value(QStringLiteral("save/quickDirectory")).toString();
    const QString format =
        settings.value(QStringLiteral("save/format"), QStringLiteral("png")).toString();
    if (settings.status() != QSettings::NoError)
        return {};
    if (directory.isEmpty() || QDir::isAbsolutePath(directory))
        preferences.quickDirectory = directory;
    if (format == QStringLiteral("jpeg"))
        preferences.format = ImageFormat::Jpeg;
    // 非法键独立回退；读取不修复文件，也不触碰热键键。
    return preferences;
}
QString AppSettings::saveSavePreferences(const SavePreferences& preferences) const
{
    if (!preferences.quickDirectory.isEmpty() && !QDir::isAbsolutePath(preferences.quickDirectory))
        return QStringLiteral("快速保存目录必须为绝对路径。");
    if (preferences.format != ImageFormat::Png && preferences.format != ImageFormat::Jpeg)
        return QStringLiteral("保存格式无效。");
    if (!QDir().mkpath(QFileInfo(filePath_).absolutePath()))
        return QStringLiteral("无法创建设置目录，请重试保存。");
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.setValue(QStringLiteral("save/quickDirectory"), preferences.quickDirectory);
    settings.setValue(QStringLiteral("save/format"), preferences.format == ImageFormat::Jpeg
                                                         ? QStringLiteral("jpeg")
                                                         : QStringLiteral("png"));
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return QStringLiteral("保存偏好写入失败，请重试保存。");
    return {};
}
QString AppSettings::saveHotkey(const QKeySequence& sequence) const
{
    const QString error = hotkeyValidationError(sequence);
    if (!error.isEmpty())
        return error;
    if (!QDir().mkpath(QFileInfo(filePath_).absolutePath()))
        return QStringLiteral("无法创建设置目录。新键已生效，但重启后可能无法保留，请重试保存。");
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.setValue(sequenceKey, sequence.toString(QKeySequence::PortableText));
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return QStringLiteral("设置文件写入失败。新键已生效，但重启后可能无法保留，请重试保存。");
    return {};
}
HotkeySettings::HotkeySettings(AppSettings settings, GlobalHotkey& hotkey,
                               std::function<void()> handler)
    : settings_(std::move(settings)), hotkey_(hotkey), handler_(std::move(handler)),
      preferredSequence_(defaultScreenshotHotkey())
{
}
HotkeyRegistration HotkeySettings::registerSequence(const QKeySequence& sequence)
{
    const auto registration = hotkey_.registerHotkey(sequence, handler_);
    if (registration.enabled)
        enabledSequence_ = sequence;
    explanation_ = registration.explanation;
    return registration;
}
HotkeyStartup HotkeySettings::initialize()
{
    const auto loaded = settings_.loadHotkey();
    preferredSequence_ = loaded.sequence;
    QStringList report;
    QStringList warnings;
    if (!loaded.explanation.isEmpty())
    {
        report.append(loaded.explanation);
        warnings.append(loaded.explanation);
    }
    auto registration = registerSequence(preferredSequence_);
    report.append(registration.explanation);
    if (!registration.enabled && preferredSequence_ != defaultScreenshotHotkey())
    {
        warnings.append(registration.explanation);
        report.append(QStringLiteral("存储截图键注册失败，尝试默认 F1。"));
        preferredSequence_ = defaultScreenshotHotkey();
        registration = registerSequence(preferredSequence_);
        report.append(registration.explanation);
    }
    if (!registration.enabled)
        warnings.append(registration.explanation);
    return {report.join(QLatin1Char('\n')), warnings.join(QLatin1Char('\n'))};
}
QKeySequence HotkeySettings::currentSequence() const
{
    return enabled() ? enabledSequence_ : preferredSequence_;
}
QString HotkeySettings::tooltip() const
{
    return enabled() ? QStringLiteral("WaibuSnap · 截图键 %1")
                           .arg(enabledSequence_.toString(QKeySequence::NativeText))
                     : QStringLiteral("WaibuSnap · %1").arg(explanation_);
}
QString HotkeySettings::changeHotkey(const QKeySequence& sequence)
{
    const QString error = hotkeyValidationError(sequence);
    if (!error.isEmpty())
        return error;
    const auto registration = registerSequence(sequence);
    if (!registration.enabled)
        return registration.explanation;
    preferredSequence_ = sequence;
    return settings_.saveHotkey(sequence);
}
}
