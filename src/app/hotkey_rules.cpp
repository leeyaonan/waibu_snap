#include "app/hotkey_rules.h"
namespace waibusnap
{
QKeySequence defaultScreenshotHotkey() { return QKeySequence(Qt::Key_F1); }
QString hotkeyValidationError(const QKeySequence& sequence)
{
    if (sequence.isEmpty())
        return QStringLiteral("截图键不能为空，请输入 F1–F12 或带修饰键的字母 / 数字。");
    if (sequence.count() != 1)
        return QStringLiteral("截图键只能包含一个组合，不能使用多段按键序列。");
    const auto combination = sequence[0];
    const auto modifiers = combination.keyboardModifiers();
    const auto supported =
        Qt::ControlModifier | Qt::MetaModifier | Qt::AltModifier | Qt::ShiftModifier;
    if (modifiers & ~supported)
        return QStringLiteral(
            "此修饰键尚不支持，请使用 Ctrl / Command、Control、Alt / Option 或 Shift。");
    const auto key = combination.key();
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12)
        return {};
    if ((key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9))
    {
        if (modifiers & (Qt::ControlModifier | Qt::MetaModifier | Qt::AltModifier))
            return {};
        return QStringLiteral(
            "字母和数字必须带 Ctrl / Command、Control 或 Alt / Option；仅 Shift 不够。");
    }
    return QStringLiteral("此键位不支持。请使用 F1–F12，或带 Ctrl / Command、Control、Alt / Option "
                          "的字母 / 数字；空格、Tab、Esc 不能作为截图键。");
}
}
