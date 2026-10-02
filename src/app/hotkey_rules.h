#pragma once
#include <QKeySequence>
#include <QString>
namespace waibusnap
{
QKeySequence defaultScreenshotHotkey();
// 返回空字符串表示合法；只校验键位，不调用平台、不推断占用状态。
QString hotkeyValidationError(const QKeySequence& sequence);
}
