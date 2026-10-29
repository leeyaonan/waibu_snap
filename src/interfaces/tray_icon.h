#pragma once
#include <QIcon>
namespace waibusnap
{
// 从内嵌确认稿创建菜单栏 / 托盘图标，包含 1x / 2x 档位与平台模板策略。
enum class TrayIconState
{
    Idle,
    Recording,
    Paused
};
QIcon createTrayIcon(TrayIconState state = TrayIconState::Idle);
}
