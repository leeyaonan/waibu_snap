#include "ui/annotation_text_edit.h"
#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
namespace waibusnap
{
AnnotationTextEdit::AnnotationTextEdit(QWidget* parent) : QTextEdit(parent)
{
    setObjectName(QStringLiteral("annotationTextEditor"));
    setAcceptRichText(false);
    setLineWrapMode(QTextEdit::NoWrap);
    setTabChangesFocus(false);
    setPlaceholderText(QStringLiteral("输入说明，可换行\n⌘ / Ctrl+Enter 完成 · Esc 取消编辑"));
    document()->setDocumentMargin(0);
}
bool AnnotationTextEdit::event(QEvent* event)
{
    if (event->type() == QEvent::ShortcutOverride)
    {
        // 输入期间所有按键先归编辑器 / 输入法，不能触发覆盖层的标注快捷键。
        event->accept();
        return true;
    }
    return QTextEdit::event(event);
}
void AnnotationTextEdit::inputMethodEvent(QInputMethodEvent* event)
{
    composing_ = !event->preeditString().isEmpty();
    QTextEdit::inputMethodEvent(event);
}
void AnnotationTextEdit::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape)
    {
        if (composing_)
        {
            QGuiApplication::inputMethod()->reset();
            QInputMethodEvent clear;
            inputMethodEvent(&clear);
        }
        else
            emit cancelRequested();
    }
    else if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
             event->modifiers().testFlag(Qt::ControlModifier))
    {
        // Qt ControlModifier 对应 macOS Command，Windows 为 Ctrl。
        if (!composing_)
            emit commitRequested();
    }
    else
        QTextEdit::keyPressEvent(event);
    // QTextEdit 不处理的按键也不向父层传播，尤其是输入法候选操作。
    event->accept();
}
}
