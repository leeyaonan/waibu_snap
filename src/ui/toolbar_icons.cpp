#include "ui/toolbar_icons.h"
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <cmath>
namespace waibusnap
{
namespace
{
constexpr qreal pi = 3.14159265358979323846;
static void arrowHead(QPainter& p, QPointF tip, qreal angleRad, qreal len)
{
    for (int k : {-1, 1})
    {
        const qreal a = angleRad + pi + k * (pi / 6.0);
        p.drawLine(tip, tip + QPointF(std::cos(a), std::sin(a)) * len);
    }
}

// 图标绘制：18×18 逻辑坐标系内作画。
static void drawIcon(QPainter& p, int id, const QRectF& box, const QColor& fg, const QColor& bg)
{
    p.save();
    p.translate(box.topLeft());
    QPen pen(fg, 1.5);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    switch (id)
    {
    case 0: // 矩形
        p.drawRoundedRect(QRectF(2.5, 4, 13, 10), 2.5, 2.5);
        break;
    case 1: // 椭圆
        p.drawEllipse(QRectF(2.5, 4, 13, 10));
        break;
    case 2: // 直线
        p.drawLine(QPointF(3.5, 14.5), QPointF(14.5, 3.5));
        break;
    case 3: // 箭头
    {
        const QPointF a(3.5, 14.5), b(14, 4);
        p.drawLine(a, b);
        arrowHead(p, b, std::atan2(b.y() - a.y(), b.x() - a.x()), 4.6);
        break;
    }
    case 4: // 画笔（自由曲线）
    {
        QPainterPath path;
        path.moveTo(2.5, 13.5);
        path.cubicTo(5.5, 6.5, 7.5, 15.5, 10, 9);
        path.cubicTo(11.5, 4.5, 13.5, 7, 15.5, 3.5);
        p.drawPath(path);
        break;
    }
    case 5: // 文本 T
        p.drawLine(QPointF(3.5, 4.5), QPointF(14.5, 4.5));
        p.drawLine(QPointF(9, 4.5), QPointF(9, 15));
        break;
    case 6: // 遮盖（实心块）
        p.setPen(Qt::NoPen);
        p.setBrush(fg);
        p.drawRoundedRect(QRectF(2.5, 4, 13, 10), 1.5, 1.5);
        break;
    case 7: // 马赛克（棋盘格）
    {
        QPainterPath clip;
        clip.addRoundedRect(QRectF(2.5, 4, 13, 10), 1.5, 1.5);
        p.save();
        p.setClipPath(clip);
        p.setPen(Qt::NoPen);
        p.setBrush(fg);
        p.fillRect(QRectF(2.5, 4, 6.5, 5), fg);
        p.fillRect(QRectF(9, 9, 6.5, 5), fg);
        p.restore();
        p.setBrush(Qt::NoBrush);
        p.setPen(pen);
        p.drawRoundedRect(QRectF(2.5, 4, 13, 10), 1.5, 1.5);
        break;
    }
    case 8: // 取字（取景角 + 文）
    {
        p.drawPolyline(QPolygonF({QPointF(3, 5.5), QPointF(3, 3), QPointF(5.5, 3)}));
        p.drawPolyline(QPolygonF({QPointF(12.5, 3), QPointF(15, 3), QPointF(15, 5.5)}));
        p.drawPolyline(QPolygonF({QPointF(15, 12.5), QPointF(15, 15), QPointF(12.5, 15)}));
        p.drawPolyline(QPolygonF({QPointF(5.5, 15), QPointF(3, 15), QPointF(3, 12.5)}));
        QFont f = p.font();
        f.setPixelSize(9);
        p.setFont(f);
        p.drawText(QRectF(3, 4.5, 12, 9.5), Qt::AlignCenter, QString::fromUtf8("文"));
        break;
    }
    case 12: // 撤销（弧 + 左箭头）
        p.drawArc(QRectF(4, 5.5, 10, 10), 180 * 16, -180 * 16);
        arrowHead(p, QPointF(4, 10.5), pi, 4.4);
        break;
    case 13: // 重做（镜像撤销）
        p.save();
        p.translate(18, 0);
        p.scale(-1, 1);
        drawIcon(p, 12, QRectF(0, 0, 18, 18), fg, bg);
        p.restore();
        break;
    case 14: // 复制（两张纸）
        p.drawRoundedRect(QRectF(3, 2.5, 8.5, 10), 2, 2);
        p.setBrush(bg);
        p.drawRoundedRect(QRectF(6.5, 5.5, 8.5, 10), 2, 2);
        break;
    case 15: // 保存（向下入盒）
        p.drawPolyline(QPolygonF(
            {QPointF(2.5, 11.5), QPointF(2.5, 15), QPointF(15.5, 15), QPointF(15.5, 11.5)}));
        p.drawLine(QPointF(9, 2.5), QPointF(9, 10));
        arrowHead(p, QPointF(9, 10), pi / 2, 4.2);
        break;
    case 16: // 钉到屏幕（图钉）
        p.setPen(Qt::NoPen);
        p.setBrush(fg);
        p.drawRoundedRect(QRectF(4.5, 3, 9, 3.2), 1.6, 1.6);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(9, 6.2), QPointF(9, 10.6));
        p.drawLine(QPointF(5, 10.6), QPointF(13, 10.6));
        p.drawLine(QPointF(9, 10.6), QPointF(9, 15));
        break;
    case 17: // 取消 X
        p.drawLine(QPointF(4.5, 4.5), QPointF(13.5, 13.5));
        p.drawLine(QPointF(13.5, 4.5), QPointF(4.5, 13.5));
        break;
    case 18: // 提取全文（文档 + 导出箭头）
        p.drawRoundedRect(QRectF(2.5, 3, 9.5, 12), 1.8, 1.8);
        p.drawLine(QPointF(4.8, 6.8), QPointF(9.7, 6.8));
        p.drawLine(QPointF(4.8, 9.3), QPointF(9.7, 9.3));
        {
            const QPointF a(9.5, 14.5), b(15.5, 8.5);
            p.drawLine(a, b);
            arrowHead(p, b, std::atan2(b.y() - a.y(), b.x() - a.x()), 3.6);
        }
        break;
    case 19: // 比例（圆角矩形 + 水平双向箭头）
        p.drawRoundedRect(QRectF(2.5, 4.5, 13, 9), 2, 2);
        p.drawLine(QPointF(6.2, 9), QPointF(11.8, 9));
        arrowHead(p, QPointF(5.5, 9), pi, 3.2);
        arrowHead(p, QPointF(12.5, 9), 0.0, 3.2);
        break;
    case 21: // 长图（长页 + 双向滚动箭头）
        p.drawRoundedRect(QRectF(2, 2, 8, 14), 1, 1);
        p.drawLine(QPointF(4, 6), QPointF(8, 6));
        p.drawLine(QPointF(4, 10), QPointF(8, 10));
        p.drawLine(QPointF(14, 3), QPointF(14, 15));
        arrowHead(p, QPointF(14, 3), -pi / 2, 3);
        arrowHead(p, QPointF(14, 15), pi / 2, 3);
        break;
    case 20: // 形状（五角星描边）
    {
        QPainterPath star;
        const QPointF c(9, 9.4);
        for (int i = 0; i < 10; ++i)
        {
            const qreal r = (i % 2 == 0) ? 6.4 : 2.9;
            const qreal a = -pi / 2 + i * pi / 5;
            const QPointF pt = c + QPointF(std::cos(a), std::sin(a)) * r;
            i == 0 ? star.moveTo(pt) : star.lineTo(pt);
        }
        star.closeSubpath();
        p.drawPath(star);
        break;
    }
    case 22: // 选择（箭头 + 选中框）
    {
        QPainterPath cursor;
        cursor.moveTo(3.5, 2.5);
        cursor.lineTo(3.5, 14);
        cursor.lineTo(7, 10.8);
        cursor.lineTo(9.3, 15.2);
        cursor.lineTo(11.2, 14.2);
        cursor.lineTo(8.9, 10);
        cursor.lineTo(13.5, 9.4);
        cursor.closeSubpath();
        p.setPen(QPen(fg, 1.2));
        p.setBrush(bg);
        p.drawPath(cursor);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(fg, 1.1, Qt::DashLine));
        p.drawRect(QRectF(11.5, 2.5, 4, 4));
        break;
    }
    }
    p.restore();
}

// 描画一枚方形按钮（含状态底色），返回按钮矩形。

void drawOption(QPainter& painter, ToolbarIcon icon, QColor color, qreal value, QColor foreground,
                bool checked)
{
    painter.save();
    painter.translate(9, 9);
    QPen pen(foreground, 1.5);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    if (icon == ToolbarIcon::Color)
    {
        painter.setPen(QPen(checked ? QColor(Qt::white) : QColor("#8a8a8a"), 1.2));
        painter.setBrush(color);
        painter.drawEllipse(QPointF(0, 0), 5.6, 5.6);
    }
    else if (icon == ToolbarIcon::LineWidth)
    {
        pen.setWidthF(value);
        painter.setPen(pen);
        painter.drawLine(QPointF(-6, 0), QPointF(6, 0));
    }
    else
    {
        painter.scale(value, value);
        QPainterPath path;
        path.moveTo(-4.5, 6.5);
        path.lineTo(0, -6.5);
        path.lineTo(4.5, 6.5);
        path.moveTo(-2.6, 2.2);
        path.lineTo(2.6, 2.2);
        painter.drawPath(path);
    }
    painter.restore();
}
}
int annotationToolSlot(AnnotationType type)
{
    switch (type)
    {
    case AnnotationType::Rectangle:
    case AnnotationType::Ellipse:
        return 0;
    case AnnotationType::Line:
    case AnnotationType::Arrow:
        return 1;
    case AnnotationType::Freehand:
        return 2;
    case AnnotationType::Text:
        return 3;
    case AnnotationType::Cover:
    case AnnotationType::Mosaic:
        return 4;
    }
    return 0;
}
QString annotationToolName(AnnotationType type)
{
    const QString names[] = {QStringLiteral("矩形"), QStringLiteral("椭圆"),
                             QStringLiteral("直线"), QStringLiteral("箭头"),
                             QStringLiteral("画笔"), QStringLiteral("文本"),
                             QStringLiteral("遮盖"), QStringLiteral("马赛克")};
    return names[static_cast<int>(type)];
}
QString annotationToolObject(AnnotationType type)
{
    const QString names[] = {
        QStringLiteral("rectangleToolButton"), QStringLiteral("ellipseToolButton"),
        QStringLiteral("lineToolButton"),      QStringLiteral("arrowToolButton"),
        QStringLiteral("freehandToolButton"),  QStringLiteral("textToolButton"),
        QStringLiteral("coverToolButton"),     QStringLiteral("mosaicToolButton")};
    return names[static_cast<int>(type)];
}
QString annotationToolTip(AnnotationType type)
{
    return type == AnnotationType::Mosaic
               ? QStringLiteral("马赛克可能被还原，高敏感内容请用实心遮盖")
               : annotationToolName(type) + QStringLiteral("：点击启用，再次点击取消工具");
}
QString annotationGroupObject(int slot)
{
    const QString names[] = {QStringLiteral("geometryToolButton"),
                             QStringLiteral("directionToolButton"),
                             QStringLiteral("freehandToolButton"), QStringLiteral("textToolButton"),
                             QStringLiteral("concealToolButton")};
    return names[slot];
}
QIcon toolbarIcon(ToolbarIcon icon, QColor color, qreal value)
{
    QIcon result;
    for (QIcon::Mode mode : {QIcon::Normal, QIcon::Disabled})
        for (QIcon::State state : {QIcon::Off, QIcon::On})
            for (int scale : {1, 2})
            {
                QPixmap pixmap(18 * scale, 18 * scale);
                pixmap.setDevicePixelRatio(scale);
                pixmap.fill(Qt::transparent);
                QPainter painter(&pixmap);
                painter.setRenderHint(QPainter::Antialiasing);
                const bool checked = state == QIcon::On;
                const QColor foreground = mode == QIcon::Disabled ? QColor("#6e6e6e")
                                          : checked               ? QColor(Qt::white)
                                                                  : QColor("#f2f2f2");
                if (icon >= ToolbarIcon::Color && icon <= ToolbarIcon::TextSize)
                    drawOption(painter, icon, mode == QIcon::Disabled ? foreground : color, value,
                               foreground, checked);
                else
                    drawIcon(painter, static_cast<int>(icon), QRectF(0, 0, 18, 18), foreground,
                             checked ? QColor("#a65316") : QColor("#202020"));
                painter.end();
                result.addPixmap(pixmap, mode, state);
            }
    return result;
}
}
