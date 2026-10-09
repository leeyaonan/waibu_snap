#pragma once
#include "interfaces/global_hotkey.h"
#include "output/image_output.h"
#include <QKeySequence>
#include <QString>
namespace waibusnap
{
struct LoadedHotkey
{
    QKeySequence sequence;
    QString explanation;
};
class AppSettings
{
  public:
    explicit AppSettings(QString filePath = {});
    LoadedHotkey loadHotkey() const;
    SavePreferences loadSavePreferences() const;
    QString saveSavePreferences(const SavePreferences& preferences) const;
    // 注册成功后才调用；失败返回中文原因，不声称重启后已保留。
    QString saveHotkey(const QKeySequence& sequence) const;

  private:
    QString filePath_;
};
struct HotkeyStartup
{
    QString report;
    QString warning;
};
// 注册与落盘协调独立于窗口和托盘，允许注入热键替身验证失败路径。
class HotkeySettings
{
  public:
    HotkeySettings(AppSettings settings, GlobalHotkey& hotkey, std::function<void()> handler);
    HotkeyStartup initialize();
    QString changeHotkey(const QKeySequence& sequence);
    bool enabled() const { return !enabledSequence_.isEmpty(); }
    QKeySequence currentSequence() const;
    QString explanation() const { return explanation_; }
    QString tooltip() const;

  private:
    HotkeyRegistration registerSequence(const QKeySequence& sequence);
    AppSettings settings_;
    GlobalHotkey& hotkey_;
    std::function<void()> handler_;
    QKeySequence preferredSequence_;
    QKeySequence enabledSequence_;
    QString explanation_;
};
}
