#include "interfaces/recording.h"
#include <QDir>
#include <QFileInfo>
#include <cmath>
namespace waibusnap
{
namespace
{
QString validateTarget(const RecordingRequest& request)
{
    if (request.targetKind == RecordingTargetKind::Window)
    {
        if (request.windowId == 0)
            return QStringLiteral("窗口录制缺少有效的窗口 ID。");
        return {};
    }
    if (request.display.id == 0)
        return QStringLiteral("录制目标显示器无效，请重新选择。");
    const double scale =
        request.display.devicePixelRatio > 0 ? request.display.devicePixelRatio : 1.0;
    const QSize displayPixels(qRound(request.display.logicalGeometry.width() * scale),
                              qRound(request.display.logicalGeometry.height() * scale));
    if (request.targetKind == RecordingTargetKind::Display)
        return {};
    const QRect& rectangle = request.sourcePixelRect;
    if (rectangle.width() <= 0 || rectangle.height() <= 0)
        return QStringLiteral("区域录制缺少有效的选区尺寸。");
    if (!QRect(QPoint(0, 0), displayPixels).contains(rectangle))
        return QStringLiteral("录制区域超出目标显示器范围，请重新框选。");
    return {};
}

} // namespace

QString validateRecordingRequest(const RecordingRequest& request)
{
    const bool gif = request.output == RecordingOutput::Gif;
    if (!gif && request.output != RecordingOutput::Video)
        return QStringLiteral("录制输出类型无效。");
    if (gif &&
        (request.systemAudio || request.microphoneEnabled || !request.microphoneDeviceId.isEmpty()))
        return QStringLiteral("GIF不支持系统声音或麦克风。");
    if (gif && request.maxDurationMs <= 0)
        return QStringLiteral("GIF最长录制时长必须为正数。");
    if (gif && !request.finalPath.endsWith(QStringLiteral(".gif"), Qt::CaseInsensitive))
        return QStringLiteral("GIF成品必须以.gif结尾。");
    for (QChar character : request.microphoneDeviceId)
        if (character.isNull() || character.category() == QChar::Other_Control)
            return QStringLiteral("麦克风设备 ID 含非法字符。");
    if (!std::isfinite(request.systemGain) || request.systemGain < 0 || request.systemGain > 2 ||
        !std::isfinite(request.microphoneGain) || request.microphoneGain < 0 ||
        request.microphoneGain > 2)
        return QStringLiteral("音频增益必须在 0–2.0 之间。");
    if (request.outputPixels.width() <= 0 || request.outputPixels.height() <= 0)
        return QStringLiteral("输出尺寸必须为正数。");
    if (!gif && (request.outputPixels.width() % 2 != 0 || request.outputPixels.height() % 2 != 0))
        return QStringLiteral("视频输出宽高必须为偶数。");
    if (gif && request.fps != 10 && request.fps != 12 && request.fps != 15)
        return QStringLiteral("GIF帧率只支持10 / 12 / 15。");
    if (gif && (request.outputPixels.width() > 65535 || request.outputPixels.height() > 65535))
        return QStringLiteral("GIF尺寸不能超过65535。");
    if (!gif && request.fps != 30 && request.fps != 60)
        return QStringLiteral("视频帧率只支持 30 或 60。");
    if (request.maxDurationMs < 0)
        return QStringLiteral("最长录制时长不能为负数。");
    if (request.tempPath.isEmpty() || request.finalPath.isEmpty())
        return QStringLiteral("录制临时路径与成品路径不能为空。");
    if (!QDir::isAbsolutePath(request.tempPath) || !QDir::isAbsolutePath(request.finalPath))
        return QStringLiteral("录制路径必须是绝对路径。");
    if (QDir::cleanPath(request.tempPath) == QDir::cleanPath(request.finalPath))
        return QStringLiteral("录制临时路径不能与成品路径相同。");
    if (!request.tempPath.endsWith(QStringLiteral(".part"), Qt::CaseInsensitive))
        return QStringLiteral("录制临时文件必须以 .part 结尾。");
    if (QFileInfo::exists(request.tempPath))
        return QStringLiteral("录制临时文件已存在，请重新选择路径。");
    if (QFileInfo::exists(request.finalPath))
        return QStringLiteral("成品路径已存在，请重新选择文件名。");
    const QFileInfo temporary(request.tempPath);
    const QFileInfo finalInfo(request.finalPath);
    if (!temporary.absoluteDir().exists() || !finalInfo.absoluteDir().exists())
        return QStringLiteral("录制目录不存在，请先在设置中重新选择目录。");
    if (!QFileInfo(temporary.absolutePath()).isWritable() ||
        !QFileInfo(finalInfo.absolutePath()).isWritable())
        return QStringLiteral("录制目录不可写，请检查目录权限。");
    return validateTarget(request);
}

} // namespace waibusnap
