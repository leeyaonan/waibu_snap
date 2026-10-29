#include "core/gif_encoder.h"
#include <QDir>
#include <QFile>
#include <algorithm>
#include <array>
#include <vector>
namespace waibusnap
{
namespace
{
constexpr int kBins = 64 * 64 * 64;
int bin(QRgb p) { return (qRed(p) >> 2) * 4096 + (qGreen(p) >> 2) * 64 + (qBlue(p) >> 2); }
struct ColorBin
{
    quint32 count = 0;
    quint64 r = 0, g = 0, b = 0;
};
}
struct GifEncoder::Impl
{
    QFile file;
    QSize size;
    int fps = 0;
    qint64 frames = 0;
    bool failed = false;
    QByteArray output;
    std::vector<ColorBin> histogram = std::vector<ColorBin>(kBins);
    std::array<quint8, kBins> indices{};
    std::array<bool, kBins> mapped{};
    std::vector<int> queue;
    std::array<quint32, 16384> keys{};
    std::array<quint16, 16384> codes{};

    bool flush()
    {
        if (failed || !file.isOpen())
            return false;
        if (file.write(output) != output.size())
            failed = true;
        output.clear();
        return !failed;
    }
    void byte(quint8 value)
    {
        if (failed)
            return;
        output.append(char(value));
        if (output.size() >= 65536)
            flush();
    }
    void word(int value)
    {
        byte(quint8(value));
        byte(quint8(value >> 8));
    }
    void literal(const char* data, int count)
    {
        for (int i = 0; i < count; ++i)
            byte(quint8(data[i]));
    }
    void palette(const QImage& frame)
    {
        std::fill(histogram.begin(), histogram.end(), ColorBin{});
        for (int y = 0; y < frame.height(); ++y)
        {
            const auto* row = reinterpret_cast<const QRgb*>(frame.constScanLine(y));
            for (int x = 0; x < frame.width(); ++x)
            {
                const QRgb p = row[x];
                auto& h = histogram[size_t(bin(p))];
                ++h.count;
                h.r += qRed(p);
                h.g += qGreen(p);
                h.b += qBlue(p);
            }
        }
        queue.clear();
        for (int i = 0; i < kBins; ++i)
            if (histogram[size_t(i)].count)
                queue.push_back(i);
        const auto compare = [this](int a, int b)
        {
            const auto ca = histogram[size_t(a)].count, cb = histogram[size_t(b)].count;
            // 同频bin用稳定散列打破并列，避免仅保留低RGB索引导致整段色域丢失。
            const auto rank = [](quint32 value)
            {
                value ^= value >> 16;
                value *= 0x7feb352du;
                value ^= value >> 15;
                value *= 0x846ca68bu;
                return value ^ (value >> 16);
            };
            return ca == cb ? rank(quint32(a)) < rank(quint32(b)) : ca > cb;
        };
        const size_t colors = std::min(size_t(256), queue.size());
        if (queue.size() > colors)
            std::nth_element(queue.begin(), queue.begin() + qsizetype(colors), queue.end(),
                             compare);
        queue.resize(colors);
        std::sort(queue.begin(), queue.end(), compare);
        mapped.fill(false);
        for (size_t i = 0; i < 256; ++i)
        {
            if (i < colors)
            {
                const int cell = queue[i];
                const auto& h = histogram[size_t(cell)];
                byte(quint8(h.r / h.count));
                byte(quint8(h.g / h.count));
                byte(quint8(h.b / h.count));
                indices[size_t(cell)] = quint8(i);
                mapped[size_t(cell)] = true;
            }
            else
            {
                byte(0);
                byte(0);
                byte(0);
            }
        }
        // 从256个种子扩散一次，生成完整bin→index表；每像素仅一次查表。
        // 固定64³空间，避免全像素逐一搜索256色。
        for (size_t head = 0; head < queue.size(); ++head)
        {
            const int cell = queue[head];
            const int r = cell >> 12, g = (cell >> 6) & 63, b = cell & 63;
            const auto visit = [&](int neighbor)
            {
                if (!mapped[size_t(neighbor)])
                {
                    mapped[size_t(neighbor)] = true;
                    indices[size_t(neighbor)] = indices[size_t(cell)];
                    queue.push_back(neighbor);
                }
            };
            if (r)
                visit(cell - 4096);
            if (r < 63)
                visit(cell + 4096);
            if (g)
                visit(cell - 64);
            if (g < 63)
                visit(cell + 64);
            if (b)
                visit(cell - 1);
            if (b < 63)
                visit(cell + 1);
        }
    }
    void lzw(const QImage& frame)
    {
        byte(8);
        std::array<quint8, 255> block{};
        int used = 0, bitCount = 0, bits = 9, next = 258;
        quint32 packed = 0;
        const auto put = [&](quint8 value)
        {
            block[size_t(used++)] = value;
            if (used == 255)
            {
                byte(255);
                for (auto v : block)
                    byte(v);
                used = 0;
            }
        };
        const auto emitCode = [&](int code)
        {
            packed |= quint32(code) << bitCount;
            bitCount += bits;
            while (bitCount >= 8)
            {
                put(quint8(packed));
                packed >>= 8;
                bitCount -= 8;
            }
        };
        const auto grow = [&]
        {
            // 解码器比编码器晚建立一个词条，位宽在输出后、插入前增长。
            if (next == (1 << bits) && bits < 12)
                ++bits;
        };
        keys.fill(0);
        emitCode(256);
        int prefix = -1;
        for (int y = 0; y < frame.height(); ++y)
        {
            const auto* row = reinterpret_cast<const QRgb*>(frame.constScanLine(y));
            for (int x = 0; x < frame.width(); ++x)
            {
                const int value = indices[size_t(bin(row[x]))];
                if (prefix < 0)
                {
                    prefix = value;
                    continue;
                }
                const quint32 key = (quint32(prefix) << 8) | quint32(value);
                size_t slot = size_t((key * 2654435761u) >> 18);
                while (keys[slot] && keys[slot] != key + 1)
                    slot = (slot + 1) & 16383;
                if (keys[slot])
                {
                    prefix = codes[slot];
                    continue;
                }
                emitCode(prefix);
                grow();
                if (next < 4096)
                {
                    keys[slot] = key + 1;
                    codes[slot] = quint16(next++);
                }
                else
                {
                    emitCode(256);
                    keys.fill(0);
                    bits = 9;
                    next = 258;
                }
                prefix = value;
            }
        }
        if (prefix >= 0)
        {
            emitCode(prefix);
            grow();
        }
        emitCode(257);
        if (bitCount)
            put(quint8(packed));
        if (used)
        {
            byte(quint8(used));
            for (int i = 0; i < used; ++i)
                byte(block[size_t(i)]);
        }
        byte(0);
    }
};
GifEncoder::GifEncoder() : impl_(std::make_unique<Impl>()) {}
GifEncoder::~GifEncoder()
{
    if (impl_->file.isOpen())
        cancel();
}
bool GifEncoder::start(const QString& path, QSize size, int fps, QString* error)
{
    auto reject = [error](const QString& text)
    {
        if (error)
            *error = text;
        return false;
    };
    if (impl_->file.isOpen())
        return reject(QStringLiteral("GIF编码器已启动。"));
    if (!size.isValid() || size.isEmpty() || size.width() > 65535 || size.height() > 65535)
        return reject(QStringLiteral("GIF尺寸必须在1–65535之间。"));
    if (fps != 10 && fps != 12 && fps != 15)
        return reject(QStringLiteral("GIF帧率只支持10 / 12 / 15。"));
    if (path.isEmpty() || !QDir::isAbsolutePath(path))
        return reject(QStringLiteral("GIF路径必须为绝对路径。"));
    impl_->file.setFileName(path);
    if (!impl_->file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return reject(QStringLiteral("无法创建GIF文件：%1").arg(impl_->file.errorString()));
    impl_->size = size;
    impl_->fps = fps;
    impl_->frames = 0;
    impl_->failed = false;
    impl_->output.clear();
    impl_->output.reserve(65536);
    impl_->queue.reserve(kBins);
    impl_->literal("GIF89a", 6);
    impl_->word(size.width());
    impl_->word(size.height());
    impl_->byte(0x70);
    impl_->byte(0);
    impl_->byte(0);
    impl_->literal("\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00", 19);
    return true;
}
bool GifEncoder::addFrame(const QImage& image, QString* error)
{
    if (!impl_->file.isOpen() || impl_->failed || image.isNull() || image.size() != impl_->size)
    {
        if (error)
            *error = QStringLiteral("GIF帧无效、尺寸不一致或写入器不可用。");
        return false;
    }
    const QImage frame = image.convertToFormat(QImage::Format_RGB32);
    if (frame.isNull())
    {
        if (error)
            *error = QStringLiteral("GIF帧格式转换失败。");
        return false;
    }
    impl_->literal("\x21\xf9\x04\x04", 4);
    const qint64 n = impl_->frames;
    impl_->word(int(((n + 1) * 100 + impl_->fps / 2) / impl_->fps -
                    (n * 100 + impl_->fps / 2) / impl_->fps));
    impl_->word(0);
    impl_->byte(0x2c);
    impl_->word(0);
    impl_->word(0);
    impl_->word(frame.width());
    impl_->word(frame.height());
    impl_->byte(0x87);
    impl_->palette(frame);
    impl_->lzw(frame);
    if (!impl_->flush())
    {
        if (error)
            *error = QStringLiteral("GIF逐帧写入失败：%1").arg(impl_->file.errorString());
        return false;
    }
    ++impl_->frames;
    return true;
}
bool GifEncoder::finish(QString* error)
{
    if (!impl_->file.isOpen() || !impl_->frames)
    {
        cancel();
        if (error)
            *error = QStringLiteral("GIF没有任何画面，未生成文件。");
        return false;
    }
    impl_->byte(0x3b);
    const bool ok = impl_->flush() && impl_->file.flush();
    if (!ok && error)
        *error = QStringLiteral("GIF收尾失败，已保留可恢复文件：%1").arg(impl_->file.fileName());
    impl_->file.close();
    return ok;
}
void GifEncoder::cancel()
{
    if (impl_->file.isOpen())
    {
        impl_->file.close();
        impl_->file.remove();
    }
    impl_->output.clear();
}
qint64 GifEncoder::frameCount() const { return impl_->frames; }
}
