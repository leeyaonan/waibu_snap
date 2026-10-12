#include "ui/settings_dialog.h"
#include "app/hotkey_rules.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
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
    const auto ratios = sanitizedRatioSettings(saveSettings_.ratios);
    for (const auto& ratio : ratios.enabled)
        ratioRows_.append({ratio, true});
    for (const auto& ratio : ratios.disabled)
        ratioRows_.append({ratio, false});
    quickRatio_ = ratios.quick;
    auto* ratioGroup = new QGroupBox(QStringLiteral("截图比例"), this);
    auto* ratioLayout = new QVBoxLayout(ratioGroup);
    // 滚动区的最小高度必须传到分组与对话框，防止首次布局压入添加行。
    ratioLayout->setSizeConstraint(QLayout::SetMinimumSize);
    ratioScroll_ = new QScrollArea(ratioGroup);
    ratioScroll_->setWidgetResizable(true);
    ratioScroll_->setMinimumHeight(160);
    ratioScroll_->setMaximumHeight(240);
    ratioLayout->addWidget(ratioScroll_);
    ratioEmptyNotice_ = new QLabel(QStringLiteral("尚未启用任何比例"), ratioGroup);
    ratioEmptyNotice_->setObjectName(QStringLiteral("ratioEmptyNotice"));
    ratioLayout->addWidget(ratioEmptyNotice_);
    auto* addLayout = new QHBoxLayout;
    ratioWidthInput_ = new QDoubleSpinBox(ratioGroup);
    ratioHeightInput_ = new QDoubleSpinBox(ratioGroup);
    ratioWidthInput_->setObjectName(QStringLiteral("ratioWidthInput"));
    ratioHeightInput_->setObjectName(QStringLiteral("ratioHeightInput"));
    for (auto* input : {ratioWidthInput_, ratioHeightInput_})
    {
        input->setRange(0.01, 999.99);
        input->setDecimals(2);
        input->setSingleStep(0.1);
        input->setValue(1);
    }
    auto* add = new QPushButton(QStringLiteral("添加"), ratioGroup);
    add->setObjectName(QStringLiteral("ratioAddButton"));
    addLayout->addWidget(new QLabel(QStringLiteral("宽"), ratioGroup));
    addLayout->addWidget(ratioWidthInput_);
    addLayout->addWidget(new QLabel(QStringLiteral(":"), ratioGroup));
    addLayout->addWidget(new QLabel(QStringLiteral("高"), ratioGroup));
    addLayout->addWidget(ratioHeightInput_);
    addLayout->addWidget(add);
    ratioLayout->addLayout(addLayout);
    connect(add, &QPushButton::clicked, this,
            [this]
            {
                const auto validInput = [](QDoubleSpinBox* input)
                {
                    bool ok = false;
                    const double value = input->locale().toDouble(input->cleanText(), &ok);
                    return input->hasAcceptableInput() && ok && std::isfinite(value) &&
                           value >= 0.01 && value <= 999.99;
                };
                QString error;
                if (!validInput(ratioWidthInput_) || !validInput(ratioHeightInput_))
                    error = QStringLiteral("比例数值需为 0.01–999.99。");
                else
                {
                    ratioWidthInput_->interpretText();
                    ratioHeightInput_->interpretText();
                    const AspectRatio ratio{ratioWidthInput_->value(), ratioHeightInput_->value()};
                    const bool duplicate = std::any_of(ratioRows_.begin(), ratioRows_.end(),
                                                       [&ratio](const RatioRow& row) {
                                                           return sameAspectRatio(row.ratio, ratio);
                                                       });
                    if (duplicate)
                        error = QStringLiteral("该比例已存在。");
                    else
                    {
                        ratioRows_.append({ratio, true});
                        rebuildRatioList();
                    }
                }
                status_->setText(error);
                status_->setVisible(!error.isEmpty());
            });
    auto* restore = new QPushButton(QStringLiteral("恢复默认比例"), ratioGroup);
    restore->setObjectName(QStringLiteral("ratioRestoreButton"));
    ratioLayout->addWidget(restore, 0, Qt::AlignLeft);
    connect(restore, &QPushButton::clicked, this,
            [this]
            {
                const auto defaults = defaultCaptureRatios();
                QVector<RatioRow> rows;
                for (const auto& ratio : defaults)
                    rows.append({ratio, true});
                for (const auto& row : ratioRows_)
                    if (std::none_of(defaults.begin(), defaults.end(),
                                     [&row](const AspectRatio& ratio)
                                     { return sameAspectRatio(row.ratio, ratio); }))
                        rows.append(row);
                ratioRows_ = std::move(rows);
                rebuildRatioList();
                status_->hide();
            });
    auto* quickLayout = new QHBoxLayout;
    auto* quickLabel = new QLabel(QStringLiteral("按住 Shift 快速使用的比例"), ratioGroup);
    quickRatioCombo_ = new QComboBox(ratioGroup);
    quickRatioCombo_->setObjectName(QStringLiteral("quickRatioCombo"));
    quickLabel->setBuddy(quickRatioCombo_);
    quickLayout->addWidget(quickLabel);
    quickLayout->addWidget(quickRatioCombo_, 1);
    ratioLayout->addLayout(quickLayout);
    connect(quickRatioCombo_, &QComboBox::currentIndexChanged, this,
            [this](int index) {
                quickRatio_ =
                    index < 0 ? std::nullopt : parseAspectRatio(quickRatioCombo_->itemText(index));
            });
    rebuildRatioList();
    layout->addWidget(ratioGroup);
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
CaptureRatioSettings SettingsDialog::collectRatios() const
{
    CaptureRatioSettings result;
    result.quick = quickRatio_;
    for (const auto& row : ratioRows_)
        (row.enabled ? result.enabled : result.disabled).append(row.ratio);
    return sanitizedRatioSettings(result);
}
void SettingsDialog::refreshQuickRatios()
{
    const auto ratios = collectRatios();
    quickRatio_ = ratios.quick;
    const QSignalBlocker block(quickRatioCombo_);
    quickRatioCombo_->clear();
    int current = 0;
    for (const auto& ratio : ratios.enabled)
    {
        if (quickRatio_ && sameAspectRatio(ratio, *quickRatio_))
            current = quickRatioCombo_->count();
        quickRatioCombo_->addItem(formatAspectRatio(ratio));
    }
    quickRatioCombo_->setCurrentIndex(ratios.enabled.isEmpty() ? -1 : current);
    quickRatioCombo_->setEnabled(!ratios.enabled.isEmpty());
    quickRatioCombo_->setToolTip(ratios.enabled.isEmpty()
                                     ? QStringLiteral("尚未启用任何比例，Shift 快捷比例不可用。")
                                     : QString());
    ratioEmptyNotice_->setVisible(ratios.enabled.isEmpty());
}
void SettingsDialog::rebuildRatioList()
{
    if (auto* old = ratioScroll_->takeWidget())
    {
        old->setParent(nullptr);
        old->deleteLater();
    }
    auto* list = new QWidget;
    list->setObjectName(QStringLiteral("ratioList"));
    auto* layout = new QVBoxLayout(list);
    for (int index = 0; index < ratioRows_.size(); ++index)
    {
        const auto row = ratioRows_[index];
        auto* line = new QHBoxLayout;
        line->addWidget(new QLabel(formatAspectRatio(row.ratio), list), 1);
        auto* toggle = new QCheckBox(QStringLiteral("启用"), list);
        toggle->setObjectName(QStringLiteral("ratioToggle%1").arg(index));
        toggle->setChecked(row.enabled);
        toggle->setAccessibleName(QStringLiteral("启用 %1").arg(formatAspectRatio(row.ratio)));
        auto* remove = new QPushButton(QStringLiteral("删除"), list);
        remove->setObjectName(QStringLiteral("ratioDelete%1").arg(index));
        remove->setAccessibleName(QStringLiteral("删除 %1").arg(formatAspectRatio(row.ratio)));
        line->addWidget(toggle);
        line->addWidget(remove);
        layout->addLayout(line);
        connect(toggle, &QCheckBox::toggled, this,
                [this, index](bool enabled)
                {
                    ratioRows_[index].enabled = enabled;
                    refreshQuickRatios();
                });
        connect(remove, &QPushButton::clicked, this,
                [this, index]
                {
                    ratioRows_.removeAt(index);
                    rebuildRatioList();
                });
    }
    layout->addStretch();
    ratioScroll_->setWidget(list);
    refreshQuickRatios();
}
bool SettingsDialog::event(QEvent* event)
{
    const bool handled = QDialog::event(event);
    if (layout() && (event->type() == QEvent::Show || event->type() == QEvent::Resize ||
                     event->type() == QEvent::LayoutRequest))
    {
        // 普通最小高度不计实际宽度下的文本换行，首屏或错误提示可能挤占底部按钮。
        const int height =
            std::max(layout()->minimumSize().height(), layout()->minimumHeightForWidth(width()));
        if (minimumHeight() != height)
        {
            setMinimumHeight(height);
            layout()->invalidate();
            layout()->activate();
        }
    }
    return handled;
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
    if (error.isEmpty() && saveSettings_.saveRatios)
        error = saveSettings_.saveRatios(collectRatios());
    if (!error.isEmpty())
    {
        status_->setText(error);
        status_->show();
        return;
    }
    accept();
}
}
