// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/ChatArt.hpp"

#include <QTextBoundaryFinder>

#include <algorithm>

namespace chatterino::detail {

namespace {

constexpr qsizetype MIN_ART_CELLS = 40;
constexpr qsizetype MIN_ART_SEGMENTS = 2;

bool isArtCodePoint(char32_t codePoint)
{
    const auto category = QChar::category(codePoint);
    return category == QChar::Symbol_Other ||
           category == QChar::Symbol_Modifier;
}

qsizetype countArtCells(QStringView text)
{
    const auto value = text.toString();
    QTextBoundaryFinder boundaries(QTextBoundaryFinder::Grapheme, value);
    qsizetype cells = 0;
    qsizetype start = 0;

    while (true)
    {
        const auto end = boundaries.toNextBoundary();
        if (end == -1)
        {
            break;
        }

        const auto cluster = QStringView{value}.sliced(start, end - start);
        if (std::ranges::any_of(cluster.toUcs4(), isArtCodePoint))
        {
            ++cells;
        }
        start = end;
    }

    return cells;
}

}  // namespace

bool isChatArt(const QString &content)
{
    // Ignore invisible formatting prefixes when classifying the message.
    qsizetype textStart = 0;
    while (textStart < content.size())
    {
        const auto codePoint = content[textStart].unicode();
        if (codePoint != 0x034F && (codePoint < 0x200B || codePoint > 0x200D) &&
            codePoint != 0xFEFF)
        {
            break;
        }
        ++textStart;
    }
    if (textStart != 0)
    {
        while (textStart < content.size() && content[textStart].isSpace())
        {
            ++textStart;
        }
    }

    const QStringView text{content.constData() + textStart,
                           content.size() - textStart};
    qsizetype artCells = 0;
    qsizetype artSegments = 0;

    for (const auto &segment : text.split(u' ', Qt::SkipEmptyParts))
    {
        const auto segmentCells = countArtCells(segment);
        artCells += segmentCells;
        artSegments += segmentCells != 0;
    }

    return artCells >= MIN_ART_CELLS && artSegments >= MIN_ART_SEGMENTS;
}

}  // namespace chatterino::detail
