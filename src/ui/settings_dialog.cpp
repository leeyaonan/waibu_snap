#include "ui/settings_dialog.h"
#include "app/hotkey_rules.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
namespace waibusnap
{
SettingsDialog::SettingsDialog(const QKeySequence& current, SaveHotkey save, const QString& notice,
                               QWidget* parent, SaveSettingsActions saveSettings,
                               AutostartActions autostartActions)
    : QDialog(parent), saveHotkey_(std::move(save)), originalSequence_(current),
      saveSettings_(std::move(saveSettings)), autostartActions_(std::move(autostartActions))
{
    if (!autostartActions_.query || !autostartActions_.setEnabled)
    {
        const std::shared_ptr<Autostart> autostart = createAutostart();
        if (!autostartActions_.query)
            autostartActions_.query = [autostart] { return autostart->query(); };
        if (!autostartActions_.setEnabled)
            autostartActions_.setEnabled = [autostart](bool enabled)
            { return autostart->setEnabled(enabled); };
    }
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
    auto* saveGroup = new QGroupBox(QStringLiteral("保存"), this);
    auto* saveLayout = new QVBoxLayout(saveGroup);
    saveLayout->addWidget(
        new QLabel(QStringLiteral("快速保存目录（留空时每次选择路径）"), saveGroup));
    auto* directoryLayout = new QHBoxLayout;
    directory_ = new QLineEdit(saveSettings_.preferences.quickDirectory, saveGroup);
    directory_->setObjectName(QStringLiteral("quickDirectory"));
    directory_->setReadOnly(true);
    directory_->setPlaceholderText(QStringLiteral("每次选择路径"));
    directory_->setToolTip(directory_->text());
    auto* choose = new QPushButton(QStringLiteral("选择…"), saveGroup);
    choose->setObjectName(QStringLiteral("chooseDirectoryButton"));
    auto* clear = new QPushButton(QStringLiteral("清除"), saveGroup);
    clear->setObjectName(QStringLiteral("clearDirectoryButton"));
    directoryLayout->addWidget(directory_, 1);
    directoryLayout->addWidget(choose);
    directoryLayout->addWidget(clear);
    saveLayout->addLayout(directoryLayout);
    if (!saveSettings_.chooseDirectory)
        saveSettings_.chooseDirectory = [](QWidget* parent, const QString& current) {
            return QFileDialog::getExistingDirectory(parent, QStringLiteral("选择快速保存目录"),
                                                     current);
        };
    connect(choose, &QPushButton::clicked, this,
            [this]
            {
                const QString selected = saveSettings_.chooseDirectory(this, directory_->text());
                if (!selected.isEmpty())
                {
                    directory_->setText(QDir(selected).absolutePath());
                    directory_->setToolTip(directory_->text());
                }
            });
    connect(clear, &QPushButton::clicked, this,
            [this]
            {
                directory_->clear();
                directory_->setToolTip({});
            });
    format_ = new QComboBox(saveGroup);
    format_->setObjectName(QStringLiteral("saveFormat"));
    format_->addItems({QStringLiteral("PNG"), QStringLiteral("JPEG")});
    format_->setCurrentIndex(saveSettings_.preferences.format == ImageFormat::Jpeg ? 1 : 0);
    saveLayout->addWidget(format_);
    auto* lossNotice = new QLabel(
        QStringLiteral("JPEG 为有损格式，压缩可能改变颜色与细节；需保留原像素时请选择 PNG。"),
        saveGroup);
    lossNotice->setObjectName(QStringLiteral("jpegLossNotice"));
    lossNotice->setWordWrap(true);
    lossNotice->setVisible(format_->currentIndex() == 1);
    saveLayout->addWidget(lossNotice);
    connect(format_, &QComboBox::currentIndexChanged, lossNotice,
            [lossNotice](int index) { lossNotice->setVisible(index == 1); });
    layout->addWidget(saveGroup);
    auto* startupGroup = new QGroupBox(QStringLiteral("启动"), this);
    auto* startupLayout = new QVBoxLayout(startupGroup);
    autostartCheck_ = new QCheckBox(QStringLiteral("登录时自动启动"), startupGroup);
    autostartCheck_->setObjectName(QStringLiteral("autostartCheck"));
    const auto startupState = autostartActions_.query();
    autostartCheck_->setChecked(startupState.enabled);
    startupLayout->addWidget(autostartCheck_);
    auto* startupHelp = new QLabel(startupGroup);
    startupHelp->setObjectName(QStringLiteral("autostartHelp"));
    startupHelp->setTextFormat(Qt::PlainText);
    startupHelp->setWordWrap(true);
    startupHelp->setText(
        !startupState.notice.isEmpty() ? startupState.notice
        : startupState.pendingApproval
            ? QStringLiteral("已在系统设置中等待批准，请在「登录项」中允许。")
            : QStringLiteral("默认关闭。开启后，下次登录时自动运行；状态以系统注册为准。"));
    startupLayout->addWidget(startupHelp);
    layout->addWidget(startupGroup);
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
    if (error.isEmpty() && sequence != originalSequence_)
    {
        error = saveHotkey_(sequence);
        if (error.isEmpty())
            originalSequence_ = sequence;
    }
    if (error.isEmpty() && saveSettings_.savePreferences)
        error = saveSettings_.savePreferences({directory_->text(), format_->currentIndex() == 1
                                                                       ? ImageFormat::Jpeg
                                                                       : ImageFormat::Png});
    if (error.isEmpty() && autostartCheck_->isChecked() != autostartActions_.query().enabled)
        error = autostartActions_.setEnabled(autostartCheck_->isChecked());
    if (!error.isEmpty())
    {
        status_->setText(error);
        status_->show();
        return;
    }
    accept();
}
}
