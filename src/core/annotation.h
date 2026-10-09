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
const std::array<QColor, 3>& annotationColors();
inline constexpr std::array<int, 3> annotationLineWidths = {2, 4, 8};
inline constexpr std::array<int, 3> annotationTextSizes = {18, 28, 40};
struct AnnotationStyle
{
    QColor color = annotationColors()[0];
    int lineWidth = annotationLineWidths[1];
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
// 三角箭头头部与线宽联动，预览和导出只使用这一份几何。
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
