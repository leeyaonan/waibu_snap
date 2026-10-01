#pragma once
#include <QtGlobal>
#include <chrono>
namespace waibusnap
{
inline qint64 monotonicNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}
