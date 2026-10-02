#pragma once
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
    // 注册成功后才调用；失败返回中文原因，不声称重启后已保留。
    QString saveHotkey(const QKeySequence& sequence) const;

  private:
    QString filePath_;
};
}
