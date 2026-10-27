#pragma once
#include "interfaces/display_topology.h"
#include <QJsonObject>
#include <optional>
namespace waibusnap
{
// 返回显示器内的物理像素范围；无效或超出整数范围时返回空尺寸。
QSize quickGunnerPixelSize(const DisplayTarget& display);
struct QuickGunnerRegion
{
    DisplayTarget display;
    QRect pixelRect;
    bool isValid() const;
    QJsonObject toJson() const;
    static std::optional<QuickGunnerRegion> fromJson(const QJsonObject& json,
                                                     QString* reason = nullptr);
};
struct QuickGunnerPreferences
{
    QString directory;
    std::optional<QuickGunnerRegion> region;
};
}
