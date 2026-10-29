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
#include <initializer_list>
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
    case HotkeyAction::QuickGunnerShutter:
        return QStringLiteral("hotkey/quickGunner");
    case HotkeyAction::RecordingStartStop:
        return QStringLiteral("hotkey/recordingStartStop");
    case HotkeyAction::RecordingPauseResume:
        return QStringLiteral("hotkey/recordingPauseResume");
    }
    return {};
}
const QString stickerCloseReminderKey = QStringLiteral("sticker/confirmUnsavedClose");
const QString stickerDoubleClickCloseKey = QStringLiteral("sticker/doubleClickClose");
const QString watermarkPreferencesKey = QStringLiteral("watermark/preferences");
const QString watermarkAutoEnabledKey = QStringLiteral("watermark/autoEnabled");
const QString recordingVideoDirectoryKey = QStringLiteral("recording/videoDirectory");
const QString recordingGifDirectoryKey = QStringLiteral("recording/gifDirectory");
const QString recordingOutputKey = QStringLiteral("recording/output");
const QString recordingVideoFpsKey = QStringLiteral("recording/videoFps");
const QString recordingResolutionKey = QStringLiteral("recording/resolution");
const QString recordingQualityKey = QStringLiteral("recording/quality");
const QString recordingGifMaxEdgeKey = QStringLiteral("recording/gifMaxEdge");
const QString recordingGifFpsKey = QStringLiteral("recording/gifFps");
const QString recordingGifMaxSecondsKey = QStringLiteral("recording/gifMaxSeconds");
const QString recordingCountdownSecondsKey = QStringLiteral("recording/countdownSeconds");
const QString recordingShowCursorKey = QStringLiteral("recording/showCursor");
const QString recordingSystemAudioKey = QStringLiteral("recording/systemAudio");
const QString recordingMicrophoneKey = QStringLiteral("recording/microphone");
const QString recordingMicrophoneDeviceIdKey = QStringLiteral("recording/microphoneDeviceId");
const QString recordingSystemVolumeKey = QStringLiteral("recording/systemVolumePercent");
const QString recordingMicrophoneVolumeKey = QStringLiteral("recording/microphoneVolumePercent");
bool readRecordingString(const QSettings& settings, const QString& key, QString& value)
{
    const QVariant variant = settings.value(key);
    if (variant.metaType().id() != QMetaType::QString)
        return false;
    value = variant.toString();
    return true;
}
bool readRecordingInt(const QSettings& settings, const QString& key, int& value)
{
    const QVariant variant = settings.value(key);
    const auto type = variant.metaType().id();
    if (type != QMetaType::QString && type != QMetaType::Int && type != QMetaType::UInt &&
        type != QMetaType::LongLong && type != QMetaType::ULongLong)
        return false;
    bool ok = false;
    const int parsed = variant.toString().toInt(&ok);
    if (!ok)
        return false;
    value = parsed;
    return true;
}
bool readRecordingBool(const QSettings& settings, const QString& key, bool& value)
{
    const QVariant variant = settings.value(key);
    if (variant.metaType().id() == QMetaType::Bool)
    {
        value = variant.toBool();
        return true;
    }
    if (variant.metaType().id() != QMetaType::QString)
        return false;
    const QString text = variant.toString();
    if (text == QStringLiteral("true"))
    {
        value = true;
        return true;
    }
    if (text == QStringLiteral("false"))
    {
        value = false;
        return true;
    }
    return false;
}
bool readRecordingChoice(const QSettings& settings, const QString& key,
                         std::initializer_list<int> choices, int& value)
{
    int parsed = 0;
    if (!readRecordingInt(settings, key, parsed))
        return false;
    for (const int choice : choices)
    {
        if (parsed == choice)
        {
            value = parsed;
            return true;
        }
    }
    return false;
}
QString recordingOutputValue(RecordingOutput output)
{
    return output == RecordingOutput::Gif ? QStringLiteral("gif") : QStringLiteral("video");
}
QString recordingResolutionValue(RecordingResolution resolution)
{
    switch (resolution)
    {
    case RecordingResolution::Native:
        return QStringLiteral("native");
    case RecordingResolution::P1080:
        return QStringLiteral("1080p");
    case RecordingResolution::P720:
        return QStringLiteral("720p");
    }
    return {};
}
QString recordingQualityValue(RecordingQuality quality)
{
    switch (quality)
    {
    case RecordingQuality::High:
        return QStringLiteral("high");
    case RecordingQuality::Medium:
        return QStringLiteral("medium");
    case RecordingQuality::Low:
        return QStringLiteral("low");
    }
    return {};
}
QString recordingGifMaxEdgeValue(int edge)
{
    return edge == 0 ? QStringLiteral("native") : QString::number(edge);
}
QString recordingDirectoryError(const QString& directory, const QString& label)
{
    if (directory.isEmpty())
        return {};
    if (directory.contains(QChar::Null) || !QDir::isAbsolutePath(directory))
        return QStringLiteral("%1必须为绝对路径且不能包含空字符。").arg(label);
    return {};
}
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
RecordingPreferences AppSettings::loadRecordingPreferences(QString* notice) const
{
    RecordingPreferences result;
    result.videoDirectory = defaultRecordingVideoDirectory();
    result.gifDirectory = defaultRecordingGifDirectory();
    if (notice)
        notice->clear();
    QFile file(filePath_);
    if (!file.open(QIODevice::ReadOnly))
    {
        if (notice && QFileInfo::exists(filePath_))
            *notice = QStringLiteral("录制设置无法读取，已回退默认设置；原文件未改写。");
        return result;
    }
    file.close();
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.sync();
    if (settings.status() != QSettings::NoError)
    {
        if (notice)
            *notice = QStringLiteral("录制设置已损坏，已回退默认设置；原文件未改写。");
        return result;
    }
    QStringList issues;
    const auto invalid = [&issues](const QString& label) { issues.append(label); };
    const auto readDirectory = [&](const QString& key, const QString& label, QString& target)
    {
        if (!settings.contains(key))
            return;
        QString value;
        if (!readRecordingString(settings, key, value) || value.contains(QChar::Null) ||
            (!value.isEmpty() && !QDir::isAbsolutePath(value)))
        {
            invalid(label);
            return;
        }
        if (!value.isEmpty())
            target = QDir::cleanPath(value);
    };
    readDirectory(recordingVideoDirectoryKey, QStringLiteral("视频目录"), result.videoDirectory);
    readDirectory(recordingGifDirectoryKey, QStringLiteral("GIF 目录"), result.gifDirectory);
    if (settings.contains(recordingOutputKey))
    {
        QString text;
        if (!readRecordingString(settings, recordingOutputKey, text))
            invalid(QStringLiteral("默认输出"));
        else if (text == QStringLiteral("video"))
            result.defaultOutput = RecordingOutput::Video;
        else if (text == QStringLiteral("gif"))
            result.defaultOutput = RecordingOutput::Gif;
        else
            invalid(QStringLiteral("默认输出"));
    }
    if (settings.contains(recordingVideoFpsKey) &&
        !readRecordingChoice(settings, recordingVideoFpsKey, {30, 60}, result.videoFps))
        invalid(QStringLiteral("视频帧率"));
    if (settings.contains(recordingResolutionKey))
    {
        QString text;
        if (!readRecordingString(settings, recordingResolutionKey, text))
            invalid(QStringLiteral("视频分辨率"));
        else if (text == QStringLiteral("native"))
            result.resolution = RecordingResolution::Native;
        else if (text == QStringLiteral("1080p"))
            result.resolution = RecordingResolution::P1080;
        else if (text == QStringLiteral("720p"))
            result.resolution = RecordingResolution::P720;
        else
            invalid(QStringLiteral("视频分辨率"));
    }
    if (settings.contains(recordingQualityKey))
    {
        QString text;
        if (!readRecordingString(settings, recordingQualityKey, text))
            invalid(QStringLiteral("视频质量"));
        else if (text == QStringLiteral("high"))
            result.quality = RecordingQuality::High;
        else if (text == QStringLiteral("medium"))
            result.quality = RecordingQuality::Medium;
        else if (text == QStringLiteral("low"))
            result.quality = RecordingQuality::Low;
        else
            invalid(QStringLiteral("视频质量"));
    }
    if (settings.contains(recordingGifMaxEdgeKey))
    {
        QString text;
        if (!readRecordingString(settings, recordingGifMaxEdgeKey, text))
            invalid(QStringLiteral("GIF 最长边"));
        else if (text == QStringLiteral("native"))
            result.gifMaxEdge = 0;
        else
        {
            bool ok = false;
            const int edge = text.toInt(&ok);
            if (!ok || (edge != 960 && edge != 1280))
                invalid(QStringLiteral("GIF 最长边"));
            else
                result.gifMaxEdge = edge;
        }
    }
    if (settings.contains(recordingGifFpsKey) &&
        !readRecordingChoice(settings, recordingGifFpsKey, {10, 12, 15}, result.gifFps))
        invalid(QStringLiteral("GIF 帧率"));
    if (settings.contains(recordingGifMaxSecondsKey) &&
        !readRecordingChoice(settings, recordingGifMaxSecondsKey, {15, 30, 60},
                             result.gifMaxSeconds))
        invalid(QStringLiteral("GIF 最长时长"));
    if (settings.contains(recordingCountdownSecondsKey) &&
        !readRecordingChoice(settings, recordingCountdownSecondsKey, {0, 3, 5},
                             result.countdownSeconds))
        invalid(QStringLiteral("倒计时"));
    if (settings.contains(recordingShowCursorKey) &&
        !readRecordingBool(settings, recordingShowCursorKey, result.showCursor))
        invalid(QStringLiteral("显示光标"));
    if (settings.contains(recordingSystemAudioKey) &&
        !readRecordingBool(settings, recordingSystemAudioKey, result.systemAudio))
        invalid(QStringLiteral("系统声音"));
    if (settings.contains(recordingMicrophoneKey) &&
        !readRecordingBool(settings, recordingMicrophoneKey, result.microphone))
        invalid(QStringLiteral("麦克风"));
    if (settings.contains(recordingMicrophoneDeviceIdKey))
    {
        QString id;
        if (!readRecordingString(settings, recordingMicrophoneDeviceIdKey, id) ||
            id.contains(QChar::Null))
            invalid(QStringLiteral("麦克风设备"));
        else
            result.microphoneDeviceId = id;
    }
    const auto readVolume = [&](const QString& key, const QString& label, int& target)
    {
        if (!settings.contains(key))
            return;
        int value = 0;
        if (!readRecordingInt(settings, key, value))
        {
            invalid(label);
            return;
        }
        if (value < 0 || value > 200)
        {
            target = qBound(0, value, 200);
            invalid(QStringLiteral("%1已修正").arg(label));
            return;
        }
        target = value;
    };
    readVolume(recordingSystemVolumeKey, QStringLiteral("系统声音音量"),
               result.systemVolumePercent);
    readVolume(recordingMicrophoneVolumeKey, QStringLiteral("麦克风音量"),
               result.microphoneVolumePercent);
    if (notice && !issues.isEmpty())
        *notice = QStringLiteral("录制设置部分内容无效，已回退默认值或修正；原文件未改写。%1")
                      .arg(issues.join(QStringLiteral("、")));
    return result;
}
QString AppSettings::saveRecordingPreferences(const RecordingPreferences& preferences) const
{
    QString error =
        recordingDirectoryError(preferences.videoDirectory, QStringLiteral("录制视频目录"));
    if (!error.isEmpty())
        return error;
    error = recordingDirectoryError(preferences.gifDirectory, QStringLiteral("录制 GIF 目录"));
    if (!error.isEmpty())
        return error;
    switch (preferences.defaultOutput)
    {
    case RecordingOutput::Video:
    case RecordingOutput::Gif:
        break;
    default:
        return QStringLiteral("默认输出无效，只支持视频或 GIF。");
    }
    if (preferences.videoFps != 30 && preferences.videoFps != 60)
        return QStringLiteral("视频帧率无效，只支持 30 或 60。");
    switch (preferences.resolution)
    {
    case RecordingResolution::Native:
    case RecordingResolution::P1080:
    case RecordingResolution::P720:
        break;
    default:
        return QStringLiteral("视频分辨率无效，只支持原尺寸、1080p 或 720p。");
    }
    switch (preferences.quality)
    {
    case RecordingQuality::High:
    case RecordingQuality::Medium:
    case RecordingQuality::Low:
        break;
    default:
        return QStringLiteral("视频质量无效，只支持高、中、低。");
    }
    if (preferences.gifMaxEdge != 0 && preferences.gifMaxEdge != 960 &&
        preferences.gifMaxEdge != 1280)
        return QStringLiteral("GIF 最长边无效，只支持 960、1280 或原尺寸。");
    if (preferences.gifFps != 10 && preferences.gifFps != 12 && preferences.gifFps != 15)
        return QStringLiteral("GIF 帧率无效，只支持 10、12 或 15。");
    if (preferences.gifMaxSeconds != 15 && preferences.gifMaxSeconds != 30 &&
        preferences.gifMaxSeconds != 60)
        return QStringLiteral("GIF 最长时长无效，只支持 15、30 或 60 秒。");
    if (preferences.countdownSeconds != 0 && preferences.countdownSeconds != 3 &&
        preferences.countdownSeconds != 5)
        return QStringLiteral("倒计时无效，只支持 0、3 或 5 秒。");
    if (preferences.systemVolumePercent < 0 || preferences.systemVolumePercent > 200 ||
        preferences.microphoneVolumePercent < 0 || preferences.microphoneVolumePercent > 200)
        return QStringLiteral("音量必须在 0–200% 之间。");
    if (preferences.microphoneDeviceId.contains(QChar::Null))
        return QStringLiteral("麦克风设备标识不能包含空字符。");
    const auto prepareDirectory = [](const QString& directory, const QString& label) -> QString
    {
        if (directory.isEmpty())
            return {};
        const QString path = QDir::cleanPath(directory);
        if (!QDir().mkpath(path))
            return QStringLiteral("%1创建失败：%2").arg(label, path);
        const QFileInfo info(path);
        if (!info.isDir())
            return QStringLiteral("%1不是目录：%2").arg(label, path);
        if (!info.isWritable())
            return QStringLiteral("%1不可写：%2").arg(label, path);
        return {};
    };
    error = prepareDirectory(preferences.videoDirectory, QStringLiteral("录制视频目录"));
    if (!error.isEmpty())
        return error;
    error = prepareDirectory(preferences.gifDirectory, QStringLiteral("录制 GIF 目录"));
    if (!error.isEmpty())
        return error;
    const QVector<QPair<QString, QVariant>> entries = {
        {recordingVideoDirectoryKey, preferences.videoDirectory.isEmpty()
                                         ? QString()
                                         : QDir::cleanPath(preferences.videoDirectory)},
        {recordingGifDirectoryKey, preferences.gifDirectory.isEmpty()
                                       ? QString()
                                       : QDir::cleanPath(preferences.gifDirectory)},
        {recordingOutputKey, recordingOutputValue(preferences.defaultOutput)},
        {recordingVideoFpsKey, preferences.videoFps},
        {recordingResolutionKey, recordingResolutionValue(preferences.resolution)},
        {recordingQualityKey, recordingQualityValue(preferences.quality)},
        {recordingGifMaxEdgeKey, recordingGifMaxEdgeValue(preferences.gifMaxEdge)},
        {recordingGifFpsKey, preferences.gifFps},
        {recordingGifMaxSecondsKey, preferences.gifMaxSeconds},
        {recordingCountdownSecondsKey, preferences.countdownSeconds},
        {recordingShowCursorKey, preferences.showCursor},
        {recordingSystemAudioKey, preferences.systemAudio},
        {recordingMicrophoneKey, preferences.microphone},
        {recordingMicrophoneDeviceIdKey, preferences.microphoneDeviceId},
        {recordingSystemVolumeKey, preferences.systemVolumePercent},
        {recordingMicrophoneVolumeKey, preferences.microphoneVolumePercent}};
    const QString writeError = QStringLiteral("录制设置写入失败，原设置已保留，请重试保存。");
    const QString parent = QFileInfo(filePath_).absolutePath();
    if (!QDir().mkpath(parent) || QFileInfo(filePath_).isDir())
        return writeError;
    QSettings settings(filePath_, QSettings::IniFormat);
    settings.setAtomicSyncRequired(true);
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return writeError;
    QVector<QPair<QString, QVariant>> previous;
    previous.reserve(entries.size());
    for (const auto& entry : entries)
        previous.append({entry.first, settings.value(entry.first)});
    for (const auto& entry : entries)
        settings.setValue(entry.first, entry.second);
    settings.sync();
    if (settings.status() != QSettings::NoError)
    {
        // 原子同步失败时恢复共享缓存，避免后续保存夹带半套录制值。
        for (const auto& entry : previous)
        {
            if (entry.second.isValid())
                settings.setValue(entry.first, entry.second);
            else
                settings.remove(entry.first);
        }
        return writeError;
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
QString AppSettings::quickGunnerStarsPath() const
{
    // 默认配置位于 AppLocalDataLocation；测试自定义配置时，元数据同步隔离。
    return QDir(QFileInfo(filePath_).absolutePath())
        .filePath(QStringLiteral("quick-gunner-stars.json"));
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
    const QString source = settings.value(prefix + QStringLiteral("lastSource")).toString();
    if (source == QStringLiteral("history") || source == QStringLiteral("quick-gunner"))
        result.lastSource = source;
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
    const QStringList names{QStringLiteral("geometry"),  QStringLiteral("splitterState"),
                            QStringLiteral("lastView"),  QStringLiteral("lastSelection"),
                            QStringLiteral("sortOrder"), QStringLiteral("lastSource")};
    const QVector<QVariant> values{state.geometry,      state.splitterState, state.lastView,
                                   state.lastSelection, state.sortOrder,     state.lastSource};
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
    return enabled() ? QStringLiteral("WaibuSnap · %1键 %2")
                           .arg(hotkeyActionName(action_),
                                enabledSequence_.toString(QKeySequence::NativeText))
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
