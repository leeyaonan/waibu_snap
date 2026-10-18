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

  signals:
    void commitRequested();
    void cancelRequested();

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
