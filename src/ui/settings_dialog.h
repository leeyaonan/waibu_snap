#pragma once
#include "output/image_output.h"
#include <QDialog>
#include <QKeySequence>
#include <functional>
class QComboBox;
class QLineEdit;
class QKeySequenceEdit;
class QLabel;
namespace waibusnap
{
struct SaveSettingsActions
{
    SavePreferences preferences;
    std::function<QString(const SavePreferences&)> savePreferences;
    std::function<QString(QWidget*, const QString&)> chooseDirectory;
};
class SettingsDialog final : public QDialog
{
    Q_OBJECT
  public:
    using SaveHotkey = std::function<QString(const QKeySequence&)>;
    explicit SettingsDialog(const QKeySequence& current, SaveHotkey save,
                            const QString& notice = {}, QWidget* parent = nullptr,
                            SaveSettingsActions saveSettings = {});

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void save();
    QKeySequenceEdit* editor_ = nullptr;
    QLabel* status_ = nullptr;
    SaveHotkey saveHotkey_;
    QKeySequence originalSequence_;
    SaveSettingsActions saveSettings_;
    QLineEdit* directory_ = nullptr;
    QComboBox* format_ = nullptr;
};
}
