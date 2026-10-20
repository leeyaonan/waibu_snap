#pragma once
#include "interfaces/global_hotkey.h"
#include <QKeySequence>
#include <QString>
namespace waibusnap
{
QKeySequence defaultScreenshotHotkey();
QKeySequence defaultHotkey(HotkeyAction action);
QString hotkeyActionName(HotkeyAction action);
QString hotkeyConflictError(const std::array<QKeySequence, 4>& sequences);
// 返回空字符串表示合法；只校验键位，不调用平台、不推断占用状态。
QString hotkeyValidationError(const QKeySequence& sequence);
}
