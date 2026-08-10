// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/AsciiArt.hpp"

#include <QList>
#include <QTextBoundaryFinder>

#include <algorithm>

namespace chatterino {

namespace {

constexpr qsizetype MIN_ART_CELLS = 40;

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

bool isAsciiArt(const QString &content)
{
    qsizetype artCells = 0;

    for (const auto &segment :
         QStringView{content}.split(u' ', Qt::SkipEmptyParts))
    {
        artCells += countArtCells(segment);
    }

    return artCells >= MIN_ART_CELLS;
}

}  // namespace chatterino
