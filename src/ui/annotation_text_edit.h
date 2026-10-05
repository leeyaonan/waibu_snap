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

  signals:
    void commitRequested();
    void cancelRequested();

  protected:
    bool event(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void inputMethodEvent(QInputMethodEvent* event) override;

  private:
    bool composing_ = false;
};
}
