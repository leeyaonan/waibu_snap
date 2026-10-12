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
