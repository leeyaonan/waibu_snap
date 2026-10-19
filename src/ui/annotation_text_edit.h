#pragma once
#include <QTextEdit>
namespace waibusnap
{
class AnnotationTextEdit final : public QTextEdit
{
    Q_OBJECT
  public:
    explicit AnnotationTextEdit(QWidget* parent);
    bool isComposing() const { return composing_; }
    void clearPreedit();
    void setEditingArea(QPoint origin, QRect bounds);
    // 将整段文本与后续输入统一为给定字体（含粗体 / 斜体 / 下划线）；
    // 只改字体属性，不改变颜色、文本内容或光标选择。
    void setAnnotationFont(const QFont& font);

  signals:
    void commitRequested();
    void cancelRequested();
    // 0 / 1 / 2 对应粗体 / 斜体 / 下划线；由持有标注样式的编辑面处理。
    void formatToggleRequested(int format);

  protected:
    bool event(QEvent* event) override;
    bool viewportEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;

  private:
    void updateEditorGeometry();
    bool composing_ = false;
    bool updatingGeometry_ = false;
    QPoint origin_;
    QRect bounds_;
};
}
