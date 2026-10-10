#include "core/text_layout.h"
#include <QTextBoundaryFinder>
#include <algorithm>
#include <cmath>
#include <limits>
namespace waibusnap::text_layout
{
namespace
{
bool isCjk(char32_t c)
{
    return (c >= 0x3400 && c <= 0x9fff) || (c >= 0x20000 && c <= 0x323af) ||
           (c >= 0xf900 && c <= 0xfaff);
}
}
QVector<TextSpan> tokenizeLine(const QString& text)
{
    QVector<TextSpan> spans;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    int begin = 0;
    int previousKind = 0;
    while (begin < text.size())
    {
        finder.setPosition(begin);
        const int end = finder.toNextBoundary();
        if (end <= begin)
            break;
        const char32_t c = text[begin].isHighSurrogate() && begin + 1 < text.size()
                               ? QChar::surrogateToUcs4(text[begin], text[begin + 1])
                               : text[begin].unicode();
        const int kind = isCjk(c)                                                 ? 1
                         : QChar::isLetterOrNumber(c) || c == '\'' || c == 0x2019 ? 2
                         : QChar::category(c) == QChar::Symbol_Other              ? 3
                                                                                  : 0;
        if (kind != 0)
        {
            if (kind == previousKind && kind != 3 && !spans.isEmpty())
                spans.last().length += end - begin;
            else
                spans.append({begin, end - begin});
        }
        previousKind = kind;
        begin = end;
    }
    return spans;
}
QString assembleFullText(const QVector<RecognizedLine>& lines)
{
    QVector<qreal> heights;
    for (const auto& line : lines)
        if (line.box.height() > 0)
            heights.append(line.box.height());
    std::sort(heights.begin(), heights.end());
    qreal median = 0;
    if (!heights.isEmpty())
        median = (heights[(heights.size() - 1) / 2] + heights[heights.size() / 2]) / 2;
    QString text;
    for (int i = 0; i < lines.size(); ++i)
    {
        if (i > 0)
        {
            text += '\n';
            if (median > 0 &&
                lines[i].box.top() - lines[i - 1].box.bottom() > paragraphGapFactor * median)
                text += '\n';
        }
        text += lines[i].text;
    }
    return text;
}
QVector<RecognizedToken> selectableTokens(const RecognizedLine& line)
{
    if (!line.tokens.isEmpty())
        return line.tokens;
    if (line.text.isEmpty())
        return {};
    return {{line.text, line.box, 0, int(line.text.size())}};
}
int nearestToken(const RecognizedLine& line, qreal x)
{
    const auto tokens = selectableTokens(line);
    int nearest = -1;
    qreal bestEdge = std::numeric_limits<qreal>::max();
    qreal bestCenter = bestEdge;
    for (int i = 0; i < tokens.size(); ++i)
    {
        const auto& box = tokens[i].box;
        const qreal edge = std::max({box.left() - x, x - box.right(), qreal(0)});
        const qreal center = std::abs(box.center().x() - x);
        if (edge < bestEdge || (edge == bestEdge && center < bestCenter))
        {
            nearest = i;
            bestEdge = edge;
            bestCenter = center;
        }
    }
    return nearest;
}
QVector<SelectionRange> selectionRange(const QVector<RecognizedLine>& lines, TokenPosition anchor,
                                       TokenPosition current)
{
    const auto valid = [&lines](TokenPosition p) {
        return p.valid() && p.line < lines.size() &&
               p.token < selectableTokens(lines[p.line]).size();
    };
    if (!valid(anchor) || !valid(current))
        return {};
    if (anchor.line > current.line || (anchor.line == current.line && anchor.token > current.token))
        std::swap(anchor, current);
    QVector<SelectionRange> ranges;
    for (int i = anchor.line; i <= current.line; ++i)
    {
        const auto tokens = selectableTokens(lines[i]);
        if (tokens.isEmpty())
            continue;
        const int first = i == anchor.line ? anchor.token : 0;
        const int last = i == current.line ? current.token : int(tokens.size()) - 1;
        const int offset = i == anchor.line ? tokens[first].offset : 0;
        const int end = i == current.line ? tokens[last].offset + tokens[last].length
                                          : int(lines[i].text.size());
        ranges.append({i, first, last, offset, end - offset});
    }
    return ranges;
}
QString selectionText(const QVector<RecognizedLine>& lines, const QVector<SelectionRange>& ranges)
{
    QStringList slices;
    for (const auto& range : ranges)
        if (range.line >= 0 && range.line < lines.size())
            slices.append(lines[range.line].text.mid(range.offset, range.length));
    return slices.join('\n');
}
}
