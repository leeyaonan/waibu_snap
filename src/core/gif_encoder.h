#pragma once
#include <QImage>
#include <QSize>
#include <QString>
#include <memory>
namespace waibusnap
{
// 同一工作线程内调用；只保留当前帧的量化与LZW工作集，不缓存整段动画。
class GifEncoder final
{
  public:
    GifEncoder();
    ~GifEncoder();
    bool start(const QString& path, QSize size, int fps, QString* error);
    bool addFrame(const QImage& frame, QString* error);
    // 成功后关闭文件；失败保留临时路径供恢复。零帧不生成文件。
    bool finish(QString* error);
    void cancel();
    qint64 frameCount() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
