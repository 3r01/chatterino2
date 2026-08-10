// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "messages/ChatArt.hpp"

#include "common/Literals.hpp"
#include "messages/MessageElement.hpp"

#include <gtest/gtest.h>

using namespace chatterino::literals;
using chatterino::MessageElementFlag;
using chatterino::TextElement;
using chatterino::detail::isChatArt;

namespace {

const QString BRAILLE_SEGMENT = QString(20, QChar(0x28FF));
const QString BLOCK_SEGMENT = QString(20, QChar(0x25AC));

}  // namespace

TEST(ChatArt, RejectsAmbiguousMessages)
{
    EXPECT_FALSE(isChatArt(u"ordinary prose ⣿⣿⣿"_s));
    EXPECT_FALSE(isChatArt(BRAILLE_SEGMENT + BRAILLE_SEGMENT));

    const auto modifiedHand = u"👉🏿"_s;
    EXPECT_FALSE(isChatArt(modifiedHand.repeated(10) + u' ' +
                           modifiedHand.repeated(10)));
}

TEST(ChatArt, DetectsMixedUnicodeArt)
{
    const auto longText = QString(200, u'x');
    EXPECT_TRUE(
        isChatArt(BLOCK_SEGMENT + u' ' + longText + u' ' + BLOCK_SEGMENT));

    const auto rowWithBlank = BRAILLE_SEGMENT + QChar(0x2800) + BRAILLE_SEGMENT;
    EXPECT_TRUE(isChatArt(BRAILLE_SEGMENT + u" mixed text "_s + rowWithBlank));
}

TEST(ChatArt, DetectsEmojiArtByGrapheme)
{
    const auto modifiedHand = u"👉🏿"_s;
    EXPECT_TRUE(isChatArt(modifiedHand.repeated(20) + u' ' +
                          modifiedHand.repeated(20)));
}

TEST(ChatArt, PreservesLayoutMetadataWhenCloned)
{
    TextElement element(u"text"_s, MessageElementFlag::Text);
    element.setChatArt();

    const auto clone = element.clone();
    EXPECT_EQ(clone->type(), TextElement::TYPE);
    EXPECT_TRUE(clone->isChatArt());
}
