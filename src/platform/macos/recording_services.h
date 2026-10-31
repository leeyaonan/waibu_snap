#pragma once
#include "interfaces/recording.h"

namespace waibusnap
{
// 只读查询并缓存；硬件与软件 H.264 编码器均可，不创建压缩会话。
bool macH264EncoderAvailable();
// 平台私有注入点；测试可在无录屏权限的环境验证编码器不可用的真实失败收口。
std::unique_ptr<RecordingSession>
createMacRecordingSession(std::function<bool()> h264EncoderAvailable);
}
