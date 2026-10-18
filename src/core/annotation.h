#pragma once
#include <QColor>
#include <QFont>
#include <QPointF>
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
bool annotationUsesLineWidth(AnnotationType type);
// 两编辑面共用高分辨率滚轮累积；角度优先，只有像素输入时每 15px 一格。
class AnnotationWidthWheel
{
  public:
    int adjust(int width, QPoint angleDelta, QPoint pixelDelta = {});
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
    bool isVisible() const;
};
QFont annotationFont(const AnnotationStyle& style);
// 返回终点尖端、底边两端；头长不超过段长。预览和导出共用，杆止于底边中点。
QVector<QPointF> arrowHead(const Annotation& annotation);
void appendFreehandPoint(Annotation& annotation, QPointF point, qreal minimumDistance = 1);
class AnnotationHistory
{
  public:
    static constexpr int capacity = 20;
    const QVector<Annotation>& annotations() const { return annotations_; }
    bool add(Annotation annotation);
    bool canUndo() const { return annotations_.size() > firstUndoable_; }
    bool canRedo() const { return !redo_.isEmpty(); }
    bool undo();
    bool redo();
    void clear();

  private:
    QVector<Annotation> annotations_;
    QVector<Annotation> redo_;
    // 超限只丢弃早期撤销资格，不丢弃仍需显示和导出的标注。
    qsizetype firstUndoable_ = 0;
};
}
