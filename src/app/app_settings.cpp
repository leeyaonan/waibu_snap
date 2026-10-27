#include "app/app_settings.h"
#include "app/history_store.h"
#include "app/hotkey_rules.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>
#include <QUuid>
#include <algorithm>
namespace waibusnap
{
namespace
{
QString sequenceKey(HotkeyAction action)
{
    switch (action)
    {
    case HotkeyAction::Screenshot:
        return QStringLiteral("hotkey/sequence");
    case HotkeyAction::HistoryPrevious:
        return QStringLiteral("hotkey/historyPrevious");
    case HotkeyAction::HistoryNext:
        return QStringLiteral("hotkey/historyNext");
    case HotkeyAction::HistoryOpen:
        return QStringLiteral("hotkey/historyOpen");
    }
    return {};
}
const QString stickerCloseReminderKey = QStringLiteral("sticker/confirmUnsavedClose");
const QString stickerDoubleClickCloseKey = QStringLiteral("sticker/doubleClickClose");
const QString watermarkPreferencesKey = QStringLiteral("watermark/preferences");
const QString watermarkAutoEnabledKey = QStringLiteral("watermark/autoEnabled");
}
AppSettings::AppSettings(QString filePath) : filePath_(std::move(filePath))
{
    if (filePath_.isEmpty())
        filePath_ = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                        .filePath(QStringLiteral("settings.ini"));
}
QuickGunnerPreferences AppSettings::loadQuickGunnerPreferences(QString* notice) const
{
    if (notice)
        notice->clear();
    const auto fail = [notice](const QString& reason)
    {
        if (notice)
            *notice = QStringLiteral("快枪手设置无法读取或已损坏，已回退为空设置；原文件未改写。%1")
                          .arg(reason);
        return QuickGunnerPreferences{};
    };
    QFile file(filePath_);
    if (!file.open(QIODevice::ReadOnly))
        return QFileInfo::exists(filePath_) ? fail(QStringLiteral("文件不可读。"))
                                            : QuickGunnerPreferences{};
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return fail(QStringLiteral("INI 格式无效。"));
    QuickGunnerPreferences result;
    const auto directory = settings.value(QStringLiteral("quickGunner/directory"), QString());
    if (directory.metaType().id() != QMetaType::QString ||
        directory.toString().contains(QChar::Null) ||
        (!directory.toString().isEmpty() && !QDir::isAbsolutePath(directory.toString())))
        return fail(QStringLiteral("目录必须为绝对路径。"));
    if (!directory.toString().isEmpty())
        result.directory = QDir::cleanPath(directory.toString());
    const auto value = settings.value(QStringLiteral("quickGunner/region"));
    if (value.isValid())
    {
        const auto document = QJsonDocument::fromJson(value.toString().toUtf8());
        QString reason;
        if (value.metaType().id() != QMetaType::QString || !document.isObject())
            return fail(QStringLiteral("机位 JSON 格式无效。"));
        result.region = QuickGunnerRegion::fromJson(document.object(), &reason);
        if (!result.region)
            return fail(reason);
    }
    return result;
}
QString AppSettings::saveQuickGunnerPreferences(const QuickGunnerPreferences& preferences) const
{
    if (preferences.directory.contains(QChar::Null) ||
        (!preferences.directory.isEmpty() && !QDir::isAbsolutePath(preferences.directory)))
        return QStringLiteral("快枪手目录必须为绝对路径。");
    if (preferences.region && !preferences.region->isValid())
        return QStringLiteral("快枪手机位无效，必须在显示器内且至少为 16×16 物理像素。");
    const QString error = QStringLiteral("快枪手设置写入失败，原设置已保留，请重试保存。");
    const QString parent = QFileInfo(filePath_).absolutePath();
    if (!QDir().mkpath(parent) || QFileInfo(filePath_).isDir())
        return error;
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.setAtomicSyncRequired(true);
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return error;
    const QString directoryKey = QStringLiteral("quickGunner/directory");
    const QString regionKey = QStringLiteral("quickGunner/region");
    const auto oldDirectory = settings.value(directoryKey);
    const auto oldRegion = settings.value(regionKey);
    settings.setValue(directoryKey, preferences.directory.isEmpty()
                                        ? QString()
                                        : QDir::cleanPath(preferences.directory));
    if (preferences.region)
        settings.setValue(
            regionKey,
            QString::fromUtf8(
                QJsonDocument(preferences.region->toJson()).toJson(QJsonDocument::Compact)));
    else
        settings.remove(regionKey);
    settings.sync();
    if (settings.status() != QSettings::NoError)
    {
        // 与其他设置入口一致：原子同步失败时恢复共享缓存，避免后续保存夹带失败值。
        if (oldDirectory.isValid())
            settings.setValue(directoryKey, oldDirectory);
        else
            settings.remove(directoryKey);
        if (oldRegion.isValid())
            settings.setValue(regionKey, oldRegion);
        else
            settings.remove(regionKey);
        return error;
    }
    return {};
}
HistoryPreferences AppSettings::loadHistoryPreferences(QString* notice) const
{
    HistoryPreferences result{HistoryStore::defaultRootDirectory(), {}, {}, false};
    if (notice)
        notice->clear();
    QFile file(filePath_);
    if (historyWriteFailed_ || !file.open(QIODevice::ReadOnly))
    {
        if (notice && (historyWriteFailed_ || QFileInfo::exists(filePath_)))
            *notice = QStringLiteral("历史设置无法读取或上次写入失败，已回退默认值。");
        return result;
    }
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.sync();
    if (settings.status() != QSettings::NoError)
    {
        if (notice)
            *notice = QStringLiteral("历史设置文件损坏，已回退默认值。");
        return result;
    }
    bool invalid = false;
    const auto root = settings.value(QStringLiteral("history/rootDirectory"), QString());
    if (root.metaType().id() != QMetaType::QString ||
        (!root.toString().isEmpty() && !QDir::isAbsolutePath(root.toString())))
        invalid = true;
    else if (!root.toString().isEmpty())
        result.rootDirectory = root.toString();
    const auto id = settings.value(QStringLiteral("history/currentGroupId"), QString());
    if (id.metaType().id() != QMetaType::QString ||
        (!id.toString().isEmpty() &&
         (QUuid(id.toString()).isNull() ||
          QUuid(id.toString()).toString(QUuid::WithoutBraces) != id.toString())))
        invalid = true;
    else
        result.currentGroupId = id.toString();
    const QString policyKey = QStringLiteral("history/defaultCleanup");
    if (settings.contains(policyKey))
    {
        const auto value = settings.value(policyKey);
        const auto document = QJsonDocument::fromJson(value.toString().toUtf8());
        if (value.metaType().id() != QMetaType::QString || !document.isObject() ||
            !cleanupPolicyFromJson(document.object(), result.defaultCleanup))
            invalid = true;
    }
    const auto enabled = settings.value(QStringLiteral("history/enabled"), false);
    if (enabled.metaType().id() != QMetaType::Bool)
        invalid = true;
    else
        result.enabled = enabled.toBool();
    if (notice && invalid)
        *notice = QStringLiteral("部分历史设置无效，相关字段已回退默认值；原文件未修复。");
    return result;
}
QString AppSettings::saveHistoryPreferences(const HistoryPreferences& preferences) const
{
    QString error = cleanupPolicyError(preferences.defaultCleanup);
    if (!error.isEmpty())
        return error;
    if (!preferences.rootDirectory.isEmpty() && !QDir::isAbsolutePath(preferences.rootDirectory))
        return QStringLiteral("历史根目录必须为绝对路径。");
    if (!preferences.currentGroupId.isEmpty() &&
        (QUuid(preferences.currentGroupId).isNull() ||
         QUuid(preferences.currentGroupId).toString(QUuid::WithoutBraces) !=
             preferences.currentGroupId))
        return QStringLiteral("当前历史分组 UUID 无效。");
    historyWriteFailed_ = true;
    error = QStringLiteral("历史设置写入失败，重启后可能无法保留，请重试保存。");
    if (!QDir().mkpath(QFileInfo(filePath_).absolutePath()) || QFileInfo(filePath_).isDir())
        return error;
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return error;
    const QStringList keys{
        QStringLiteral("history/rootDirectory"), QStringLiteral("history/currentGroupId"),
        QStringLiteral("history/defaultCleanup"), QStringLiteral("history/enabled")};
    QVector<QVariant> previous;
    for (const auto& key : keys)
        previous.append(settings.value(key));
    settings.setValue(keys[0], preferences.rootDirectory);
    settings.setValue(keys[1], preferences.currentGroupId);
    settings.setValue(
        keys[2], QString::fromUtf8(QJsonDocument(cleanupPolicyToJson(preferences.defaultCleanup))
                                       .toJson(QJsonDocument::Compact)));
    settings.setValue(keys[3], preferences.enabled);
    settings.sync();
    if (settings.status() != QSettings::NoError)
    {
        // 不让失败写入留在进程共享缓存中，被下一次无关设置保存意外提交。
        for (qsizetype i = 0; i < keys.size(); ++i)
            if (previous[i].isValid())
                settings.setValue(keys[i], previous[i]);
            else
                settings.remove(keys[i]);
        return error;
    }
    historyWriteFailed_ = false;
    return {};
}
QString AppSettings::saveHistoryCurrentGroupId(const QString& groupId) const
{
    if (QUuid(groupId).isNull() || QUuid(groupId).toString(QUuid::WithoutBraces) != groupId)
        return QStringLiteral("当前历史分组 UUID 无效。");
    const QString error = QStringLiteral("当前分组写入失败，原分组偏好已保留，请重试保存。");
    if (!QDir().mkpath(QFileInfo(filePath_).absolutePath()) || QFileInfo(filePath_).isDir())
        return error;
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return error;
    const QString key = QStringLiteral("history/currentGroupId");
    const auto previous = settings.value(key);
    settings.setValue(key, groupId);
    settings.sync();
    if (settings.status() != QSettings::NoError)
    {
        if (previous.isValid())
            settings.setValue(key, previous);
        else
            settings.remove(key);
        return error;
    }
    return {};
}
HistoryWindowState AppSettings::loadHistoryWindowState() const
{
    QFile file(filePath_);
    if (historyWindowWriteFailed_ || !file.open(QIODevice::ReadOnly))
        return {};
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return {};
    HistoryWindowState result;
    const QString prefix = QStringLiteral("history/window/");
    const auto geometry = settings.value(prefix + QStringLiteral("geometry"));
    const auto splitter = settings.value(prefix + QStringLiteral("splitterState"));
    if (geometry.metaType().id() == QMetaType::QByteArray)
        result.geometry = geometry.toByteArray();
    if (splitter.metaType().id() == QMetaType::QByteArray)
        result.splitterState = splitter.toByteArray();
    const QString view = settings.value(prefix + QStringLiteral("lastView")).toString();
    if (view == QStringLiteral("all") || view == QStringLiteral("starred") ||
        (!QUuid(view).isNull() && QUuid(view).toString(QUuid::WithoutBraces) == view))
        result.lastView = view;
    result.lastSelection = settings.value(prefix + QStringLiteral("lastSelection")).toString();
    const QString order = settings.value(prefix + QStringLiteral("sortOrder")).toString();
    if (order == QStringLiteral("oldest") || order == QStringLiteral("newest"))
        result.sortOrder = order;
    return result;
}
QString AppSettings::saveHistoryWindowState(const HistoryWindowState& state) const
{
    historyWindowWriteFailed_ = true;
    const QString error = QStringLiteral("历史窗口状态保存失败，下次将使用默认窗口状态。");
    if (!QDir().mkpath(QFileInfo(filePath_).absolutePath()) || QFileInfo(filePath_).isDir())
        return error;
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return error;
    const QString prefix = QStringLiteral("history/window/");
    const QStringList names{QStringLiteral("geometry"), QStringLiteral("splitterState"),
                            QStringLiteral("lastView"), QStringLiteral("lastSelection"),
                            QStringLiteral("sortOrder")};
    const QVector<QVariant> values{state.geometry, state.splitterState, state.lastView,
                                   state.lastSelection, state.sortOrder};
    QVector<QVariant> previous;
    for (qsizetype i = 0; i < names.size(); ++i)
    {
        previous.append(settings.value(prefix + names[i]));
        settings.setValue(prefix + names[i], values[i]);
    }
    settings.sync();
    if (settings.status() != QSettings::NoError)
    {
        for (qsizetype i = 0; i < names.size(); ++i)
            if (previous[i].isValid())
                settings.setValue(prefix + names[i], previous[i]);
            else
                settings.remove(prefix + names[i]);
        return error;
    }
    historyWindowWriteFailed_ = false;
    return {};
}
LoadedHotkey AppSettings::loadHotkey(HotkeyAction action) const
{
    QSettings settings(filePath_, QSettings::IniFormat);
    const QString text =
        settings
            .value(sequenceKey(action), defaultHotkey(action).toString(QKeySequence::PortableText))
            .toString()
            .trimmed();
    const auto sequence = QKeySequence::fromString(text, QKeySequence::PortableText);
    const QString error = hotkeyValidationError(sequence);
    // 完整往返避免解析器容忍多余文本或截断长序列，把损坏配置误判为合法。
    if (settings.status() != QSettings::NoError || !error.isEmpty() ||
        sequence.toString(QKeySequence::PortableText).compare(text, Qt::CaseInsensitive) != 0)
        return {
            defaultHotkey(action),
            QStringLiteral(
                "存储的%1键无效或设置文件无法读取，已回退默认 %2；启用状态以实际注册结果为准。%3")
                .arg(hotkeyActionName(action),
                     defaultHotkey(action).toString(QKeySequence::PortableText), error)};
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
bool AppSettings::loadStickerCloseReminder() const
{
    if (stickerCloseReminderWriteFailed_)
        return true;
    // 先验证实际文件可读，避免 QSettings 缓存掩盖路径失效或权限变化。
    QFile file(filePath_);
    if (!file.open(QIODevice::ReadOnly))
        return true;
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.sync();
    const QString value = settings.value(stickerCloseReminderKey, true).toString();
    // 不用 toBool() 容忍任意字符串；只有明确 false 才允许跳过确认。
    return settings.status() != QSettings::NoError || value != QStringLiteral("false");
}
std::optional<bool> AppSettings::loadStickerDoubleClickClose() const
{
    if (stickerDoubleClickCloseWriteFailed_)
        return false;
    QFile file(filePath_);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return std::nullopt;
    if (!settings.contains(stickerDoubleClickCloseKey))
        return std::nullopt;
    const QString value = settings.value(stickerDoubleClickCloseKey).toString();
    if (value == QStringLiteral("true"))
        return true;
    if (value == QStringLiteral("false"))
        return false;
    return std::nullopt;
}
QString AppSettings::saveStickerDoubleClickClose(bool enabled) const
{
    stickerDoubleClickCloseWriteFailed_ = true;
    const QString error = QStringLiteral("双击关闭贴图设置写入失败，请重试保存。");
    if (!QDir().mkpath(QFileInfo(filePath_).absolutePath()))
        return error;
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return error;
    settings.setValue(stickerDoubleClickCloseKey, enabled);
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return error;
    stickerDoubleClickCloseWriteFailed_ = false;
    return {};
}
QString AppSettings::saveStickerCloseReminder(bool enabled) const
{
    stickerCloseReminderWriteFailed_ = true;
    const QString error = QStringLiteral("贴图关闭提醒设置写入失败，已保持提醒开启，请重试保存。");
    if (!QDir().mkpath(QFileInfo(filePath_).absolutePath()))
        return error;
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return error;
    settings.setValue(stickerCloseReminderKey, enabled);
    settings.sync();
    if (settings.status() != QSettings::NoError)
    {
        // 失败的 false 不能留在进程共享的待写缓存中影响其他贴图。
        settings.setValue(stickerCloseReminderKey, true);
        return error;
    }
    stickerCloseReminderWriteFailed_ = false;
    return {};
}
WatermarkPreferences AppSettings::loadWatermarkPreferences(QString* notice) const
{
    WatermarkPreferences result;
    if (notice)
        notice->clear();
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.sync();
    if (watermarkWriteFailed_ || settings.status() != QSettings::NoError)
    {
        if (notice && (watermarkWriteFailed_ || QFileInfo::exists(filePath_)))
            *notice = QStringLiteral("水印设置无法读取或上次写入失败，已回退为空设置。");
        return result;
    }
    const auto value = settings.value(watermarkPreferencesKey);
    if (!value.isValid())
        return result;
    if (value.metaType().id() != QMetaType::QString)
    {
        if (notice)
            *notice = QStringLiteral("水印设置格式无效，已回退为空设置；原文件未修复。");
        return result;
    }
    const auto document = QJsonDocument::fromJson(value.toString().toUtf8());
    QString error;
    if (!document.isObject() || !watermarkPreferencesFromJson(document.object(), result, &error))
    {
        if (notice)
            *notice = QStringLiteral("水印设置损坏，已回退为空设置：%1").arg(error);
        result = {};
        return result;
    }
    const auto automatic = settings.value(watermarkAutoEnabledKey);
    if (automatic.isValid() && automatic.metaType().id() == QMetaType::Bool)
        result.autoEnabled = automatic.toBool();
    return result;
}
QString AppSettings::saveWatermarkPreferences(const WatermarkPreferences& preferences) const
{
    const QString validation = watermarkPreferencesError(preferences);
    if (!validation.isEmpty())
        return validation;
    watermarkWriteFailed_ = true;
    const QString error = QStringLiteral("水印设置写入失败，重启后可能无法保留，请重试保存。");
    if (!QDir().mkpath(QFileInfo(filePath_).absolutePath()) || QFileInfo(filePath_).isDir())
        return error;
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return error;
    const auto previousPreferences = settings.value(watermarkPreferencesKey);
    const auto previousAuto = settings.value(watermarkAutoEnabledKey);
    settings.setValue(
        watermarkPreferencesKey,
        QString::fromUtf8(
            QJsonDocument(watermarkPreferencesToJson(preferences)).toJson(QJsonDocument::Compact)));
    settings.setValue(watermarkAutoEnabledKey, preferences.autoEnabled);
    settings.sync();
    if (settings.status() != QSettings::NoError)
    {
        if (previousPreferences.isValid())
            settings.setValue(watermarkPreferencesKey, previousPreferences);
        else
            settings.remove(watermarkPreferencesKey);
        if (previousAuto.isValid())
            settings.setValue(watermarkAutoEnabledKey, previousAuto);
        else
            settings.remove(watermarkAutoEnabledKey);
        return error;
    }
    watermarkWriteFailed_ = false;
    return {};
}
CaptureRatioSettings AppSettings::loadCaptureRatios() const
{
    QSettings settings(filePath_, QSettings::IniFormat);
    const QString enabledKey = QStringLiteral("capture/ratio.enabled");
    const QString disabledKey = QStringLiteral("capture/ratio.disabled");
    if (!settings.contains(enabledKey) && !settings.contains(disabledKey))
        return {defaultCaptureRatios(), {}, AspectRatio{1, 1}};
    const auto listText = [&settings](const QString& key)
    {
        const auto value = settings.value(key);
        // 手写 INI 未加引号的逗号列表会被 QSettings 解析为 QStringList。
        return value.metaType().id() == QMetaType::QStringList
                   ? value.toStringList().join(QLatin1Char(','))
                   : value.toString();
    };
    return sanitizedRatioSettings(
        {parseRatioList(listText(enabledKey)), parseRatioList(listText(disabledKey)),
         parseAspectRatio(settings.value(QStringLiteral("capture/ratio.quick")).toString())});
}
QString AppSettings::shapeTemplateDir() const
{
    return QDir(QFileInfo(filePath_).absolutePath()).filePath(QStringLiteral("shapes"));
}
QVector<ShapeTemplateEntry> AppSettings::loadShapeTemplates(QString* notice) const
{
    QSettings settings(filePath_, QSettings::IniFormat);
    const auto value = settings.value(QStringLiteral("capture/shape.templates"));
    const QString text = value.metaType().id() == QMetaType::QStringList
                             ? value.toStringList().join(QLatin1Char(','))
                             : value.toString();
    QVector<ShapeTemplateEntry> result;
    bool ignored = settings.status() != QSettings::NoError;
    for (const auto& entry : parseShapeTemplates(text))
    {
        QImageReader reader(QDir(shapeTemplateDir()).filePath(entry.file));
        reader.setDecideFormatFromContent(true);
        if (reader.format() == QByteArray("png") && !reader.read().isNull())
            result.append(entry);
        else
            ignored = true;
    }
    if (notice)
        *notice = ignored ? QStringLiteral("部分形状模板缺失或损坏，已忽略。") : QString();
    return result;
}
ShapeTemplateImport AppSettings::importShapeTemplates(const QStringList& paths) const
{
    ShapeTemplateImport result;
    QStringList errors;
    for (const QString& path : paths)
    {
        QImageReader reader(path);
        reader.setDecideFormatFromContent(true);
        QImage image;
        if (reader.format() == QByteArray("png"))
            image = reader.read();
        if (image.isNull())
        {
            errors.append(QStringLiteral("模板导入失败：不是有效的 PNG。"));
            continue;
        }
        const QImage pixels = image.convertToFormat(QImage::Format_ARGB32);
        bool visible = false;
        for (int y = 0; y < pixels.height() && !visible; ++y)
        {
            const auto* row = reinterpret_cast<const QRgb*>(pixels.constScanLine(y));
            for (int x = 0; x < pixels.width(); ++x)
                if (qAlpha(row[x]) != 0)
                {
                    visible = true;
                    break;
                }
        }
        if (!visible)
        {
            errors.append(QStringLiteral("模板没有任何可见像素，已忽略。"));
            continue;
        }
        if (!QDir().mkpath(shapeTemplateDir()))
        {
            errors.append(QStringLiteral("模板导入失败：无法创建模板目录。"));
            continue;
        }
        if (image.width() > 4096 || image.height() > 4096)
            image = image.scaled(4096, 4096, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        image.setDevicePixelRatio(1);
        QString base = QFileInfo(path).completeBaseName().trimmed();
        base.replace(QRegularExpression(QStringLiteral(R"([<>:"/\\|?*,])")), QStringLiteral("_"));
        if (base.isEmpty())
            base = QStringLiteral("template");
        QString file = base + QStringLiteral(".png");
        int suffix = 2;
        const QDir directory(shapeTemplateDir());
        while (QFileInfo::exists(directory.filePath(file)) ||
               QFileInfo(directory.filePath(file)).isSymLink())
            file = base + QStringLiteral("-%1.png").arg(suffix++);
        QSaveFile output(directory.filePath(file));
        if (!output.open(QIODevice::WriteOnly) || !image.save(&output, "PNG") || !output.commit())
        {
            errors.append(QStringLiteral("模板导入失败：无法写入模板文件。"));
            continue;
        }
        result.imported.append({file, QFileInfo(file).completeBaseName(), true});
    }
    errors.removeDuplicates();
    result.error = errors.join(QLatin1Char('\n'));
    return result;
}
QString AppSettings::saveShapeTemplates(const QVector<ShapeTemplateEntry>& entries) const
{
    if (!QDir().mkpath(QFileInfo(filePath_).absolutePath()))
        return QStringLiteral("无法创建设置目录。形状模板重启后可能无法保留，请重试保存。");
    const auto previous = loadShapeTemplates();
    const QString serialized = serializeShapeTemplates(entries);
    const auto clean = parseShapeTemplates(serialized);
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.setValue(QStringLiteral("capture/shape.templates"), serialized);
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return QStringLiteral("形状模板写入失败，重启后可能无法保留，请重试保存。");
    for (const auto& old : previous)
        if (std::none_of(clean.begin(), clean.end(), [&old](const ShapeTemplateEntry& entry)
                         { return entry.file == old.file; }))
        {
            const QString path = QDir(shapeTemplateDir()).filePath(old.file);
            if (QFileInfo::exists(path) && !QFile::remove(path))
                return QStringLiteral("模板列表已保存，但模板文件删除失败，文件已保留。");
        }
    return {};
}
QString AppSettings::removeShapeTemplate(const QString& file) const
{
    auto entries = loadShapeTemplates();
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&file](const ShapeTemplateEntry& entry)
                                 { return entry.file == file; }),
                  entries.end());
    return saveShapeTemplates(entries);
}
QString AppSettings::saveCaptureRatios(const CaptureRatioSettings& ratios) const
{
    if (!QDir().mkpath(QFileInfo(filePath_).absolutePath()))
        return QStringLiteral("无法创建设置目录。截图比例重启后可能无法保留，请重试保存。");
    const auto clean = sanitizedRatioSettings(ratios);
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.setValue(QStringLiteral("capture/ratio.enabled"), serializeRatioList(clean.enabled));
    settings.setValue(QStringLiteral("capture/ratio.disabled"), serializeRatioList(clean.disabled));
    settings.setValue(QStringLiteral("capture/ratio.quick"),
                      clean.quick ? formatAspectRatio(*clean.quick) : QString());
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return QStringLiteral("截图比例写入失败，重启后可能无法保留，请重试保存。");
    return {};
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
QString AppSettings::saveHotkey(const QKeySequence& sequence, HotkeyAction action) const
{
    const QString error = hotkeyValidationError(sequence);
    if (!error.isEmpty())
        return error;
    if (!QDir().mkpath(QFileInfo(filePath_).absolutePath()))
        return QStringLiteral("无法创建设置目录。新键已生效，但重启后可能无法保留，请重试保存。");
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.setValue(sequenceKey(action), sequence.toString(QKeySequence::PortableText));
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return QStringLiteral("设置文件写入失败。新键已生效，但重启后可能无法保留，请重试保存。");
    return {};
}
HotkeySettings::HotkeySettings(AppSettings settings, GlobalHotkey& hotkey,
                               std::function<void()> handler, HotkeyAction action)
    : action_(action), settings_(std::move(settings)), hotkey_(hotkey),
      handler_(std::move(handler)), preferredSequence_(defaultHotkey(action))
{
}
HotkeyRegistration HotkeySettings::registerSequence(const QKeySequence& sequence)
{
    const auto registration = hotkey_.registerHotkey(action_, sequence, handler_);
    if (registration.enabled)
        enabledSequence_ = sequence;
    explanation_ = registration.explanation;
    return registration;
}
HotkeyStartup HotkeySettings::initialize()
{
    const auto loaded = settings_.loadHotkey(action_);
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
    if (!registration.enabled && preferredSequence_ != defaultHotkey(action_))
    {
        warnings.append(registration.explanation);
        report.append(QStringLiteral("存储%1键注册失败，尝试默认 %2。")
                          .arg(hotkeyActionName(action_),
                               defaultHotkey(action_).toString(QKeySequence::PortableText)));
        preferredSequence_ = defaultHotkey(action_);
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
    return settings_.saveHotkey(sequence, action_);
}
}
