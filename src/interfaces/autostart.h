#pragma once
#include <QString>
#include <functional>
#include <memory>
namespace waibusnap
{
struct AutostartState
{
    bool enabled = false;
    bool pendingApproval = false;
    QString notice;
};
class Autostart
{
  public:
    virtual ~Autostart() = default;
    // 操作系统注册为唯一事实来源；查询不注册、不写偏好文件。
    virtual AutostartState query() const = 0;
    // 空字符串表示成功；失败必须保留系统原因，不能回退成伪成功。
    virtual QString setEnabled(bool enabled) = 0;
};
struct AutostartActions
{
    std::function<AutostartState()> query;
    std::function<QString(bool)> setEnabled;
};
std::unique_ptr<Autostart> createAutostart();
}
