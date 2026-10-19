#include "ui/annotation_text_edit.h"
#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QScopedValueRollback>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextFrame>
#include <QtMath>
#include <algorithm>
namespace waibusnap
{
AnnotationTextEdit::AnnotationTextEdit(QWidget* parent) : QTextEdit(parent)
{
    setObjectName(QStringLiteral("annotationTextEditor"));
    setAcceptRichText(false);
    setLineWrapMode(QTextEdit::NoWrap);
    setTabChangesFocus(false);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setPlaceholderText(QStringLiteral("输入文字"));
    setToolTip(QStringLiteral("可换行 · ⌘ / Ctrl+Enter 完成 · Esc 取消编辑"));
    QPalette transparent = palette();
    transparent.setColor(QPalette::Base, Qt::transparent);
    setPalette(transparent);
    setAutoFillBackground(false);
    viewport()->setAutoFillBackground(false);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent, false);
    document()->setDocumentMargin(0);
    connect(this, &QTextEdit::textChanged, this, &AnnotationTextEdit::updateEditorGeometry);
    connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this,
            &AnnotationTextEdit::updateEditorGeometry);
}
void AnnotationTextEdit::setEditingArea(QPoint origin, QRect bounds)
{
    bounds_ = bounds;
    if (bounds_.isEmpty())
        return;
    // 起笔先给一行与初始宽度留出空间；后续固定起点增长，不在输入时跳动文字。
    const int initialWidth = std::min(240, bounds_.width());
    const int initialHeight = std::min(qCeil(QFontMetricsF(font()).height()), bounds_.height());
    origin_ = {std::clamp(origin.x(), bounds_.left(), bounds_.right() - initialWidth + 1),
               std::clamp(origin.y(), bounds_.top(), bounds_.bottom() - initialHeight + 1)};
    updateEditorGeometry();
}
void AnnotationTextEdit::setAnnotationFont(const QFont& font)
{
    if (composing_)
        return;
    setFont(font);
    QTextCharFormat format;
    format.setFont(font);
    format.setFontWeight(font.weight());
    format.setFontItalic(font.italic());
    format.setFontUnderline(font.underline());
    QTextCursor cursor = textCursor();
    const int anchor = cursor.anchor();
    const int position = cursor.position();
    cursor.select(QTextCursor::Document);
    cursor.mergeCharFormat(format);
    cursor.setPosition(anchor);
    cursor.setPosition(position, QTextCursor::KeepAnchor);
    setTextCursor(cursor);
    setCurrentCharFormat(format);
    updateEditorGeometry();
}
void AnnotationTextEdit::updateEditorGeometry()
{
    if (bounds_.isEmpty() || updatingGeometry_)
        return;
    // resize 会再次触发文档排版通知，只处理本轮；不替换文档或移动光标 / 预编辑。
    QScopedValueRollback<bool> guard(updatingGeometry_, true);
    // clear / setPlainText 会重置根框架；仅行尾留出光标宽度，不改变文本左上起点。
    QTextFrameFormat frame = document()->rootFrame()->frameFormat();
    if (frame.rightMargin() != cursorWidth())
    {
        frame.setRightMargin(cursorWidth());
        document()->rootFrame()->setFrameFormat(frame);
    }
    const int desiredWidth = std::clamp(qCeil(document()->idealWidth()) + cursorWidth(), 240, 400);
    const int desiredHeight =
        std::min(240, qCeil(std::max(document()->size().height(), QFontMetricsF(font()).height())));
    setGeometry(QRect(origin_, QSize(std::min(desiredWidth, bounds_.right() - origin_.x() + 1),
                                     std::min(desiredHeight, bounds_.bottom() - origin_.y() + 1))));
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
bool AnnotationTextEdit::viewportEvent(QEvent* event)
{
    const bool handled = QTextEdit::viewportEvent(event);
    // Qt 的 placeholder 在获得焦点后会隐藏，透明编辑框会因此完全没有落笔提示。
    // 空文档且有焦点时自绘高对比提示：黑字 + 白描边，深浅底色都可见；
    // 不画背景或边框，提交后仅保留实际文字。
    if (event->type() == QEvent::Paint && hasFocus() && document()->isEmpty() && !composing_ &&
        !placeholderText().isEmpty())
    {
        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRect caret = cursorRect();
        const QFontMetricsF metrics(font());
        const QPointF baseline(caret.left() + 3.0, std::max(caret.top(), 0) + metrics.ascent());
        QPainterPath path;
        path.addText(baseline, font(), placeholderText());
        painter.strokePath(path, QPen(QColor(255, 255, 255, 230), 2.0));
        painter.fillPath(path, QColor(0, 0, 0, 190));
    }
    return handled;
}
void AnnotationTextEdit::inputMethodEvent(QInputMethodEvent* event)
{
    composing_ = !event->preeditString().isEmpty();
    QTextEdit::inputMethodEvent(event);
}
void AnnotationTextEdit::clearPreedit()
{
    QInputMethodEvent clear;
    inputMethodEvent(&clear);
}
void AnnotationTextEdit::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape)
    {
        if (composing_)
        {
            QGuiApplication::inputMethod()->reset();
            clearPreedit();
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
    else if (event->modifiers() == Qt::ControlModifier &&
             (event->key() == Qt::Key_B || event->key() == Qt::Key_I || event->key() == Qt::Key_U))
    {
        const int format = event->key() == Qt::Key_B ? 0 : event->key() == Qt::Key_I ? 1 : 2;
        emit formatToggleRequested(format);
    }
    else
        QTextEdit::keyPressEvent(event);
    // QTextEdit 不处理的按键也不向父层传播，尤其是输入法候选操作。
    event->accept();
}
}
