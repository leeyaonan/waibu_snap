#pragma once
#include "interfaces/global_hotkey.h"
#include <QKeySequence>
#include <QString>
namespace waibusnap
{
QKeySequence defaultScreenshotHotkey();
QKeySequence defaultHotkey(HotkeyAction action);
QString hotkeyActionName(HotkeyAction action);
// 子集沿用动作表前 N 项；生产设置统一校验七项。
QString hotkeyValidationError(const QKeySequence& sequence);
template <size_t N> QString hotkeyConflictError(const std::array<QKeySequence, N>& sequences)
{
    static_assert(N <= hotkeyActions.size());
    for (size_t i = 0; i < sequences.size(); ++i)
    {
        const auto error = hotkeyValidationError(sequences[i]);
        if (!error.isEmpty())
            return hotkeyActionName(hotkeyActions[i]) + QStringLiteral("：") + error;
        for (size_t j = 0; j < i; ++j)
            if (sequences[i] == sequences[j])
                return QStringLiteral("%1与%2快捷键重复，请分别设置。")
                    .arg(hotkeyActionName(hotkeyActions[i]), hotkeyActionName(hotkeyActions[j]));
    }
    return {};
}
// 返回空字符串表示合法；只校验键位，不调用平台、不推断占用状态。
QString hotkeyValidationError(const QKeySequence& sequence);
}
