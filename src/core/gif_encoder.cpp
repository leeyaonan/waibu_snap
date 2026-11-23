#include "core/gif_encoder.h"
#include <QDir>
#include <QFile>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
namespace waibusnap
{
namespace
{
constexpr int kBins = 64 * 64 * 64;
constexpr int kPaletteColors = 256;
int bin(QRgb p) { return (qRed(p) >> 2) * 4096 + (qGreen(p) >> 2) * 64 + (qBlue(p) >> 2); }
struct ColorBin
{
    quint32 count = 0;
    quint64 r = 0, g = 0, b = 0;
};
// 非空颜色桶：6-bit 桶索引、桶内均值与 OkLab 坐标。
// 切分与选色都在感知空间进行，避免小面积高饱和色被大面积近似色挤掉。
struct QuantBin
{
    int cell = 0;
    quint32 count = 0;
    float r = 0.0F, g = 0.0F, b = 0.0F;
    float lightness = 0.0F, chromaA = 0.0F, chromaB = 0.0F;
};
// 中位切分的当前盒子，只记 QuantBin 上的区间；cut 为该盒加权平方误差的最大轴。
struct QuantBox
{
    int start = 0;
    int length = 0;
    int axis = 0;
    float cut = 0.0F;
};
float toLinear(float value)
{
    return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}
// sRGB(0–255) → OkLab。感知空间里红色与蓝色的距离远大于两者各自与相邻色的距离，
// 因此按误差切分时小面积色会被单独切成一个盒子，而不是按像素数被淘汰。
void toOklab(float r, float g, float b, float& lightness, float& chromaA, float& chromaB)
{
    const float lr = toLinear(r / 255.0F);
    const float lg = toLinear(g / 255.0F);
    const float lb = toLinear(b / 255.0F);
    const float l = std::cbrt(0.4122214708F * lr + 0.5363325363F * lg + 0.0514459929F * lb);
    const float m = std::cbrt(0.2119034982F * lr + 0.6806995451F * lg + 0.1073969566F * lb);
    const float s = std::cbrt(0.0883024619F * lr + 0.2817188376F * lg + 0.6299787005F * lb);
    lightness = 0.2104542553F * l + 0.7936177850F * m - 0.0040720468F * s;
    chromaA = 1.9779984951F * l - 2.4285922050F * m + 0.4505937099F * s;
    chromaB = 0.0259040371F * l + 0.7827717662F * m - 0.8086757660F * s;
}
} // namespace
struct GifEncoder::Impl
{
    QFile file;
    QSize size;
    int fps = 0;
    qint64 frames = 0;
    bool failed = false;
    QByteArray output;
    std::vector<ColorBin> histogram = std::vector<ColorBin>(kBins);
    std::vector<QuantBin> colors;
    std::vector<QuantBox> boxes;
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
    // 统计当前帧的 6-bit 直方图，并整理成带 OkLab 坐标的非空桶列表。
    void collect(const QImage& frame)
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
        colors.clear();
        for (int i = 0; i < kBins; ++i)
        {
            const auto& h = histogram[size_t(i)];
            if (!h.count)
                continue;
            QuantBin item;
            item.cell = i;
            item.count = h.count;
            item.r = float(h.r) / float(h.count);
            item.g = float(h.g) / float(h.count);
            item.b = float(h.b) / float(h.count);
            toOklab(item.r, item.g, item.b, item.lightness, item.chromaA, item.chromaB);
            colors.push_back(item);
        }
    }
    // 重算盒子的加权均值与加权平方误差，误差最大的轴作为下一步切分方向。
    void updateBox(size_t index)
    {
        QuantBox& box = boxes[index];
        quint64 weight = 0;
        double sumL = 0.0, sumA = 0.0, sumB = 0.0;
        for (int i = box.start; i < box.start + box.length; ++i)
        {
            const QuantBin& item = colors[size_t(i)];
            weight += item.count;
            sumL += double(item.lightness) * item.count;
            sumA += double(item.chromaA) * item.count;
            sumB += double(item.chromaB) * item.count;
        }
        if (!weight)
        {
            box.cut = 0.0F;
            return;
        }
        const double meanL = sumL / double(weight);
        const double meanA = sumA / double(weight);
        const double meanB = sumB / double(weight);
        double error[3] = {0.0, 0.0, 0.0};
        for (int i = box.start; i < box.start + box.length; ++i)
        {
            const QuantBin& item = colors[size_t(i)];
            const double dL = double(item.lightness) - meanL;
            const double dA = double(item.chromaA) - meanA;
            const double dB = double(item.chromaB) - meanB;
            error[0] += dL * dL * item.count;
            error[1] += dA * dA * item.count;
            error[2] += dB * dB * item.count;
        }
        box.axis =
            error[0] >= error[1] ? (error[0] >= error[2] ? 0 : 2) : (error[1] >= error[2] ? 1 : 2);
        box.cut = float(std::max({error[0], error[1], error[2]}));
    }
    // 沿误差最大的轴按加权中位数二分盒子，保证两侧都至少保留一个桶。
    void split(size_t index)
    {
        const QuantBox box = boxes[index];
        const int axis = box.axis;
        const auto first = colors.begin() + box.start;
        std::sort(first, first + box.length,
                  [axis](const QuantBin& left, const QuantBin& right)
                  {
                      const float a =
                          axis == 0 ? left.lightness : (axis == 1 ? left.chromaA : left.chromaB);
                      const float b =
                          axis == 0 ? right.lightness : (axis == 1 ? right.chromaA : right.chromaB);
                      return a < b;
                  });
        quint64 total = 0;
        for (int i = box.start; i < box.start + box.length; ++i)
            total += colors[size_t(i)].count;
        quint64 accumulated = 0;
        int cut = box.start + box.length - 1;
        for (int i = box.start; i < box.start + box.length; ++i)
        {
            accumulated += colors[size_t(i)].count;
            if (accumulated * 2 >= total)
            {
                cut = std::min(std::max(i, box.start + 1), box.start + box.length - 1);
                break;
            }
        }
        boxes[index] = QuantBox{box.start, cut - box.start, 0, 0.0F};
        boxes.push_back(QuantBox{cut, box.start + box.length - cut, 0, 0.0F});
        updateBox(index);
        updateBox(boxes.size() - 1);
    }
    // 逐帧局部色表：OkLab 加权中位切分选 256 色，再按盒子归属直接建立桶→索引映射。
    void palette(const QImage& frame)
    {
        collect(frame);
        boxes.clear();
        mapped.fill(false);
        queue.clear();
        if (colors.empty())
        {
            for (int i = 0; i < kPaletteColors; ++i)
            {
                byte(0);
                byte(0);
                byte(0);
            }
            return;
        }
        boxes.push_back(QuantBox{0, int(colors.size()), 0, 0.0F});
        updateBox(0);
        while (int(boxes.size()) < kPaletteColors)
        {
            size_t best = boxes.size();
            float bestCut = 0.0F;
            for (size_t i = 0; i < boxes.size(); ++i)
                if (boxes[i].length > 1 && boxes[i].cut > bestCut)
                {
                    bestCut = boxes[i].cut;
                    best = i;
                }
            if (best == boxes.size())
                break;
            split(best);
        }
        for (size_t index = 0; index < boxes.size(); ++index)
        {
            const QuantBox& box = boxes[index];
            quint64 weight = 0;
            double sumR = 0.0, sumG = 0.0, sumB = 0.0;
            for (int i = box.start; i < box.start + box.length; ++i)
            {
                const QuantBin& item = colors[size_t(i)];
                weight += item.count;
                sumR += double(item.r) * item.count;
                sumG += double(item.g) * item.count;
                sumB += double(item.b) * item.count;
            }
            const double divisor = weight ? double(weight) : 1.0;
            byte(quint8(sumR / divisor));
            byte(quint8(sumG / divisor));
            byte(quint8(sumB / divisor));
            for (int i = box.start; i < box.start + box.length; ++i)
            {
                const int cell = colors[size_t(i)].cell;
                indices[size_t(cell)] = quint8(index);
                mapped[size_t(cell)] = true;
                queue.push_back(cell);
            }
        }
        for (size_t index = boxes.size(); index < size_t(kPaletteColors); ++index)
        {
            byte(0);
            byte(0);
            byte(0);
        }
        // 当前帧未出现的桶从已映射桶向外扩散，保证每像素一次查表即可取到索引。
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
GifEncoder::GifEncoder() : impl_(std::make_unique<Impl>())
{
    impl_->colors.reserve(4096);
    impl_->boxes.reserve(kPaletteColors);
    impl_->queue.reserve(kBins);
}
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
} // namespace waibusnap
