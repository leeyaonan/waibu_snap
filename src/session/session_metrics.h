#pragma once
#include <QJsonObject>
#include <QString>
namespace waibusnap
{
struct SessionMetrics
{
    int sequence = 0;
    QString trigger;
    qint64 t0 = 0;
    qint64 captureStart = 0;
    qint64 captureComplete = 0;
    qint64 windowCreated = 0;
    qint64 paintComplete = 0;
    qint64 visibleProxy = 0;
    qint64 interactive = 0;
    double refreshIntervalMs = 0;
    int width = 0;
    int height = 0;
    int frameWidth = 0;
    int frameHeight = 0;
    int outcome = 0;
    QJsonObject toJson() const;
};
bool appendMetrics(const QString& path, const SessionMetrics& metrics);
}
