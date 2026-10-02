#include "app/app_settings.h"
#include "app/hotkey_rules.h"
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>
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
}
