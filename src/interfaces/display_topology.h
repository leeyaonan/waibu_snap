#pragma once
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>
#include <memory>
namespace waibusnap
{
struct DisplayTarget
{
    quint32 id = 0;
    QRect logicalGeometry;
    qreal devicePixelRatio = 1.0;
    quint64 layoutVersion = 0;
    // 捕获时刻的系统可用区；空值回退为 logicalGeometry。
    QRect availableLogicalGeometry;
};
struct DisplayResult
{
    DisplayTarget target;
    QString error;
};
class DisplayTopology
{
  public:
    virtual ~DisplayTopology() = default;
    virtual DisplayResult cursorDisplay() const = 0;
    virtual bool stillMatches(const DisplayTarget& target) const = 0;
    // 全部启用显示器的一致性快照（一次调用内读取）。默认空实现表示平台不支持，
    // 既有测试桩无需覆写即可编译；Windows 不覆写，入口据此置灰。
    virtual QVector<DisplayResult> allDisplays() const { return {}; }
};
std::unique_ptr<DisplayTopology> createDisplayTopology();
}
