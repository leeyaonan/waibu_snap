#pragma once
#include "app/history_manager.h"
#include <QDialog>
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;
namespace waibusnap
{
// 取消返回空值；删除失败仍由调用方在原对话框 / 窗口展示并允许重试。
std::optional<HistoryStatus> confirmHistoryGroupDeletion(HistoryManager& manager,
                                                         const QString& groupId,
                                                         const HistoryTrash& trash,
                                                         QWidget* parent);
class GroupSettingsDialog final : public QDialog
{
    Q_OBJECT
  public:
    GroupSettingsDialog(HistoryManager& manager, QString groupId, HistoryTrash trash,
                        QString selectedId = {}, QWidget* parent = nullptr);

  private:
    CleanupPolicy policy() const;
    void setPolicy(const CleanupPolicy& policy);
    void updateUsage();
    void syncCurrentGroup();
    HistoryManager& manager_;
    QString groupId_;
    QString selectedId_;
    HistoryTrash trash_;
    QCheckBox* enabled_;
    QCheckBox* age_;
    QCheckBox* count_;
    QCheckBox* bytes_;
    QCheckBox* current_;
    QSpinBox* days_;
    QSpinBox* maxCount_;
    QDoubleSpinBox* maxMiB_;
    QLabel* usage_;
    QLabel* error_;
    QPushButton* save_;
    qint64 originalBytes_ = 0;
    double originalMiB_ = 0;
};
}
