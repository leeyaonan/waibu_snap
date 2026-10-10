#pragma once
#include "interfaces/text_recognizer.h"
namespace waibusnap::text_layout
{
struct TextSpan
{
    int offset = 0;
    int length = 0;
};
struct TokenPosition
{
    int line = -1;
    int token = -1;
    bool valid() const { return line >= 0 && token >= 0; }
};
struct SelectionRange
{
    int line = 0;
    int firstToken = 0;
    int lastToken = 0;
    int offset = 0;
    int length = 0;
};
constexpr qreal paragraphGapFactor = 1.6;
QVector<TextSpan> tokenizeLine(const QString& text);
QString assembleFullText(const QVector<RecognizedLine>& lines);
// 原生词盒缺失时整行成为一个选择单元，不猜测字符盒。
QVector<RecognizedToken> selectableTokens(const RecognizedLine& line);
int nearestToken(const RecognizedLine& line, qreal x);
QVector<SelectionRange> selectionRange(const QVector<RecognizedLine>& lines, TokenPosition anchor,
                                       TokenPosition current);
QString selectionText(const QVector<RecognizedLine>& lines, const QVector<SelectionRange>& ranges);
}
