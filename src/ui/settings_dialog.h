#pragma once
#include <QDialog>
#include <QKeySequence>
#include <functional>
class QKeySequenceEdit;
class QLabel;
namespace waibusnap
{
class SettingsDialog final : public QDialog
{
    Q_OBJECT
  public:
    using SaveHotkey = std::function<QString(const QKeySequence&)>;
    explicit SettingsDialog(const QKeySequence& current, SaveHotkey save,
                            const QString& notice = {}, QWidget* parent = nullptr);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void save();
    QKeySequenceEdit* editor_ = nullptr;
    QLabel* status_ = nullptr;
    SaveHotkey saveHotkey_;
};
}
