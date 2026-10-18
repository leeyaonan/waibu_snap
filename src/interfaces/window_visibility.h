#pragma once
#include <QString>
#include <QtGlobal>
class QWindow;
namespace waibusnap
{
struct WindowVisibility
{
    bool available = false;
    bool onScreen = false;
    quint64 windowNumber = 0;
    int isOnActiveSpace = -1;
    qint64 occlusionState = -1;
    qint64 windowLevel = -1;
    quint64 collectionBehavior = 0;
    int nativeVisible = -1;
    int hidesOnDeactivate = -1;
    QString diagnostic() const
    {
        return QStringLiteral("onScreen=%1 windowNumber=%2 isOnActiveSpace=%3 occlusionState=%4 "
                              "visibilityAvailable=%5 windowLevel=%6 collectionBehavior=%7 "
                              "nativeVisible=%8 hidesOnDeactivate=%9")
            .arg(onScreen)
            .arg(windowNumber)
            .arg(isOnActiveSpace)
            .arg(occlusionState)
            .arg(available)
            .arg(windowLevel)
            .arg(collectionBehavior)
            .arg(nativeVisible)
            .arg(hidesOnDeactivate);
    }
};
// 不创建原生窗口。不可查询与不在屏分别标记；在屏不等于像素已物理呈现。
WindowVisibility queryWindowVisibility(QWindow* window);
}
