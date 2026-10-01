#include "session/session_metrics.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
namespace waibusnap
{
QJsonObject SessionMetrics::toJson() const
{
    QJsonObject result{{"schema", 1},
                       {"sequence", sequence},
                       {"cold", sequence == 1},
                       {"trigger", trigger},
                       {"outcome", outcome},
                       {"selection_width_px", width},
                       {"selection_height_px", height},
                       {"frame_width_px", frameWidth},
                       {"frame_height_px", frameHeight},
                       {"presentation_method", "display_link_proxy"},
                       {"refresh_interval_ms", refreshIntervalMs}};
    const auto stamp = [&result](const char* name, qint64 value)
    {
        if (value)
            result.insert(QString::fromLatin1(name), QString::number(value));
    };
    stamp("t0_ns", t0);
    stamp("capture_start_ns", captureStart);
    stamp("capture_complete_ns", captureComplete);
    stamp("window_created_ns", windowCreated);
    stamp("paint_complete_ns", paintComplete);
    stamp("visible_proxy_ns", visibleProxy);
    stamp("interactive_ns", interactive);
    const auto duration = [&result](const char* name, qint64 start, qint64 end)
    {
        if (start && end >= start)
            result.insert(QString::fromLatin1(name), double(end - start) / 1e6);
    };
    duration("pre_capture_ms", t0, captureStart);
    duration("capture_ms", captureStart, captureComplete);
    duration("window_create_ms", captureComplete, windowCreated);
    duration("paint_ms", windowCreated, paintComplete);
    duration("presentation_wait_ms", paintComplete, visibleProxy);
    duration("nf01_proxy_ms", t0, interactive);
    return result;
}
bool appendMetrics(const QString& path, const SessionMetrics& metrics)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append))
        return false;
    const QByteArray line = QJsonDocument(metrics.toJson()).toJson(QJsonDocument::Compact) + '\n';
    return file.write(line) == line.size() && file.flush();
}
}
