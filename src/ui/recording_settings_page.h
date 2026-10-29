#pragma once
#include "interfaces/recording.h"
#include <QWidget>
class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QSlider;
class QSpinBox;
namespace waibusnap
{
struct RecordingSettingsActions
{
    RecordingPreferences preferences;
    bool supported = recordingCaptureSupported();
    std::function<QString(const RecordingPreferences&)> savePreferences;
    std::function<QString(QWidget*, const QString&)> chooseDirectory;
    std::function<QVector<MicrophoneDevice>()> queryMicrophones = microphoneDevices;
    // 保存时只读全部分组登记；不初始化 / 扫描历史。返回分组目录，另回传快枪手目录。
    std::function<QStringList(const QString&, QString*, QString*)> reservedDirectories;
};
class RecordingSettingsPage final : public QWidget
{
    Q_OBJECT
  public:
    explicit RecordingSettingsPage(RecordingSettingsActions actions, QWidget* parent = nullptr);
    RecordingPreferences collect() const;
    QString prepareDirectories(const QString& historyRoot, QStringList groupDirectories);
    bool supported() const { return actions_.supported; }

  protected:
    void showEvent(QShowEvent* event) override;

  private:
    void updateState();
    void refreshMicrophones();
    void setDirectory(bool gif, const QString& path);
    QString directory(bool gif) const;
    RecordingSettingsActions actions_;
    QString videoDirectory_;
    QString gifDirectory_;
    bool devicesRequested_ = false;
    bool refreshing_ = false;
    QRadioButton* video_ = nullptr;
    QRadioButton* gif_ = nullptr;
    QComboBox* resolution_ = nullptr;
    QComboBox* videoFps_ = nullptr;
    QComboBox* quality_ = nullptr;
    QComboBox* gifEdge_ = nullptr;
    QComboBox* gifFps_ = nullptr;
    QComboBox* gifSeconds_ = nullptr;
    QComboBox* countdown_ = nullptr;
    QCheckBox* cursor_ = nullptr;
    QGroupBox* audio_ = nullptr;
    QCheckBox* systemAudio_ = nullptr;
    QCheckBox* microphone_ = nullptr;
    QComboBox* devices_ = nullptr;
    QPushButton* refresh_ = nullptr;
    QSlider* systemSlider_ = nullptr;
    QSlider* microphoneSlider_ = nullptr;
    QSpinBox* systemVolume_ = nullptr;
    QSpinBox* microphoneVolume_ = nullptr;
    QLineEdit* videoPath_ = nullptr;
    QLineEdit* gifPath_ = nullptr;
    QLabel* videoEstimate_ = nullptr;
    QLabel* gifEstimate_ = nullptr;
    QLabel* countdownHelp_ = nullptr;
    QLabel* audioHelp_ = nullptr;
    QLabel* directoryError_ = nullptr;
};
}
