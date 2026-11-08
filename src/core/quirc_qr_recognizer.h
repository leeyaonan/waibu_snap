#pragma once
#include "interfaces/qr_recognizer.h"
namespace waibusnap
{
// Windows 生产环境使用；macOS 生产环境用 Vision，此实现仅由真实引擎测试交叉验证。
std::unique_ptr<QrRecognizer> createQuircQrRecognizer();
}
