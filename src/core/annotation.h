#pragma once
#include <QColor>
#include <QFont>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QString>
#include <QVector>
#include <array>
namespace waibusnap
{
enum class AnnotationType
{
    Rectangle,
    Ellipse,
    Line,
    Arrow,
    Freehand,
    Text,
    Cover,
    Mosaic
};
inline constexpr int annotationColorCount = 9;
const std::array<QColor, annotationColorCount>& annotationColors();
const std::array<QString, annotationColorCount>& annotationColorNames();
inline constexpr int annotationDefaultWidth = 4;
inline constexpr int annotationMinimumWidth = 1;
inline constexpr int annotationMaximumWidth = 24;
inline constexpr int annotationMinimumTextSize = 9;
inline constexpr int annotationMaximumTextSize = 96;
bool annotationUsesLineWidth(AnnotationType type);
// 两编辑面共用高分辨率滚轮累积；角度优先，只有像素输入时每 15px 一格。
// 调用方传入不同上下限：线宽默认 1–24，文本字号使用 9–96。
class AnnotationWidthWheel
{
  public:
    int adjust(int value, QPoint angleDelta, QPoint pixelDelta = {},
               int minimum = annotationMinimumWidth, int maximum = annotationMaximumWidth);
    void reset()
    {
        remainder_ = 0;
        unit_ = 0;
    }

  private:
    int remainder_ = 0;
    int unit_ = 0;
};
inline constexpr std::array<int, 3> annotationTextSizes = {18, 28, 40};
struct AnnotationStyle
{
    QColor color = annotationColors()[0];
    int lineWidth = annotationDefaultWidth;
    int textSize = annotationTextSizes[1];
    QString fontFamily;
    bool bold = false;
    bool italic = false;
    bool underline = false;
};
struct Annotation
{
    AnnotationType type = AnnotationType::Rectangle;
    AnnotationStyle style;
    // 全部几何均为冻结帧绝对物理像素；文本起点是排版框左上角。
    QPointF first;
    QPointF last;
    QVector<QPointF> points;
    QString text;
    // 围绕基础包围盒中心的旋转角，单位度；屏幕坐标 y 向下。
    qreal rotation = 0;
    bool isVisible() const;
};
QFont annotationFont(const AnnotationStyle& style);
// 旋转前 / 旋转后的源图像素包围盒；文本使用与输出一致的 QTextDocument 排版。
QRectF annotationBaseBounds(const Annotation& annotation);
QRectF annotationBounds(const Annotation& annotation);
QPolygonF annotationCorners(const Annotation& annotation);
QPointF annotationCenter(const Annotation& annotation);
// 上边中点沿局部坐标向上偏移后的旋转手柄位置。
QPointF annotationRotationHandle(const Annotation& annotation, qreal offset = 24);
// 在基础坐标与旋转后坐标之间转换，供命中测试和手柄使用。
QPointF annotationToBase(const Annotation& annotation, QPointF point);
QPointF annotationFromBase(const Annotation& annotation, QPointF point);
void translateAnnotation(Annotation& annotation, QPointF delta);
qreal normalizedRotation(qreal degrees);
qreal snapAnnotationRotation(qreal degrees, qreal step = 15);
void setAnnotationRotation(Annotation& annotation, qreal degrees);
bool annotationHitTest(const Annotation& annotation, QPointF point, qreal tolerance = 4);
// 返回终点尖端、底边两端；头长不超过段长。预览和导出共用，杆止于底边中点。
QVector<QPointF> arrowHead(const Annotation& annotation);
void appendFreehandPoint(Annotation& annotation, QPointF point, qreal minimumDistance = 1);
class AnnotationHistory
{
  public:
    static constexpr int capacity = 20;
    AnnotationHistory();
    const QVector<Annotation>& annotations() const;
    bool add(Annotation annotation);
    // 调整最近一条标注的线宽；仅用于刚绘制完、仍处于可调选中态的图形。
    // 不新增历史项，清空重做栈；返回是否实际改变。
    bool setLastLineWidth(int width);
    // 调整最近一条文本的字号与整段粗体 / 斜体 / 下划线；规则同线宽。
    bool setLastTextStyle(int textSize, bool bold, bool italic, bool underline);
    // 对象移动 / 旋转使用编辑事务：手势期间实时改当前状态，松开时提交为一步。
    bool beginEdit();
    bool updateAnnotation(qsizetype index, Annotation annotation);
    bool commitEdit();
    void cancelEdit();
    bool isEditing() const { return editing_; }
    bool canUndo() const { return !editing_ && current_ > 0; }
    bool canRedo() const { return !editing_ && current_ + 1 < states_.size(); }
    bool undo();
    bool redo();
    void clear();

  private:
    void truncateRedo();
    void appendState(QVector<Annotation> state);
    void trimCapacity();
    QVector<QVector<Annotation>> states_;
    qsizetype current_ = 0;
    bool editing_ = false;
    bool editChanged_ = false;
    QVector<Annotation> editBefore_;
};
}
