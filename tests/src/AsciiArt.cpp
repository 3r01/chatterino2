// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/AsciiArt.hpp"

#include "common/Literals.hpp"
#include "messages/MessageElement.hpp"

#include <gtest/gtest.h>

using namespace chatterino::literals;
using chatterino::MessageElementFlag;
using chatterino::TextElement;
using chatterino::detail::isAsciiArt;

namespace {

const QString BRAILLE_SEGMENT = QString(20, QChar(0x28FF));
const QString BLOCK_SEGMENT = QString(20, QChar(0x25AC));

}  // namespace

TEST(AsciiArt, RejectsAmbiguousMessages)
{
    EXPECT_FALSE(isAsciiArt(u"ordinary prose ⣿⣿⣿"_s));
    EXPECT_FALSE(isAsciiArt(BRAILLE_SEGMENT + BRAILLE_SEGMENT));

    const auto modifiedHand = u"👉🏿"_s;
    EXPECT_FALSE(isAsciiArt(modifiedHand.repeated(10) + u' ' +
                            modifiedHand.repeated(10)));
}

TEST(AsciiArt, DetectsMixedUnicodeArt)
{
    const auto longText = QString(200, u'x');
    EXPECT_TRUE(
        isAsciiArt(BLOCK_SEGMENT + u' ' + longText + u' ' + BLOCK_SEGMENT));

    const auto rowWithBlank = BRAILLE_SEGMENT + QChar(0x2800) + BRAILLE_SEGMENT;
    EXPECT_TRUE(isAsciiArt(BRAILLE_SEGMENT + u" mixed text "_s + rowWithBlank));
}

TEST(AsciiArt, DetectsEmojiArtByGrapheme)
{
    const auto modifiedHand = u"👉🏿"_s;
    EXPECT_TRUE(isAsciiArt(modifiedHand.repeated(20) + u' ' +
                           modifiedHand.repeated(20)));
}

TEST(AsciiArt, PreservesLayoutMetadataWhenCloned)
{
    TextElement element(u"text"_s, MessageElementFlag::Text);
    element.setAsciiArt();

    const auto clone = element.clone();
    EXPECT_EQ(clone->type(), TextElement::TYPE);
    EXPECT_TRUE(clone->isAsciiArt());
}
