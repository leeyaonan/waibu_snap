#include "ui/group_settings_dialog.h"
#include <QCheckBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <limits>
namespace waibusnap
{
std::optional<HistoryStatus> confirmHistoryGroupDeletion(HistoryManager& manager,
                                                         const QString& groupId,
                                                         const HistoryTrash& trash, QWidget* parent)
{
    const auto snapshot = manager.refresh();
    for (const auto& group : snapshot.groups)
        if (group.group.id == groupId)
        {
            if (group.group.isDefault)
                return HistoryStatus{false, QStringLiteral("默认分组不能删除，可以改名。")};
            QPointer<QMessageBox> box =
                new QMessageBox(QMessageBox::Warning, QStringLiteral("删除分组"),
                                QStringLiteral("将分组“%1”移入系统回收站？\n外部图片：%2 "
                                               "张。\n整个文件夹及其中所有文件都会移入回收站。")
                                    .arg(group.group.name)
                                    .arg(group.usage.externalCount),
                                QMessageBox::NoButton, parent);
            box->setObjectName(QStringLiteral("historyDeleteGroupConfirmation"));
            auto* remove =
                box->addButton(QStringLiteral("移入回收站"), QMessageBox::DestructiveRole);
            auto* cancel = box->addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
            box->setDefaultButton(cancel);
            box->setEscapeButton(cancel);
            box->exec();
            const bool confirmed = box && box->clickedButton() == remove;
            delete box.data();
            if (!confirmed)
                return {};
            return manager.deleteGroup(groupId, trash);
        }
    return HistoryStatus{false, QStringLiteral("分组已不存在，请刷新历史。")};
}
GroupSettingsDialog::GroupSettingsDialog(HistoryManager& manager, QString groupId,
                                         HistoryTrash trash, QString selectedId, QWidget* parent)
    : QDialog(parent), manager_(manager), groupId_(std::move(groupId)),
      selectedId_(std::move(selectedId)), trash_(std::move(trash))
{
    setObjectName(QStringLiteral("groupSettingsDialog"));
    setWindowTitle(QStringLiteral("分组设置"));
    setMinimumWidth(360);
    auto* layout = new QVBoxLayout(this);
    auto* name = new QLabel(this);
    name->setWordWrap(true);
    layout->addWidget(name);
    enabled_ = new QCheckBox(QStringLiteral("自动清理"), this);
    enabled_->setObjectName(QStringLiteral("groupCleanupEnabled"));
    layout->addWidget(enabled_);
    const auto rule = [this, layout](const char* id, const QString& text)
    {
        auto* row = new QHBoxLayout;
        auto* check = new QCheckBox(text, this);
        check->setObjectName(QLatin1String(id));
        row->addWidget(check);
        layout->addLayout(row);
        return std::make_pair(check, row);
    };
    auto age = rule("groupAgeEnabled", QStringLiteral("保留时长"));
    age_ = age.first;
    days_ = new QSpinBox(this);
    days_->setObjectName(QStringLiteral("groupMaxAgeDays"));
    days_->setRange(1, std::numeric_limits<int>::max());
    days_->setSuffix(QStringLiteral(" 天"));
    age.second->addWidget(days_);
    auto count = rule("groupCountEnabled", QStringLiteral("保留张数"));
    count_ = count.first;
    maxCount_ = new QSpinBox(this);
    maxCount_->setObjectName(QStringLiteral("groupMaxCount"));
    maxCount_->setRange(1, std::numeric_limits<int>::max());
    maxCount_->setSuffix(QStringLiteral(" 张"));
    count.second->addWidget(maxCount_);
    auto bytes = rule("groupBytesEnabled", QStringLiteral("存储大小"));
    bytes_ = bytes.first;
    maxMiB_ = new QDoubleSpinBox(this);
    maxMiB_->setObjectName(QStringLiteral("groupMaxMiB"));
    maxMiB_->setDecimals(6);
    maxMiB_->setRange(0.000001, 8589934591.999999);
    maxMiB_->setSuffix(QStringLiteral(" MiB"));
    bytes.second->addWidget(maxMiB_);
    auto* explanation = new QLabel(
        QStringLiteral(
            "自动清理会永久删除应用生成的历史副本。任一规则超限即可触发。星标、最新截图、正在查看及"
            "单张超大图片受保护；外部图片不自动清理。"),
        this);
    explanation->setObjectName(QStringLiteral("groupCleanupExplanation"));
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    auto* restore = new QPushButton(QStringLiteral("恢复全局默认"), this);
    restore->setObjectName(QStringLiteral("groupRestoreDefaults"));
    layout->addWidget(restore);
    usage_ = new QLabel(this);
    usage_->setObjectName(QStringLiteral("groupUsage"));
    usage_->setWordWrap(true);
    layout->addWidget(usage_);
    current_ = new QCheckBox(QStringLiteral("设为当前分组（保存后生效）"), this);
    current_->setObjectName(QStringLiteral("groupMakeCurrent"));
    layout->addWidget(current_);
    auto* remove = new QPushButton(QStringLiteral("删除分组…"), this);
    remove->setObjectName(QStringLiteral("groupDelete"));
    layout->addWidget(remove);
    error_ = new QLabel(this);
    error_->setObjectName(QStringLiteral("groupSettingsError"));
    error_->setWordWrap(true);
    layout->addWidget(error_);
    auto* buttons = new QDialogButtonBox(this);
    save_ = buttons->addButton(QStringLiteral("保存"), QDialogButtonBox::AcceptRole);
    save_->setObjectName(QStringLiteral("groupSettingsSave"));
    buttons->addButton(QStringLiteral("取消"), QDialogButtonBox::RejectRole)
        ->setObjectName(QStringLiteral("groupSettingsCancel"));
    layout->addWidget(buttons);
    bool found = false;
    for (const auto& group : manager_.refresh().groups)
        if (group.group.id == groupId_)
        {
            found = true;
            name->setText(group.group.name);
            setPolicy(group.group.cleanup);
            remove->setEnabled(!group.group.isDefault && bool(trash_));
        }
    save_->setEnabled(found);
    if (!found)
    {
        remove->setEnabled(false);
        error_->setText(QStringLiteral("分组已不存在，请刷新历史。"));
    }
    syncCurrentGroup();
    connect(&manager_, &HistoryManager::currentGroupChanged, this,
            &GroupSettingsDialog::syncCurrentGroup);
    connect(&manager_, &HistoryManager::groupsChanged, this, &GroupSettingsDialog::updateUsage);
    connect(restore, &QPushButton::clicked, this,
            [this] { setPolicy(manager_.defaultCleanupPolicy()); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, this,
            [this]
            {
                const auto result =
                    manager_.saveCleanupPolicy(groupId_, policy(), current_->isChecked());
                if (result.success)
                    accept();
                else
                    error_->setText(result.error);
            });
    connect(remove, &QPushButton::clicked, this,
            [this]
            {
                const auto result = confirmHistoryGroupDeletion(manager_, groupId_, trash_, this);
                if (!result)
                    return;
                if (result->success)
                    accept();
                else
                    error_->setText(result->error);
            });
    const auto changed = [this] { updateUsage(); };
    for (auto* check : {enabled_, age_, count_, bytes_})
        connect(check, &QCheckBox::toggled, this, changed);
    connect(days_, &QSpinBox::valueChanged, this, changed);
    connect(maxCount_, &QSpinBox::valueChanged, this, changed);
    connect(maxMiB_, &QDoubleSpinBox::valueChanged, this, changed);
    updateUsage();
}
CleanupPolicy GroupSettingsDialog::policy() const
{
    // 未修改大小时保留原始字节值，避免显示单位换算改变已有策略。
    const qint64 bytes =
        maxMiB_->value() == originalMiB_
            ? originalBytes_
            : std::min(qint64(std::llround(maxMiB_->value() * 1048576.0)), 9007199254740991LL);
    return {enabled_->isChecked(), age_->isChecked(),   days_->value(), count_->isChecked(),
            maxCount_->value(),    bytes_->isChecked(), bytes};
}
void GroupSettingsDialog::setPolicy(const CleanupPolicy& policy)
{
    const QSignalBlocker a(enabled_), b(age_), c(count_), d(bytes_), e(days_), f(maxCount_),
        g(maxMiB_);
    enabled_->setChecked(policy.enabled);
    age_->setChecked(policy.ageEnabled);
    count_->setChecked(policy.countEnabled);
    bytes_->setChecked(policy.bytesEnabled);
    days_->setValue(policy.maxAgeDays);
    maxCount_->setValue(policy.maxCount);
    maxMiB_->setValue(policy.maxBytes / 1048576.0);
    originalBytes_ = policy.maxBytes;
    originalMiB_ = maxMiB_->value();
    updateUsage();
}
void GroupSettingsDialog::syncCurrentGroup()
{
    const bool current = manager_.currentGroupId() == groupId_;
    current_->setChecked(current);
    current_->setEnabled(!current);
}
void GroupSettingsDialog::updateUsage()
{
    const auto draft = policy();
    age_->setEnabled(draft.enabled);
    count_->setEnabled(draft.enabled);
    bytes_->setEnabled(draft.enabled);
    days_->setEnabled(draft.enabled && draft.ageEnabled);
    maxCount_->setEnabled(draft.enabled && draft.countEnabled);
    maxMiB_->setEnabled(draft.enabled && draft.bytesEnabled);
    error_->setText(cleanupPolicyError(draft));
    for (const auto& group : manager_.snapshot().groups)
        if (group.group.id == groupId_)
        {
            auto measuring = draft;
            measuring.enabled = false;
            const auto protection = selectCleanupCandidates(
                measuring, group.items, QDateTime::currentMSecsSinceEpoch(), selectedId_);
            usage_->setText(
                QStringLiteral("当前占用：%1 张 / %2 MiB\n外部图片：%3 张 / %4 MiB\n受保护占用：%5 "
                               "张 / %6 MiB")
                    .arg(group.items.size())
                    .arg((group.usage.managedBytes + group.usage.externalBytes) / 1048576.0, 0, 'f',
                         2)
                    .arg(group.usage.externalCount)
                    .arg(group.usage.externalBytes / 1048576.0, 0, 'f', 2)
                    .arg(protection.protectedCount)
                    .arg(protection.protectedBytes / 1048576.0, 0, 'f', 2));
        }
}
}
