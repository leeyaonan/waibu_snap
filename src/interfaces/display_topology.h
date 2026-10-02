#pragma once
#include <QRect>
#include <QSize>
#include <QString>
#include <memory>
namespace waibusnap
{
struct DisplayTarget
{
    quint32 id = 0;
    QRect logicalGeometry;
    qreal devicePixelRatio = 1.0;
    quint64 layoutVersion = 0;
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
};
std::unique_ptr<DisplayTopology> createDisplayTopology();
}
