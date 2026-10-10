#include "ui/ocr_dialog.h"
#include "core/annotation.h"
#include "core/text_layout.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeData>
#include <QPushButton>
#include <QShortcut>
#include <QTextEdit>
#include <QTextList>
#include <QVBoxLayout>
namespace waibusnap
{
namespace
{
class PreviewTextEdit final : public QTextEdit
{
  public:
    using QTextEdit::QTextEdit;

  protected:
    QMimeData* createMimeDataFromSelection() const override
    {
        if (!isReadOnly())
            return QTextEdit::createMimeDataFromSelection();
        auto* mime = new QMimeData;
        mime->setText(textCursor().selectedText().replace(QChar::ParagraphSeparator, '\n'));
        return mime;
    }
};
}
OcrDialog::OcrDialog(QWidget* parent, std::function<void(const QMimeData&)> setClipboard)
    : QDialog(parent), setClipboard_(std::move(setClipboard))
{
    setObjectName(QStringLiteral("ocrDialog"));
    setWindowTitle(QStringLiteral("提取文字"));
    setWindowModality(Qt::WindowModal);
    resize(640, 480);
    if (!setClipboard_)
        setClipboard_ = [](const QMimeData& source)
        {
            auto* copy = new QMimeData;
            for (const auto& format : source.formats())
                copy->setData(format, source.data(format));
            QApplication::clipboard()->setMimeData(copy);
        };
    auto* layout = new QVBoxLayout(this);
    state_ = new QLabel(this);
    state_->setObjectName(QStringLiteral("ocrStateLabel"));
    state_->setTextFormat(Qt::PlainText);
    state_->setWordWrap(true);
    layout->addWidget(state_);
    formats_ = new QWidget(this);
    formats_->setObjectName(QStringLiteral("ocrFormatToolbar"));
    auto* formatLayout = new QHBoxLayout(formats_);
    formatLayout->setContentsMargins(0, 0, 0, 0);
    const auto button = [this](const QString& text, const QString& name)
    {
        auto* control = new QPushButton(text, this);
        control->setObjectName(name);
        control->setAutoDefault(false);
        return control;
    };
    auto* bold = button(QStringLiteral("粗体"), QStringLiteral("ocrBoldButton"));
    auto* italic = button(QStringLiteral("斜体"), QStringLiteral("ocrItalicButton"));
    auto* underline = button(QStringLiteral("下划线"), QStringLiteral("ocrUnderlineButton"));
    for (auto* control : {bold, italic, underline})
    {
        control->setCheckable(true);
        formatLayout->addWidget(control);
    }
    auto* sizes = new QComboBox(this);
    sizes->setObjectName(QStringLiteral("ocrSizeCombo"));
    sizes->setAccessibleName(QStringLiteral("文字字号"));
    sizes->addItem(QStringLiteral("小"), 12);
    sizes->addItem(QStringLiteral("中"), 16);
    sizes->addItem(QStringLiteral("大"), 22);
    sizes->setCurrentIndex(1);
    formatLayout->addWidget(sizes);
    auto* colors = new QComboBox(this);
    colors->setObjectName(QStringLiteral("ocrColorCombo"));
    colors->setAccessibleName(QStringLiteral("文字颜色"));
    colors->addItem(QStringLiteral("黑"), QColor(Qt::black));
    const QString names[] = {QStringLiteral("红"), QStringLiteral("蓝"), QStringLiteral("黄")};
    const auto palette = annotationColors();
    colors->addItem(names[0], palette[0]);
    colors->addItem(names[1], palette[2]);
    colors->addItem(names[2], palette[1]);
    formatLayout->addWidget(colors);
    auto* bullets = button(QStringLiteral("• 列表"), QStringLiteral("ocrBulletsButton"));
    auto* numbers = button(QStringLiteral("1. 列表"), QStringLiteral("ocrNumbersButton"));
    formatLayout->addWidget(bullets);
    formatLayout->addWidget(numbers);
    layout->addWidget(formats_);
    editor_ = new PreviewTextEdit(this);
    editor_->setObjectName(QStringLiteral("ocrTextEdit"));
    editor_->setAcceptRichText(false);
    editor_->installEventFilter(this);
    QFont editorFont = editor_->font();
    editorFont.setPointSize(16);
    editor_->setFont(editorFont);
    layout->addWidget(editor_, 1);
    auto* actions = new QHBoxLayout;
    edit_ = button(QStringLiteral("编辑"), QStringLiteral("ocrEditButton"));
    retry_ = button(QStringLiteral("重试"), QStringLiteral("ocrRetryButton"));
    copy_ = button(QStringLiteral("复制"), QStringLiteral("ocrCopyButton"));
    plainCopy_ = button(QStringLiteral("仅复制文字"), QStringLiteral("ocrPlainCopyButton"));
    feedback_ = new QLabel(this);
    feedback_->setObjectName(QStringLiteral("ocrCopyFeedback"));
    actions->addWidget(edit_);
    actions->addWidget(retry_);
    actions->addStretch();
    actions->addWidget(feedback_);
    actions->addWidget(copy_);
    actions->addWidget(plainCopy_);
    layout->addLayout(actions);
    connect(edit_, &QPushButton::clicked, this, [this] { setEditing(!editing_); });
    connect(retry_, &QPushButton::clicked, this,
            [this]
            {
                setRecognizing();
                emit retryRequested();
            });
    connect(copy_, &QPushButton::clicked, this, [this] { copyDocument(true); });
    connect(plainCopy_, &QPushButton::clicked, this, [this] { copyDocument(false); });
    connect(bold, &QPushButton::clicked, this,
            [this, bold]
            {
                QTextCharFormat f;
                f.setFontWeight(bold->isChecked() ? QFont::Bold : QFont::Normal);
                applyFormat(f);
            });
    connect(italic, &QPushButton::clicked, this,
            [this, italic]
            {
                QTextCharFormat f;
                f.setFontItalic(italic->isChecked());
                applyFormat(f);
            });
    connect(underline, &QPushButton::clicked, this,
            [this, underline]
            {
                QTextCharFormat f;
                f.setFontUnderline(underline->isChecked());
                applyFormat(f);
            });
    const QKeySequence shortcuts[] = {QKeySequence(Qt::CTRL | Qt::Key_B),
                                      QKeySequence(Qt::CTRL | Qt::Key_I),
                                      QKeySequence(Qt::CTRL | Qt::Key_U)};
    int index = 0;
    for (auto* control : {bold, italic, underline})
    {
        auto* shortcut = new QShortcut(shortcuts[index++], this);
        connect(shortcut, &QShortcut::activated, this,
                [this, control]
                {
                    if (editing_)
                        control->click();
                });
    }
    connect(sizes, &QComboBox::activated, this,
            [this, sizes]
            {
                QTextCharFormat f;
                f.setFontPointSize(sizes->currentData().toInt());
                applyFormat(f);
            });
    connect(colors, &QComboBox::activated, this,
            [this, colors]
            {
                QTextCharFormat f;
                f.setForeground(colors->currentData().value<QColor>());
                applyFormat(f);
            });
    connect(editor_, &QTextEdit::currentCharFormatChanged, this,
            [bold, italic, underline](const QTextCharFormat& f)
            {
                bold->setChecked(f.fontWeight() >= QFont::Bold);
                italic->setChecked(f.fontItalic());
                underline->setChecked(f.fontUnderline());
            });
    const auto list = [this](QTextListFormat::Style style)
    {
        if (!editing_)
            return;
        QTextListFormat format;
        format.setStyle(style);
        editor_->textCursor().createList(format);
        editor_->setFocus();
    };
    connect(bullets, &QPushButton::clicked, this, [list] { list(QTextListFormat::ListDisc); });
    connect(numbers, &QPushButton::clicked, this, [list] { list(QTextListFormat::ListDecimal); });
    feedbackTimeout_.setSingleShot(true);
    connect(&feedbackTimeout_, &QTimer::timeout, feedback_, &QLabel::clear);
    setRecognizing();
}
void OcrDialog::setEditing(bool editing)
{
    editing_ = editing;
    editor_->setReadOnly(!editing);
    formats_->setVisible(editing);
    edit_->setText(editing ? QStringLiteral("完成") : QStringLiteral("编辑"));
    editor_->setFocus();
}
void OcrDialog::setRecognizing()
{
    setEditing(false);
    editor_->clear();
    editor_->hide();
    state_->setText(QStringLiteral("正在识别文字…"));
    state_->show();
    retry_->hide();
    edit_->setEnabled(false);
    copy_->setEnabled(false);
    plainCopy_->setEnabled(false);
    feedbackTimeout_.stop();
    feedback_->clear();
}
void OcrDialog::setResult(const TextRecognitionResult& result)
{
    setRecognizing();
    if (!result.ok)
    {
        state_->setText(QStringLiteral("识别失败：%1").arg(result.explanation));
        retry_->show();
    }
    else if (result.lines.isEmpty())
    {
        state_->setText(QStringLiteral("未识别到文字"));
        retry_->show();
    }
    else
    {
        state_->hide();
        editor_->setPlainText(text_layout::assembleFullText(result.lines));
        editor_->show();
        edit_->setEnabled(true);
        copy_->setEnabled(true);
        plainCopy_->setEnabled(true);
        editor_->setFocus();
    }
}
void OcrDialog::applyFormat(const QTextCharFormat& format)
{
    if (!editing_)
        return;
    editor_->mergeCurrentCharFormat(format);
    editor_->setFocus();
}
void OcrDialog::copyDocument(bool html)
{
    if (!copy_->isEnabled())
        return;
    QMimeData mime;
    mime.setText(editor_->toPlainText());
    if (html)
        mime.setHtml(editor_->document()->toHtml());
    setClipboard_(mime);
    feedback_->setText(QStringLiteral("已复制"));
    feedbackTimeout_.start(2000);
}
bool OcrDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == editor_ && editing_ &&
        (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress))
    {
        auto* key = static_cast<QKeyEvent*>(event);
        const QKeySequence sequence(key->keyCombination());
        const char* name = sequence == QKeySequence(Qt::CTRL | Qt::Key_B)   ? "ocrBoldButton"
                           : sequence == QKeySequence(Qt::CTRL | Qt::Key_I) ? "ocrItalicButton"
                           : sequence == QKeySequence(Qt::CTRL | Qt::Key_U) ? "ocrUnderlineButton"
                                                                            : nullptr;
        if (name)
        {
            event->accept();
            if (event->type() == QEvent::KeyPress)
                findChild<QPushButton*>(QString::fromLatin1(name))->click();
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

}
