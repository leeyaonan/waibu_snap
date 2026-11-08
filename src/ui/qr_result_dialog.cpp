#include "ui/qr_result_dialog.h"
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
namespace waibusnap
{
QrResultDialog::QrResultDialog(QWidget* parent, std::function<bool(const QUrl&)> openUrl)
    : QDialog(parent), openUrl_(std::move(openUrl))
{
    setObjectName(QStringLiteral("qrResultDialog"));
    setWindowTitle(QStringLiteral("二维码"));
    setWindowModality(Qt::WindowModal);
    resize(560, 340);
    if (!openUrl_)
        openUrl_ = [](const QUrl& url) { return QDesktopServices::openUrl(url); };
    auto* layout = new QVBoxLayout(this);
    state_ = new QLabel(this);
    state_->setObjectName(QStringLiteral("qrStateLabel"));
    state_->setTextFormat(Qt::PlainText);
    state_->setWordWrap(true);
    layout->addWidget(state_);
    list_ = new QListWidget(this);
    list_->setObjectName(QStringLiteral("qrResultList"));
    list_->setMaximumHeight(108);
    list_->hide();
    layout->addWidget(list_);
    content_ = new QPlainTextEdit(this);
    content_->setObjectName(QStringLiteral("qrContentEdit"));
    content_->setReadOnly(true);
    content_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    content_->hide();
    layout->addWidget(content_, 1);
    auto* actions = new QHBoxLayout;
    const auto makeButton = [this](const QString& text, const QString& name)
    {
        auto* button = new QPushButton(text, this);
        button->setObjectName(name);
        button->setAutoDefault(false);
        return button;
    };
    retry_ = makeButton(QStringLiteral("重新扫描"), QStringLiteral("qrRetryButton"));
    copy_ = makeButton(QStringLiteral("复制内容"), QStringLiteral("qrCopyButton"));
    open_ = makeButton(QStringLiteral("打开链接"), QStringLiteral("qrOpenButton"));
    close_ = makeButton(QStringLiteral("关闭"), QStringLiteral("qrCloseButton"));
    feedback_ = new QLabel(this);
    feedback_->setObjectName(QStringLiteral("qrCopyFeedback"));
    actions->addWidget(retry_);
    actions->addStretch();
    actions->addWidget(feedback_);
    actions->addWidget(copy_);
    actions->addWidget(open_);
    actions->addWidget(close_);
    layout->addLayout(actions);
    connect(list_, &QListWidget::currentRowChanged, this, [this](int row) { currentChanged(row); });
    connect(retry_, &QPushButton::clicked, this,
            [this]
            {
                setScanning();
                emit retryRequested();
            });
    connect(copy_, &QPushButton::clicked, this,
            [this]
            {
                const QString text = currentText();
                if (text.isEmpty())
                    return;
                emit copyRequested(text);
                feedback_->setText(QStringLiteral("已复制"));
                feedbackTimeout_.start(2000);
            });
    connect(open_, &QPushButton::clicked, this,
            [this]
            {
                const QString text = currentText().trimmed();
                if (!openable(text))
                    return;
                const bool opened = openUrl_ && openUrl_(QUrl::fromUserInput(text));
                feedback_->setText(opened ? QStringLiteral("已在浏览器打开")
                                          : QStringLiteral("无法打开链接"));
                feedbackTimeout_.start(2000);
            });
    connect(close_, &QPushButton::clicked, this, &QDialog::reject);
    feedbackTimeout_.setSingleShot(true);
    connect(&feedbackTimeout_, &QTimer::timeout, feedback_, &QLabel::clear);
    setScanning();
}
void QrResultDialog::setScanning()
{
    scanning_ = true;
    codes_.clear();
    list_->clear();
    list_->hide();
    content_->clear();
    content_->hide();
    state_->setText(QStringLiteral("正在识别二维码…"));
    state_->show();
    retry_->hide();
    copy_->setEnabled(false);
    copy_->setText(QStringLiteral("复制内容"));
    open_->setEnabled(false);
    feedbackTimeout_.stop();
    feedback_->clear();
}
void QrResultDialog::setResult(const QrRecognitionResult& result)
{
    scanning_ = false;
    codes_ = result.codes;
    list_->clear();
    feedbackTimeout_.stop();
    feedback_->clear();
    if (!result.ok)
    {
        state_->setText(QStringLiteral("识别失败：%1").arg(result.explanation));
        state_->show();
        list_->hide();
        content_->hide();
        retry_->show();
        copy_->setEnabled(false);
        copy_->setText(QStringLiteral("复制内容"));
        open_->setEnabled(false);
        return;
    }
    if (codes_.isEmpty())
    {
        state_->setText(QStringLiteral("未识别到二维码"));
        state_->show();
        list_->hide();
        content_->hide();
        retry_->show();
        copy_->setEnabled(false);
        copy_->setText(QStringLiteral("复制内容"));
        open_->setEnabled(false);
        return;
    }
    if (codes_.size() > 1)
    {
        for (int index = 0; index < codes_.size(); ++index)
            list_->addItem(QStringLiteral("二维码 %1").arg(index + 1));
        list_->show();
        state_->setText(
            QStringLiteral("识别到 %1 个二维码，选择后复制或打开。").arg(codes_.size()));
    }
    else
    {
        list_->hide();
        state_->setText(QStringLiteral("识别到二维码。"));
    }
    state_->show();
    content_->setPlainText(codes_.first().text);
    content_->show();
    retry_->show();
    copy_->setEnabled(true);
    if (codes_.size() > 1)
        list_->setCurrentRow(0);
    updateOpenState();
}
void QrResultDialog::currentChanged(int row)
{
    if (row < 0 || row >= codes_.size())
        return;
    content_->setPlainText(codes_[row].text);
    updateOpenState();
}
void QrResultDialog::updateOpenState()
{
    const bool canOpen = !scanning_ && openable(currentText());
    copy_->setText(canOpen ? QStringLiteral("复制链接") : QStringLiteral("复制内容"));
    open_->setEnabled(canOpen);
    open_->setToolTip(canOpen ? QStringLiteral("使用系统默认浏览器打开")
                              : QStringLiteral("内容不是 http/https 链接"));
    open_->setAccessibleName(open_->toolTip());
}
QString QrResultDialog::currentText() const
{
    if (codes_.isEmpty())
        return {};
    const int row = list_->currentRow();
    return codes_[row >= 0 && row < codes_.size() ? row : 0].text;
}
bool QrResultDialog::openable(const QString& text)
{
    if (!text.startsWith(QStringLiteral("http://"), Qt::CaseInsensitive) &&
        !text.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive))
        return false;
    const QUrl url = QUrl::fromUserInput(text);
    return url.isValid() && !url.host().isEmpty();
}
} // namespace waibusnap
