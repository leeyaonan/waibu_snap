#include "core/quick_gunner.h"
#include <cmath>
#include <limits>
namespace waibusnap
{
namespace
{
bool integer(const QJsonValue& value, qint64 minimum, qint64 maximum, qint64& result)
{
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < double(minimum) || number > double(maximum) ||
        std::floor(number) != number)
        return false;
    result = qint64(number);
    return true;
}
QJsonObject rectJson(QRect rect)
{
    return {{QStringLiteral("x"), rect.x()},
            {QStringLiteral("y"), rect.y()},
            {QStringLiteral("width"), rect.width()},
            {QStringLiteral("height"), rect.height()}};
}
bool readRect(const QJsonValue& value, QRect& rect)
{
    if (!value.isObject())
        return false;
    const auto object = value.toObject();
    qint64 x, y, width, height;
    constexpr auto minimum = std::numeric_limits<int>::min();
    constexpr auto maximum = std::numeric_limits<int>::max();
    if (!integer(object.value(QStringLiteral("x")), minimum, maximum, x) ||
        !integer(object.value(QStringLiteral("y")), minimum, maximum, y) ||
        !integer(object.value(QStringLiteral("width")), 0, maximum, width) ||
        !integer(object.value(QStringLiteral("height")), 0, maximum, height) ||
        x + width > maximum || y + height > maximum)
        return false;
    rect = QRect(int(x), int(y), int(width), int(height));
    return true;
}
}
QSize quickGunnerPixelSize(const DisplayTarget& display)
{
    const qreal dpr = display.devicePixelRatio;
    if (display.id == 0 || display.logicalGeometry.isEmpty() || !std::isfinite(dpr) || dpr <= 0)
        return {};
    const qreal width = display.logicalGeometry.width() * dpr;
    const qreal height = display.logicalGeometry.height() * dpr;
    const auto maximum = std::numeric_limits<int>::max();
    if (!std::isfinite(width) || !std::isfinite(height) || width > maximum || height > maximum)
        return {};
    return {qRound(width), qRound(height)};
}
bool QuickGunnerRegion::isValid() const
{
    const QSize size = quickGunnerPixelSize(display);
    const auto available = display.availableLogicalGeometry;
    return !size.isEmpty() &&
           (available == QRect() ||
            (!available.isEmpty() && display.logicalGeometry.contains(available))) &&
           pixelRect.width() >= 16 && pixelRect.height() >= 16 && pixelRect.x() >= 0 &&
           pixelRect.y() >= 0 && qint64(pixelRect.x()) + pixelRect.width() <= size.width() &&
           qint64(pixelRect.y()) + pixelRect.height() <= size.height();
}
QJsonObject QuickGunnerRegion::toJson() const
{
    return {
        {QStringLiteral("displayId"), qint64(display.id)},
        {QStringLiteral("logicalGeometry"), rectJson(display.logicalGeometry)},
        {QStringLiteral("devicePixelRatio"), display.devicePixelRatio},
        // 布局指纹是完整 uint64，十进制字符串避免 JSON 浮点数丢失精度。
        {QStringLiteral("layoutVersion"), QString::number(display.layoutVersion)},
        {QStringLiteral("availableLogicalGeometry"), rectJson(display.availableLogicalGeometry)},
        {QStringLiteral("pixelRect"), rectJson(pixelRect)}};
}
std::optional<QuickGunnerRegion> QuickGunnerRegion::fromJson(const QJsonObject& json,
                                                             QString* reason)
{
    if (reason)
        reason->clear();
    const auto fail = [reason](const QString& field) -> std::optional<QuickGunnerRegion>
    {
        if (reason)
            *reason = QStringLiteral("快枪手机位字段 %1 缺失或无效。").arg(field);
        return std::nullopt;
    };
    QuickGunnerRegion result;
    qint64 id = 0;
    if (!integer(json.value(QStringLiteral("displayId")), 1, std::numeric_limits<quint32>::max(),
                 id))
        return fail(QStringLiteral("displayId"));
    result.display.id = quint32(id);
    if (!readRect(json.value(QStringLiteral("logicalGeometry")), result.display.logicalGeometry) ||
        result.display.logicalGeometry.isEmpty())
        return fail(QStringLiteral("logicalGeometry"));
    const auto ratio = json.value(QStringLiteral("devicePixelRatio"));
    if (!ratio.isDouble() || !std::isfinite(ratio.toDouble()) || ratio.toDouble() <= 0)
        return fail(QStringLiteral("devicePixelRatio"));
    result.display.devicePixelRatio = ratio.toDouble();
    const auto version = json.value(QStringLiteral("layoutVersion"));
    if (version.isString())
    {
        bool ok = false;
        result.display.layoutVersion = version.toString().toULongLong(&ok);
        if (!ok || QString::number(result.display.layoutVersion) != version.toString())
            return fail(QStringLiteral("layoutVersion"));
    }
    else
    {
        qint64 number = 0;
        if (!integer(version, 0, 9007199254740991LL, number))
            return fail(QStringLiteral("layoutVersion"));
        result.display.layoutVersion = quint64(number);
    }
    if (!readRect(json.value(QStringLiteral("availableLogicalGeometry")),
                  result.display.availableLogicalGeometry) ||
        (result.display.availableLogicalGeometry != QRect() &&
         (result.display.availableLogicalGeometry.isEmpty() ||
          !result.display.logicalGeometry.contains(result.display.availableLogicalGeometry))))
        return fail(QStringLiteral("availableLogicalGeometry"));
    if (!readRect(json.value(QStringLiteral("pixelRect")), result.pixelRect) || !result.isValid())
        return fail(QStringLiteral("pixelRect"));
    return result;
}
}
