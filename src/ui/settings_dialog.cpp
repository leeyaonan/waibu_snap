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
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>
namespace waibusnap
{
SettingsDialog::SettingsDialog(const QKeySequence& current, SaveHotkey save, const QString& notice,
                               QWidget* parent, SaveSettingsActions saveSettings,
                               AutostartActions autostartActions,
                               HistorySettingsActions historySettings)
    : QDialog(parent), saveHotkey_(std::move(save)), originalSequence_(current),
      saveSettings_(std::move(saveSettings)), autostartActions_(std::move(autostartActions)),
      historySettings_(std::move(historySettings))
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
    setMinimumSize(560, 600);
    auto* layout = new QVBoxLayout(this);
    auto* tabs = new QTabWidget(this);
    tabs->setObjectName(QStringLiteral("settingsTabs"));
    layout->addWidget(tabs, 1);
    // 页面一次创建，切换不重建草稿；新类别（如贴图）在这里加入独立页面。
    const auto addPage = [tabs](const QString& title, const QString& name)
    {
        auto* page = new QWidget(tabs);
        page->setObjectName(name);
        tabs->addTab(page, title);
        return page;
    };
    auto* generalPage = addPage(QStringLiteral("通用"), QStringLiteral("settingsGeneralPage"));
    auto* savePage = addPage(QStringLiteral("保存"), QStringLiteral("settingsSavePage"));
    auto* capturePage = addPage(QStringLiteral("截图"), QStringLiteral("settingsCapturePage"));
    auto* stickerPage = addPage(QStringLiteral("贴图"), QStringLiteral("settingsStickerPage"));
    buildHistoryPage(addPage(QStringLiteral("历史"), QStringLiteral("settingsHistoryPage")));
    auto* stickerLayout = new QVBoxLayout(stickerPage);
    stickerCloseReminderCheck_ = new QCheckBox(QStringLiteral("关闭未保存贴图前提醒"), stickerPage);
    stickerCloseReminderCheck_->setObjectName(QStringLiteral("stickerCloseReminderCheck"));
    stickerCloseReminderCheck_->setChecked(saveSettings_.stickerCloseReminder);
    stickerLayout->addWidget(stickerCloseReminderCheck_);
    auto* stickerHelp = new QLabel(
        QStringLiteral("关闭提醒后，单张未保存贴图将直接关闭。应用退出时仍会确认未保存贴图。"),
        stickerPage);
    stickerHelp->setObjectName(QStringLiteral("stickerCloseReminderHelp"));
    stickerHelp->setWordWrap(true);
    stickerLayout->addWidget(stickerHelp);
    stickerLayout->addStretch();
    auto* generalLayout = new QVBoxLayout(generalPage);
    auto* savePageLayout = new QVBoxLayout(savePage);
    auto* captureLayout = new QVBoxLayout(capturePage);
    auto* title = new QLabel(QStringLiteral("截图快捷键"), generalPage);
    generalLayout->addWidget(title);
    editor_ = new QKeySequenceEdit(generalPage);
    editor_->setObjectName(QStringLiteral("hotkeyEditor"));
    editor_->setMaximumSequenceLength(1);
    editor_->setKeySequence(current);
    editor_->setClearButtonEnabled(true);
    title->setBuddy(editor_);
    generalLayout->addWidget(editor_);
    editor_->installEventFilter(this);
    for (QObject* child : editor_->findChildren<QObject*>())
        child->installEventFilter(this);
    auto* help = new QLabel(
        QStringLiteral(
            "支持 F1–F12，或带 Ctrl / Command、Control、Alt / Option 的字母和数字。\n"
            "Mac 的 F1 可能需要配合 Fn / 地球键或系统功能键模式；应用不修改系统键盘设置。\n"
            "窗口打开期间暂停截图快捷键。"),
        generalPage);
    help->setObjectName(QStringLiteral("hotkeyHelp"));
    help->setWordWrap(true);
    generalLayout->addWidget(help);
    auto* saveGroup = new QGroupBox(QStringLiteral("保存"), savePage);
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
    savePageLayout->addWidget(saveGroup);
    savePageLayout->addStretch();
    auto* startupGroup = new QGroupBox(QStringLiteral("启动"), generalPage);
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
    generalLayout->addWidget(startupGroup);
    generalLayout->addStretch();
    const auto ratios = sanitizedRatioSettings(saveSettings_.ratios);
    for (const auto& ratio : ratios.enabled)
        ratioRows_.append({ratio, true});
    for (const auto& ratio : ratios.disabled)
        ratioRows_.append({ratio, false});
    quickRatio_ = ratios.quick;
    auto* ratioGroup = new QGroupBox(QStringLiteral("截图比例"), capturePage);
    auto* ratioLayout = new QVBoxLayout(ratioGroup);
    // 滚动区的最小高度必须传到分组与对话框，防止首次布局压入添加行。
    ratioLayout->setSizeConstraint(QLayout::SetMinimumSize);
    ratioScroll_ = new QScrollArea(ratioGroup);
    ratioScroll_->setWidgetResizable(true);
    // 长列表由滚动区承接，最小高度计算不采用整列内容的建议高度。
    ratioScroll_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
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
    captureLayout->addWidget(ratioGroup);
    shapeTemplates_ = saveSettings_.shapeTemplates;
    auto* shapeGroup = new QGroupBox(QStringLiteral("形状模板"), capturePage);
    auto* shapeLayout = new QVBoxLayout(shapeGroup);
    shapeLayout->setSizeConstraint(QLayout::SetMinimumSize);
    shapeTemplateScroll_ = new QScrollArea(shapeGroup);
    shapeTemplateScroll_->setWidgetResizable(true);
    shapeTemplateScroll_->setMinimumHeight(70);
    shapeTemplateScroll_->setMaximumHeight(110);
    shapeLayout->addWidget(shapeTemplateScroll_);
    shapeTemplateEmptyNotice_ = new QLabel(QStringLiteral("尚未导入任何模板"), shapeGroup);
    shapeTemplateEmptyNotice_->setObjectName(QStringLiteral("shapeTemplateEmptyNotice"));
    shapeLayout->addWidget(shapeTemplateEmptyNotice_);
    auto* import = new QPushButton(QStringLiteral("导入 PNG…"), shapeGroup);
    import->setObjectName(QStringLiteral("shapeTemplateImportButton"));
    shapeLayout->addWidget(import, 0, Qt::AlignLeft);
    auto* shapeHelp =
        new QLabel(QStringLiteral("透明区域即镂空；超过 4096px 自动等比缩小；重名自动加序号。\n"
                                  "启用的模板会出现在形状条；删除在保存后生效。"),
                   shapeGroup);
    shapeHelp->setWordWrap(true);
    shapeLayout->addWidget(shapeHelp);
    if (!saveSettings_.chooseShapeTemplatePaths)
        saveSettings_.chooseShapeTemplatePaths = [](QWidget* parent)
        {
            return QFileDialog::getOpenFileNames(parent, QStringLiteral("导入形状模板"), QString(),
                                                 QStringLiteral("PNG 图片 (*.png)"));
        };
    connect(import, &QPushButton::clicked, this,
            [this]
            {
                const QStringList paths = saveSettings_.chooseShapeTemplatePaths(this);
                if (paths.isEmpty() || !saveSettings_.importShapeTemplates)
                    return;
                const auto result = saveSettings_.importShapeTemplates(paths);
                shapeTemplates_ += result.imported;
                rebuildShapeTemplateList();
                status_->setText(result.error);
                status_->setVisible(!result.error.isEmpty());
            });
    rebuildShapeTemplateList();
    captureLayout->addWidget(shapeGroup);
    captureLayout->addStretch();
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
void SettingsDialog::rebuildShapeTemplateList()
{
    auto* list = new QWidget;
    list->setObjectName(QStringLiteral("shapeTemplateList"));
    auto* layout = new QVBoxLayout(list);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);
    for (int index = 0; index < shapeTemplates_.size(); ++index)
    {
        const auto entry = shapeTemplates_[index];
        auto* row = new QHBoxLayout;
        auto* thumbnail = new QLabel(list);
        thumbnail->setFixedSize(28, 28);
        thumbnail->setPixmap(QPixmap(QDir(saveSettings_.shapeTemplateDir).filePath(entry.file))
                                 .scaled(28, 28, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        row->addWidget(thumbnail);
        auto* name = new QLabel(entry.label, list);
        name->setTextFormat(Qt::PlainText);
        name->setWordWrap(true);
        row->addWidget(name, 1);
        auto* toggle = new QCheckBox(QStringLiteral("启用"), list);
        toggle->setObjectName(QStringLiteral("shapeTemplateToggle%1").arg(index));
        toggle->setChecked(entry.enabled);
        row->addWidget(toggle);
        connect(toggle, &QCheckBox::toggled, this,
                [this, index](bool enabled) { shapeTemplates_[index].enabled = enabled; });
        auto* remove = new QPushButton(QStringLiteral("删除"), list);
        remove->setObjectName(QStringLiteral("shapeTemplateDelete%1").arg(index));
        row->addWidget(remove);
        connect(remove, &QPushButton::clicked, this,
                [this, index]
                {
                    shapeTemplates_.removeAt(index);
                    rebuildShapeTemplateList();
                });
        layout->addLayout(row);
    }
    layout->addStretch();
    // 延迟销毁旧行，避免 clicked 栈内删除发送控件。
    if (QWidget* old = shapeTemplateScroll_->takeWidget())
    {
        old->setParent(nullptr);
        old->hide();
        old->deleteLater();
    }
    shapeTemplateScroll_->setWidget(list);
    shapeTemplateEmptyNotice_->setVisible(shapeTemplates_.isEmpty());
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
        const int height = std::max(
            {600, layout()->minimumSize().height(), layout()->minimumHeightForWidth(width())});
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
void SettingsDialog::buildHistoryPage(QWidget* page)
{
    auto* layout = new QVBoxLayout(page);
    layout->addWidget(new QLabel(QStringLiteral("历史根目录（只切换，不迁移；旧目录保留）"), page));
    auto* row = new QHBoxLayout;
    historyDirectory_ = new QLineEdit(historySettings_.preferences.rootDirectory, page);
    historyDirectory_->setObjectName(QStringLiteral("historyRootDirectory"));
    historyDirectory_->setReadOnly(true);
    historyDirectory_->setPlaceholderText(QStringLiteral("默认 Pictures/WaibuSnap"));
    historyDirectory_->setToolTip(historyDirectory_->text());
    row->addWidget(historyDirectory_, 1);
    auto* choose = new QPushButton(QStringLiteral("选择…"), page);
    choose->setObjectName(QStringLiteral("historyChooseDirectory"));
    auto* clear = new QPushButton(QStringLiteral("清除"), page);
    clear->setObjectName(QStringLiteral("historyClearDirectory"));
    row->addWidget(choose);
    row->addWidget(clear);
    layout->addLayout(row);
    if (!historySettings_.chooseDirectory)
        historySettings_.chooseDirectory = [](QWidget* parent, const QString& current) {
            return QFileDialog::getExistingDirectory(parent, QStringLiteral("选择历史根目录"),
                                                     current);
        };
    connect(choose, &QPushButton::clicked, this,
            [this]
            {
                const auto path = historySettings_.chooseDirectory(this, historyDirectory_->text());
                if (!path.isEmpty())
                {
                    historyDirectory_->setText(QDir(path).absolutePath());
                    historyDirectory_->setToolTip(historyDirectory_->text());
                }
            });
    connect(clear, &QPushButton::clicked, this,
            [this]
            {
                historyDirectory_->clear();
                historyDirectory_->setToolTip({});
            });
    auto* policy = new QGroupBox(QStringLiteral("新建分组的默认清理策略"), page);
    auto* rules = new QVBoxLayout(policy);
    const auto original = historySettings_.preferences.defaultCleanup;
    const std::array<QString, 3> ids = {QStringLiteral("historyAgeEnabled"),
                                        QStringLiteral("historyCountEnabled"),
                                        QStringLiteral("historyBytesEnabled")};
    const std::array<QString, 3> labels = {QStringLiteral("保留时长"), QStringLiteral("保留张数"),
                                           QStringLiteral("存储大小")};
    const std::array<bool, 3> checked = {original.ageEnabled, original.countEnabled,
                                         original.bytesEnabled};
    historyDays_ = new QSpinBox(policy);
    historyDays_->setObjectName(QStringLiteral("historyMaxAgeDays"));
    historyDays_->setRange(1, std::numeric_limits<int>::max());
    historyDays_->setSuffix(QStringLiteral(" 天"));
    historyDays_->setValue(original.maxAgeDays);
    historyCount_ = new QSpinBox(policy);
    historyCount_->setObjectName(QStringLiteral("historyMaxCount"));
    historyCount_->setRange(1, std::numeric_limits<int>::max());
    historyCount_->setSuffix(QStringLiteral(" 张"));
    historyCount_->setValue(original.maxCount);
    historyMiB_ = new QDoubleSpinBox(policy);
    historyMiB_->setObjectName(QStringLiteral("historyMaxMiB"));
    historyMiB_->setDecimals(6);
    historyMiB_->setRange(0.000001, 8589934591.999999);
    historyMiB_->setSuffix(QStringLiteral(" MiB"));
    historyMiB_->setValue(original.maxBytes / 1048576.0);
    originalHistoryMiB_ = historyMiB_->value();
    const std::array<QWidget*, 3> inputs = {historyDays_, historyCount_, historyMiB_};
    for (size_t i = 0; i < ids.size(); ++i)
    {
        auto* rule = new QHBoxLayout;
        auto* check = new QCheckBox(labels[i], policy);
        check->setObjectName(ids[i]);
        check->setChecked(checked[i]);
        inputs[i]->setEnabled(checked[i]);
        historyRules_[i] = check;
        rule->addWidget(check);
        rule->addWidget(inputs[i], 1);
        rules->addLayout(rule);
        connect(check, &QCheckBox::toggled, inputs[i], &QWidget::setEnabled);
    }
    auto* help = new QLabel(
        QStringLiteral("至少启用一项，任一超限即可触发；只影响以后新建的分组。自动清理尚未"
                       "启用；届时只清理受管副本，外部图片不参与。"),
        policy);
    help->setWordWrap(true);
    rules->addWidget(help);
    layout->addWidget(policy);
    const std::array<QString, 3> names = {QStringLiteral("historyPreviousHotkey"),
                                          QStringLiteral("historyNextHotkey"),
                                          QStringLiteral("historyOpenHotkey")};
    for (size_t i = 0; i < historyEditors_.size(); ++i)
    {
        auto* line = new QHBoxLayout;
        auto* label = new QLabel(hotkeyActionName(hotkeyActions[i + 1]), page);
        auto* editor = new QKeySequenceEdit(historySettings_.hotkeys[i], page);
        editor->setObjectName(names[i]);
        editor->setMaximumSequenceLength(1);
        editor->setClearButtonEnabled(true);
        label->setBuddy(editor);
        line->addWidget(label);
        line->addWidget(editor, 1);
        layout->addLayout(line);
        historyEditors_[i] = editor;
        editor->installEventFilter(this);
        for (auto* child : editor->findChildren<QObject*>())
            child->installEventFilter(this);
    }
    auto* usage = new QLabel(page);
    usage->setObjectName(QStringLiteral("historyTotalUsage"));
    usage->setTextFormat(Qt::PlainText);
    usage->setWordWrap(true);
    const auto u = historySettings_.usage;
    usage->setText(QStringLiteral("分组库总占用：%1 MiB\n受管：%2 张 / %3 MiB；外部：%4 张 / %5 "
                                  "MiB\n受保护：%6 张 / %7 MiB")
                       .arg((u.managedBytes + u.externalBytes) / 1048576.0, 0, 'f', 2)
                       .arg(u.managedCount)
                       .arg(u.managedBytes / 1048576.0, 0, 'f', 2)
                       .arg(u.externalCount)
                       .arg(u.externalBytes / 1048576.0, 0, 'f', 2)
                       .arg(u.protectedCount)
                       .arg(u.protectedBytes / 1048576.0, 0, 'f', 2) +
                   (historySettings_.usageNotice.isEmpty()
                        ? QString()
                        : QStringLiteral("\n") + historySettings_.usageNotice));
    layout->addWidget(usage);
    layout->addStretch();
}
HistoryPreferences SettingsDialog::collectHistoryPreferences() const
{
    auto result = historySettings_.preferences;
    result.rootDirectory = historyDirectory_->text();
    auto& policy = result.defaultCleanup;
    policy.enabled = true;
    policy.ageEnabled = historyRules_[0]->isChecked();
    policy.maxAgeDays = historyDays_->value();
    policy.countEnabled = historyRules_[1]->isChecked();
    policy.maxCount = historyCount_->value();
    policy.bytesEnabled = historyRules_[2]->isChecked();
    if (historyMiB_->value() != originalHistoryMiB_)
        policy.maxBytes =
            std::min(qint64(std::llround(historyMiB_->value() * 1048576.0)), 9007199254740991LL);
    return result;
}
void SettingsDialog::save()
{
    const auto sequence = editor_->keySequence();
    const std::array<QKeySequence, 4> sequences = {sequence, historyEditors_[0]->keySequence(),
                                                   historyEditors_[1]->keySequence(),
                                                   historyEditors_[2]->keySequence()};
    QString error = hotkeyConflictError(sequences);
    const auto historyPreferences = collectHistoryPreferences();
    if (error.isEmpty())
        error = cleanupPolicyError(historyPreferences.defaultCleanup);
    if (error.isEmpty() && sequence != originalSequence_)
    {
        error = saveHotkey_(sequence);
        if (error.isEmpty())
            originalSequence_ = sequence;
    }
    for (size_t i = 0; error.isEmpty() && i < historyEditors_.size(); ++i)
        if (sequences[i + 1] != historySettings_.hotkeys[i] && historySettings_.saveHotkey)
        {
            error = historySettings_.saveHotkey(hotkeyActions[i + 1], sequences[i + 1]);
            if (error.isEmpty())
                historySettings_.hotkeys[i] = sequences[i + 1];
        }
    if (error.isEmpty() && saveSettings_.savePreferences)
        error = saveSettings_.savePreferences({directory_->text(), format_->currentIndex() == 1
                                                                       ? ImageFormat::Jpeg
                                                                       : ImageFormat::Png});
    if (error.isEmpty() && autostartCheck_->isChecked() != autostartActions_.query().enabled)
        error = autostartActions_.setEnabled(autostartCheck_->isChecked());
    if (error.isEmpty() && saveSettings_.saveRatios)
        error = saveSettings_.saveRatios(collectRatios());
    if (error.isEmpty() && saveSettings_.saveShapeTemplates)
        error = saveSettings_.saveShapeTemplates(shapeTemplates_);
    if (error.isEmpty() && saveSettings_.saveStickerCloseReminder)
        error = saveSettings_.saveStickerCloseReminder(stickerCloseReminderCheck_->isChecked());
    if (error.isEmpty() && historySettings_.savePreferences)
        error = historySettings_.savePreferences(historyPreferences);
    if (!error.isEmpty())
    {
        status_->setText(error);
        status_->show();
        return;
    }
    accept();
}
}
