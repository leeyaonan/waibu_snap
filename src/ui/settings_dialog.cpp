#include "ui/settings_dialog.h"
#include "app/hotkey_rules.h"
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
namespace waibusnap
{
SettingsDialog::SettingsDialog(const QKeySequence& current, SaveHotkey save, const QString& notice,
                               QWidget* parent)
    : QDialog(parent), saveHotkey_(std::move(save))
{
    setWindowTitle(QStringLiteral("WaibuSnap 设置"));
    setMinimumWidth(440);
    auto* layout = new QVBoxLayout(this);
    auto* title = new QLabel(QStringLiteral("截图快捷键"), this);
    layout->addWidget(title);
    editor_ = new QKeySequenceEdit(this);
    editor_->setObjectName(QStringLiteral("hotkeyEditor"));
    editor_->setMaximumSequenceLength(1);
    editor_->setKeySequence(current);
    editor_->setClearButtonEnabled(true);
    title->setBuddy(editor_);
    layout->addWidget(editor_);
    editor_->installEventFilter(this);
    for (QObject* child : editor_->findChildren<QObject*>())
        child->installEventFilter(this);
    auto* help = new QLabel(
        QStringLiteral(
            "支持 F1–F12，或带 Ctrl / Command、Control、Alt / Option 的字母和数字。\n"
            "Mac 的 F1 可能需要配合 Fn / 地球键或系统功能键模式；应用不修改系统键盘设置。\n"
            "窗口打开期间暂停截图快捷键。"),
        this);
    help->setObjectName(QStringLiteral("hotkeyHelp"));
    help->setWordWrap(true);
    layout->addWidget(help);
    status_ = new QLabel(notice, this);
    status_->setObjectName(QStringLiteral("settingsStatus"));
    status_->setTextFormat(Qt::PlainText);
    status_->setWordWrap(true);
    status_->setVisible(!notice.isEmpty());
    layout->addWidget(status_);
    auto* buttons = new QDialogButtonBox(this);
    auto* saveButton = buttons->addButton(QStringLiteral("保存"), QDialogButtonBox::AcceptRole);
    saveButton->setObjectName(QStringLiteral("saveSettingsButton"));
    auto* cancelButton = buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole);
    cancelButton->setObjectName(QStringLiteral("cancelSettingsButton"));
    layout->addWidget(buttons);
    connect(saveButton, &QPushButton::clicked, this, &SettingsDialog::save);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
}
bool SettingsDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape)
    {
        reject();
        return true;
    }
    return QDialog::eventFilter(watched, event);
}
void SettingsDialog::save()
{
    const auto sequence = editor_->keySequence();
    QString error = hotkeyValidationError(sequence);
    if (error.isEmpty())
        error = saveHotkey_(sequence);
    if (!error.isEmpty())
    {
        status_->setText(error);
        status_->show();
        return;
    }
    accept();
}
}
