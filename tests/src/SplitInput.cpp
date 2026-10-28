// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/SplitInput.hpp"

#include "common/Literals.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/completion/sources/CommandSource.hpp"
#include "controllers/completion/strategies/CommandStrategy.hpp"
#include "controllers/completion/TabCompletionModel.hpp"
#include "controllers/hotkeys/Hotkey.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/plugins/PluginController.hpp"
#include "messages/Emote.hpp"
#include "messages/MessageElement.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/Channel.hpp"
#include "mocks/EmoteController.hpp"
#include "mocks/Helix.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "Test.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/ResizingTextEdit.hpp"
#include "widgets/listview/GenericListModel.hpp"
#include "widgets/listview/GenericListView.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/Scrollbar.hpp"
#include "widgets/splits/InputCompletionItem.hpp"
#include "widgets/splits/InputCompletionPopup.hpp"
#include "widgets/splits/Split.hpp"

#include <QApplication>
#include <QCompleter>
#include <QDebug>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QString>

#include <initializer_list>
#include <tuple>

using namespace chatterino;
using ::testing::Exactly;

namespace {

class MockApplication : public mock::BaseApplication
{
public:
    MockApplication()
        : windowManager(this->args_, this->paths_, this->settings, this->theme,
                        this->fonts)
        , commands(this->paths_)
    {
    }

    HotkeyController *getHotkeys() override
    {
        return &this->hotkeys;
    }

    WindowManager *getWindows() override
    {
        return &this->windowManager;
    }

    AccountController *getAccounts() override
    {
        return &this->accounts;
    }

    CommandController *getCommands() override
    {
        return &this->commands;
    }

    EmoteController *getEmotes() override
    {
        return &this->emotes;
    }

    ITwitchIrcServer *getTwitch() override
    {
        return &this->twitch;
    }

    mock::MockTwitchIrcServer twitch;

    BttvEmotes *getBttvEmotes() override
    {
        return &this->bttv;
    }

    FfzEmotes *getFfzEmotes() override
    {
        return &this->ffz;
    }

    SeventvEmotes *getSeventvEmotes() override
    {
        return &this->seventv;
    }

    BttvEmotes bttv;
    FfzEmotes ffz;
    SeventvEmotes seventv;

#ifdef CHATTERINO_HAVE_PLUGINS
    PluginController *getPlugins() override
    {
        return &this->plugins;
    }

    PluginController plugins{this->paths_};
#endif

    HotkeyController hotkeys;
    WindowManager windowManager;
    AccountController accounts;
    CommandController commands;
    mock::EmoteController emotes;
};

class TestSplitInput : public SplitInput
{
public:
    using SplitInput::insertCompletionText;
    using SplitInput::showMessageHeight;
    using SplitInput::SplitInput;
    using SplitInput::updateMessageHeight;

    QByteArray heightRequest() const
    {
        return this->lastHeightRequest_;
    }

    void undoInput()
    {
        this->ui_.textEdit->undo();
    }

    bool completionPopupBlocksSendShortcut(const QString &text)
    {
        this->showCompletionPopup(text, CompletionKind::User);
        QKeyEvent event(QEvent::ShortcutOverride, Qt::Key_Return,
                        Qt::NoModifier);
        event.ignore();
        this->eventFilter(this, &event);
        return event.isAccepted();
    }
};

class SplitInputCompletionTest : public ::testing::Test
{
public:
    MockApplication mockApplication;
    Split split{nullptr};
    TestSplitInput input{&this->split};
};

std::shared_ptr<Message> makeReplyableMessage(QString id, QString user,
                                              const QString &text = "message")
{
    auto message = std::make_shared<Message>();
    message->id = std::move(id);
    message->loginName = user;
    message->displayName = std::move(user);
    message->timestamp = QDateTime::currentDateTime();
    message->elements.emplace_back(
        std::make_unique<TextElement>(text, MessageElementFlag::Text));
    return message;
}

class SplitInputTest
    : public ::testing::TestWithParam<std::tuple<QString, QString>>
{
public:
    SplitInputTest()
        : split(new Split(nullptr))
        , input(this->split)
    {
    }

    MockApplication mockApplication;
    Split *split;
    SplitInput input;
};

class CommandPopupFixture : public ::testing::Test
{
public:
    CommandPopupFixture()
        : input(this->split.getInput())
    {
    }

    void setChannel(ChannelPtr channel)
    {
        this->split.setChannel(IndirectChannel(std::move(channel)));
        this->split.show();
    }

    ResizingTextEdit *edit()
    {
        auto *textEdit = this->input.findChild<QTextEdit *>();
        // NOLINTNEXTLINE(clazy-unneeded-cast)
        return dynamic_cast<ResizingTextEdit *>(textEdit);
    }

    InputCompletionPopup *popup()
    {
        const auto children = this->input.findChildren<QWidget *>();
        for (auto *child : children)
        {
            if (auto *result = dynamic_cast<InputCompletionPopup *>(child))
            {
                return result;
            }
        }
        return nullptr;
    }

    void pressTab()
    {
        QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier, "\t");
        QApplication::sendEvent(this->edit(), &tab);
    }

    MockApplication app;
    Split split{nullptr};
    SplitInput &input;
};

}  // namespace

TEST(SplitInput, SetWhisperTarget)
{
    MockApplication app;
    Split split{nullptr};
    TestSplitInput input{&split};
    auto *edit = input.findChild<QTextEdit *>();
    ASSERT_NE(edit, nullptr);

    input.insertText("please stop whispering me");
    auto cursor = edit->textCursor();
    cursor.setPosition(6);
    cursor.setPosition(11, QTextCursor::KeepAnchor);
    edit->setTextCursor(cursor);

    // Prepending keeps existing input text, and does not replace selected text
    input.setWhisperTarget("forsen");
    EXPECT_EQ(input.getInputText(), "/w forsen please stop whispering me");
    EXPECT_EQ(edit->textCursor().position(), input.getInputText().size());

    // Undo removes the prefix without clearing undo history
    input.undoInput();
    EXPECT_EQ(input.getInputText(), "please stop whispering me");
    input.undoInput();
    EXPECT_TRUE(input.getInputText().isEmpty());
}

TEST(SplitInput, SetWhisperTargetWithEmptyInput)
{
    MockApplication app;
    Split split{nullptr};
    TestSplitInput input{&split};

    input.setWhisperTarget("forsen");
    EXPECT_EQ(input.getInputText(), "/w forsen ");
}

TEST(SplitInput, RepeatedWhisperTarget)
{
    MockApplication app;
    Split split{nullptr};
    TestSplitInput input{&split};

    input.insertText("please stop whispering me");
    input.setWhisperTarget("forsen");
    input.setWhisperTarget("forsen");
    EXPECT_EQ(input.getInputText(), "/w forsen please stop whispering me");
}

TEST(SplitInput, ChangeWhisperTarget)
{
    MockApplication app;
    Split split{nullptr};
    TestSplitInput input{&split};

    input.insertText(".w pajlada FeelsDankMan");
    input.setWhisperTarget("forsen");
    EXPECT_EQ(input.getInputText(), "/w forsen FeelsDankMan");

    input.undoInput();
    EXPECT_EQ(input.getInputText(), ".w pajlada FeelsDankMan");
}

TEST(SplitInput, SetWhisperTargetWithoutMessage)
{
    MockApplication app;
    Split split{nullptr};
    TestSplitInput input{&split};

    for (const auto &text : {"/w", "/w ", "/w pajlada", ".w pajlada "})
    {
        input.setInputText(text);
        input.setWhisperTarget("forsen");
        EXPECT_EQ(input.getInputText(), "/w forsen ") << text;
    }
}

namespace chatterino {

class PopupCycleFixture : public ::testing::Test
{
public:
    PopupCycleFixture()
        : input(this->split.getInput())
    {
    }

    void setChannel(ChannelPtr channel)
    {
        this->split.setChannel(IndirectChannel(std::move(channel)));
        this->split.show();
    }

    void setChatters(std::initializer_list<QString> names)
    {
        auto channel = std::make_shared<TwitchChannel>("forsen");
        for (const auto &name : names)
        {
            channel->addRecentChatter(name);
        }
        this->setChannel(channel);
    }

    void setEmotes(std::initializer_list<QString> names, bool zeroWidth = false)
    {
        auto emotes = std::make_shared<EmoteMap>();
        for (const auto &name : names)
        {
            auto emote = std::make_shared<Emote>();
            emote->name = EmoteName{name};
            emote->zeroWidth = zeroWidth;
            emotes->emplace(emote->name, std::move(emote));
        }
        auto channel = std::make_shared<TwitchChannel>("forsen");
        channel->setBttvEmotes(std::move(emotes));
        this->setChannel(channel);
    }

    ResizingTextEdit *edit()
    {
        auto *textEdit = this->input.findChild<QTextEdit *>();
        // NOLINTNEXTLINE(clazy-unneeded-cast)
        return dynamic_cast<ResizingTextEdit *>(textEdit);
    }

    InputCompletionPopup *popup()
    {
        const auto children = this->input.findChildren<QWidget *>();
        for (auto *child : children)
        {
            if (auto *result = dynamic_cast<InputCompletionPopup *>(child))
            {
                return result;
            }
        }
        return nullptr;
    }

    void showUserPopup(const QString &text)
    {
        this->input.showCompletionPopup(text, CompletionKind::User);
    }

    GenericListView *list()
    {
        auto *result = this->popup();
        return result ? result->findChild<GenericListView *>() : nullptr;
    }

    void pressTab()
    {
        QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier, "\t");
        QApplication::sendEvent(this->edit(), &tab);
    }

    void pressBacktab()
    {
        QKeyEvent backtab(QEvent::KeyPress, Qt::Key_Backtab, Qt::ShiftModifier);
        QApplication::sendEvent(this->edit(), &backtab);
    }

    MockApplication app;
    Split split{nullptr};
    SplitInput &input;
};

}  // namespace chatterino

TEST_F(SplitInputCompletionTest, SelectReplyTarget)
{
    auto channel = std::make_shared<TwitchChannel>("test");
    this->split.setChannel(IndirectChannel{channel});

    const auto older = makeReplyableMessage("1", "older");
    auto notReplyable = makeReplyableMessage("2", "system");
    notReplyable->flags.set(MessageFlag::System);
    const auto newer = makeReplyableMessage("3", "newer");
    channel->addMessage(older, MessageContext::Repost);
    channel->addMessage(notReplyable, MessageContext::Repost);
    channel->addMessage(newer, MessageContext::Repost);

    auto &view = this->split.getChannelView();
    auto &input = this->split.getInput();

    // Pressing older without an active reply selects the newest message.
    view.navigateReplyTarget(nullptr, ReplyTargetDirection::Older);
    EXPECT_EQ(input.getInputText(), "@newer ");

    // Skip messages that cannot be replied to.
    view.navigateReplyTarget(newer, ReplyTargetDirection::Older);
    EXPECT_EQ(input.getInputText(), "@older ");

    // Pressing older at the oldest message does nothing.
    view.navigateReplyTarget(older, ReplyTargetDirection::Older);
    EXPECT_EQ(input.getInputText(), "@older ");

    // Pressing newer selects the next replyable message.
    view.navigateReplyTarget(older, ReplyTargetDirection::Newer);
    EXPECT_EQ(input.getInputText(), "@newer ");

    // Moving past the newest replyable message clears the reply.
    view.navigateReplyTarget(newer, ReplyTargetDirection::Newer);
    EXPECT_TRUE(input.getInputText().isEmpty());
}

TEST_F(SplitInputCompletionTest, ReplyTargetScrolling)
{
    auto channel = std::make_shared<TwitchChannel>("test");
    this->split.setChannel(IndirectChannel{channel});
    this->split.resize(500, 300);
    this->split.show();
    auto &view = this->split.getChannelView();
    view.resize(500, 250);
    view.show();

    std::vector<MessagePtr> messages;
    for (int i = 0; i < 30; ++i)
    {
        auto message = makeReplyableMessage(QString::number(i), "user");
        messages.push_back(message);
        channel->addMessage(message, MessageContext::Repost);
    }
    QApplication::processEvents();

    auto &scrollbar = view.getScrollBar();
    ASSERT_GT(scrollbar.getPageSize(), 0);
    scrollbar.scrollToBottom();

    // Navigate far enough back to scroll away from the latest messages.
    view.navigateReplyTarget(nullptr, ReplyTargetDirection::Older);
    for (int i = 29; i > 19; --i)
    {
        view.navigateReplyTarget(messages[i], ReplyTargetDirection::Older);
        QApplication::processEvents();
        QApplication::processEvents();
    }

    ASSERT_LT(scrollbar.getDesiredValue(), scrollbar.getBottom());

    // Moving towards newer messages scrolls only far enough to reveal them.
    bool adjustedScroll = false;
    for (int i = 19; i < 29; ++i)
    {
        const auto previousScroll = scrollbar.getDesiredValue();
        view.navigateReplyTarget(messages[i], ReplyTargetDirection::Newer);
        QApplication::processEvents();
        QApplication::processEvents();

        if (scrollbar.getDesiredValue() > previousScroll)
        {
            adjustedScroll = true;
            EXPECT_LT(scrollbar.getDesiredValue(), i + 1);
        }
    }
    EXPECT_TRUE(adjustedScroll);

    // Cancelling after navigating from the bottom returns to the bottom.
    this->split.getInput().setReply(nullptr);
    QApplication::processEvents();
    EXPECT_GE(scrollbar.getDesiredValue(), scrollbar.getBottom());

    // We do not force the view back to the selected target if the user
    // manually scrolls away
    view.navigateReplyTarget(nullptr, ReplyTargetDirection::Older);
    QApplication::processEvents();
    scrollbar.scrollToTop();
    QApplication::processEvents();
    QApplication::processEvents();
    EXPECT_EQ(scrollbar.getDesiredValue(), scrollbar.getMinimum());

    // After manually scrolling, cancelling the reply does not
    // return the view to the bottom
    this->split.getInput().setReply(nullptr);
    QApplication::processEvents();
    EXPECT_EQ(scrollbar.getDesiredValue(), scrollbar.getMinimum());
}

TEST_F(SplitInputCompletionTest, OversizedReplyTargetAlignsToTop)
{
    auto channel = std::make_shared<TwitchChannel>("test");
    this->split.setChannel(IndirectChannel{channel});
    this->split.resize(500, 300);
    this->split.show();
    auto &view = this->split.getChannelView();
    view.resize(500, 100);
    view.show();

    const auto oversized = makeReplyableMessage(
        "1", "oversized", QString("forsen ").repeated(200));
    const auto newer = makeReplyableMessage("2", "newer");
    channel->addMessage(oversized, MessageContext::Repost);
    channel->addMessage(newer, MessageContext::Repost);
    QApplication::processEvents();

    auto &scrollbar = view.getScrollBar();
    scrollbar.scrollToBottom();

    // An oversized target cannot be fully shown, so align it to the top.
    view.navigateReplyTarget(nullptr, ReplyTargetDirection::Older);
    view.navigateReplyTarget(newer, ReplyTargetDirection::Older);
    QApplication::processEvents();
    QApplication::processEvents();

    EXPECT_EQ(scrollbar.getDesiredValue(), scrollbar.getMinimum());
}

TEST_F(SplitInputCompletionTest, EmoteCompletionPreservesUndoHistory)
{
    this->input.insertText("don't ping him :paja");
    ASSERT_EQ("don't ping him :paja", this->input.getInputText());

    this->input.insertCompletionText("pajaGIGA");
    ASSERT_EQ("don't ping him pajaGIGA ", this->input.getInputText());

    this->input.undoInput();
    EXPECT_EQ("don't ping him :paja", this->input.getInputText());
    this->input.undoInput();
    EXPECT_TRUE(this->input.getInputText().isEmpty());
}

TEST_F(SplitInputCompletionTest, UsernameCompletionPreservesUndoHistory)
{
    this->input.insertText("boring game @fors");
    ASSERT_EQ("boring game @fors", this->input.getInputText());

    this->input.insertCompletionText("forsen");
    ASSERT_EQ("boring game @forsen ", this->input.getInputText());

    this->input.undoInput();
    EXPECT_EQ("boring game @fors", this->input.getInputText());
    this->input.undoInput();
    EXPECT_TRUE(this->input.getInputText().isEmpty());
}

TEST(CommandSource, DotPrefixSelectsSlashCommand)
{
    MockApplication app;
    app.commands.items.append(Command{"/zzpopupcommand", "test"});

    QString selected;
    completion::CommandSource source(
        std::make_unique<completion::CommandStrategy>(true),
        [&selected](const QString &value) {
            selected = value;
        });
    source.update(".zzpopupcommand");
    GenericListModel model;
    source.addToListModel(model);

    ASSERT_EQ(model.rowCount(), 1);
    auto *item = GenericListItem::fromVariant(
        model.data(model.index(0, 0), Qt::DisplayRole));
    ASSERT_NE(item, nullptr);
    item->action();
    EXPECT_EQ(selected, "/zzpopupcommand");
}

TEST(CommandSource, DuplicateCommandsAppearOnce)
{
    MockApplication app;
    app.commands.items.append(Command{"/zzpopupcommand", "test"});
    app.commands.items.append(Command{"/zzpopupcommand", "test"});
    completion::CommandSource source(
        std::make_unique<completion::CommandStrategy>(true));
    source.update("/zzpopupcommand");
    GenericListModel model;
    source.addToListModel(model);

    EXPECT_EQ(model.rowCount(), 1);
}

TEST_F(CommandPopupFixture, TabAcceptsAndCyclesItsSelections)
{
    this->app.commands.items.append(Command{"/zzpopupfirst", "test"});
    this->app.commands.items.append(Command{"/zzpopupsecond", "test"});
    this->setChannel(std::make_shared<TwitchChannel>("forsen"));
    auto *edit = this->edit();
    ASSERT_NE(edit, nullptr);

    this->input.insertText("/zzpopup");
    auto *popup = this->popup();
    ASSERT_NE(popup, nullptr);
    ASSERT_TRUE(popup->isVisible());

    auto *list = popup->findChild<GenericListView *>();
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->model()->rowCount(), 2);
    list->setCurrentIndex(list->model()->index(1, 0));

    this->pressTab();
    EXPECT_FALSE(popup->isVisible());
    EXPECT_EQ(this->input.getInputText(), "/zzpopupsecond ");

    this->pressTab();
    EXPECT_EQ(this->input.getInputText(), "/zzpopupfirst ");

    this->pressTab();
    EXPECT_EQ(this->input.getInputText(), "/zzpopupsecond ");
}

TEST_F(CommandPopupFixture, DisabledPopupKeepsTabCompletion)
{
    this->app.settings.showCommandCompletionMenu = false;
    this->app.commands.items.append(Command{"/zzpopupfirst", "test"});
    this->setChannel(std::make_shared<TwitchChannel>("forsen"));

    this->input.insertText("/zzpopup");
    EXPECT_EQ(this->popup(), nullptr);

    this->pressTab();
    EXPECT_EQ(this->input.getInputText(), "/zzpopupfirst ");
}

TEST_F(PopupCycleFixture, CustomSendShortcutWorksWithEmptyPopup)
{
    class RecordingChannel : public mock::MockChannel
    {
    public:
        RecordingChannel()
            : mock::MockChannel("forsen")
        {
        }

        void sendMessage(const QString &message) override
        {
            this->sentMessages.append(message);
        }

        QStringList sentMessages;
    };

    this->app.hotkeys.replaceHotkey(
        "send message",
        std::make_shared<Hotkey>(HotkeyCategory::SplitInput, QKeySequence("F6"),
                                 "sendMessage", std::vector<QString>{},
                                 "send message"));
    this->app.hotkeys.onItemsUpdated.invoke();
    auto channel = std::make_shared<RecordingChannel>();
    this->setChannel(channel);
    QApplication::setActiveWindow(&this->split);
    this->edit()->setFocus();
    QApplication::processEvents();
    ASSERT_TRUE(this->edit()->hasFocus());

    const auto pressSend = [this] {
        QKeyEvent press(QEvent::KeyPress, Qt::Key_F6, Qt::NoModifier);
        QApplication::sendEvent(this->edit(), &press);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_F6, Qt::NoModifier);
        QApplication::sendEvent(this->edit(), &release);
        QApplication::processEvents();
    };

    // The custom binding sends normally without a popup
    this->input.insertText("hello");
    pressSend();
    ASSERT_EQ(channel->sentMessages, QStringList{"hello"});
    ASSERT_TRUE(this->input.getInputText().isEmpty());

    // An empty popup doesn't block the custom send binding
    this->input.insertText("@zznothingmatches");
    this->showUserPopup("@zznothingmatches");
    ASSERT_NE(this->popup(), nullptr);
    QApplication::setActiveWindow(&this->split);
    this->edit()->setFocus();
    this->popup()->show();
    ASSERT_TRUE(this->edit()->hasFocus());
    ASSERT_TRUE(this->popup()->isVisible());
    ASSERT_EQ(this->list()->model()->rowCount(), 0);
    pressSend();
    EXPECT_EQ(channel->sentMessages,
              (QStringList{"hello", "@zznothingmatches"}));
    EXPECT_TRUE(this->input.getInputText().isEmpty());
}

TEST_F(PopupCycleFixture, UsernameSelectionCyclesWithTab)
{
    this->setChatters({"zzpopupfirst", "zzpopupsecond"});
    ASSERT_NE(this->edit(), nullptr);

    this->input.insertText("@zzpopup");
    auto *list = this->list();
    ASSERT_NE(list, nullptr);
    ASSERT_TRUE(this->popup()->isVisible());
    ASSERT_EQ(list->model()->rowCount(), 2);
    list->setCurrentIndex(list->model()->index(1, 0));
    const auto *selected = dynamic_cast<const InputCompletionItem *>(
        GenericListItem::fromVariant(list->currentIndex().data()));
    ASSERT_NE(selected, nullptr);
    const auto selectedText = "@" + selected->insertionText() + ", ";

    // The first tab accepts the selected username
    this->pressTab();
    const auto first = this->input.getInputText();
    EXPECT_EQ(first, selectedText);

    // Further tabs cycle through the popup usernames
    this->pressTab();
    const auto second = this->input.getInputText();
    EXPECT_TRUE(second == "@zzpopupfirst, " || second == "@zzpopupsecond, ");
    EXPECT_NE(second, first);
    this->pressTab();
    EXPECT_EQ(this->input.getInputText(), first);
    this->pressBacktab();
    EXPECT_EQ(this->input.getInputText(), second);
}

TEST_F(PopupCycleFixture, EnterAcceptanceCyclesWithTab)
{
    this->setChatters({"zzpopupfirst", "zzpopupsecond"});
    this->input.insertText("@zzpopup");
    auto *list = this->list();
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->model()->rowCount(), 2);
    list->setCurrentIndex(list->model()->index(1, 0));

    // Enter accepts the popup item and tab continues through its list
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, "\r");
    QApplication::sendEvent(this->edit(), &enter);
    const auto first = this->input.getInputText();
    EXPECT_TRUE(first == "@zzpopupfirst, " || first == "@zzpopupsecond, ");
    this->pressTab();
    EXPECT_NE(this->input.getInputText(), first);
}

TEST_F(PopupCycleFixture, OneCharacterSelectionCyclesAfterClick)
{
    this->setEmotes({"⚽", "⚽more"}, true);
    this->input.insertText(":~⚽");
    auto *list = this->list();
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->model()->rowCount(), 2);
    const auto *first = dynamic_cast<const InputCompletionItem *>(
        GenericListItem::fromVariant(list->model()->index(0, 0).data()));
    ASSERT_NE(first, nullptr);
    const auto selection = list->model()->index(
        first->insertionText() == QString::fromUtf8("⚽") ? 0 : 1, 0);
    const auto *selected = dynamic_cast<const InputCompletionItem *>(
        GenericListItem::fromVariant(selection.data()));
    ASSERT_NE(selected, nullptr);
    ASSERT_EQ(selected->insertionText(), QString::fromUtf8("⚽"));

    // Clicking a one-character result still allows tab to choose the next
    list->setCurrentIndex(selection);
    Q_EMIT list->clicked(selection);
    EXPECT_EQ(this->input.getInputText(), QString::fromUtf8("⚽ "));
    this->pressTab();
    EXPECT_EQ(this->input.getInputText(), QString::fromUtf8("⚽more "));
}

TEST_F(PopupCycleFixture, CompletionAfterNewlineKeepsPreviousLine)
{
    this->setChatters({"zzpopupfirst", "zzpopupsecond"});
    this->input.insertText("previous\n@zzpopup");
    auto *list = this->list();
    ASSERT_NE(list, nullptr);
    ASSERT_EQ(list->model()->rowCount(), 2);
    list->setCurrentIndex(list->model()->index(0, 0));

    // Cycling a completion on a new line keeps the text before the newline
    this->pressTab();
    const auto first = this->input.getInputText();
    ASSERT_TRUE(first.startsWith("previous\n@"));
    this->pressTab();
    const auto second = this->input.getInputText();
    EXPECT_TRUE(second.startsWith("previous\n@"));
    EXPECT_NE(second, first);
}

TEST_F(PopupCycleFixture, DuplicateEmoteNamesCycleOnce)
{
    this->setEmotes({"zzdupone", "zzduptwo"});
    auto duplicate = std::make_shared<Emote>();
    duplicate->name = EmoteName{"zzdupone"};
    auto globalEmotes = std::make_shared<EmoteMap>();
    globalEmotes->emplace(duplicate->name, duplicate);
    this->app.bttv.setEmotes(std::move(globalEmotes));

    this->input.insertText(":zzdup");
    auto *list = this->list();
    ASSERT_NE(list, nullptr);
    // The same emote name appears in both the channel and global results
    ASSERT_EQ(list->model()->rowCount(), 3);

    int lastDuplicateRow = -1;
    int duplicateCount = 0;
    for (int row = 0; row < list->model()->rowCount(); ++row)
    {
        const auto *item = dynamic_cast<const InputCompletionItem *>(
            GenericListItem::fromVariant(list->model()->index(row, 0).data()));
        ASSERT_NE(item, nullptr);
        if (item->insertionText() == "zzdupone")
        {
            lastDuplicateRow = row;
            ++duplicateCount;
        }
    }
    ASSERT_EQ(duplicateCount, 2);
    ASSERT_GE(lastDuplicateRow, 0);
    list->setCurrentIndex(list->model()->index(lastDuplicateRow, 0));

    // Selecting a duplicate skips the other copy on the next tab
    this->pressTab();
    EXPECT_EQ(this->input.getInputText(), "zzdupone ");
    this->pressTab();
    EXPECT_EQ(this->input.getInputText(), "zzduptwo ");
}

TEST_F(SplitInputCompletionTest, EmptyPopupDoesNotBlockSendShortcut)
{
    auto channel = std::make_shared<TwitchChannel>("forsen");
    channel->addRecentChatter("pajlada");
    this->split.setChannel(IndirectChannel{channel});

    // An empty completion popup does not block the send shortcut.
    EXPECT_FALSE(this->input.completionPopupBlocksSendShortcut("@nothing"));

    // A matching completion still takes priority over sending.
    EXPECT_TRUE(this->input.completionPopupBlocksSendShortcut("@paj"));
}

TEST_F(SplitInputCompletionTest, TabCompletionPreservesUndoHistory)
{
    ResizingTextEdit edit;
    QCompleter completer;
    edit.setCompleter(&completer);
    edit.insertPlainText("UR DONE BA");

    Q_EMIT completer.highlighted(QString("BAND "));
    ASSERT_EQ("UR DONE BAND ", edit.toPlainText());

    edit.undo();
    EXPECT_EQ("UR DONE BA", edit.toPlainText());
    edit.undo();
    EXPECT_TRUE(edit.toPlainText().isEmpty());
}

TEST_P(SplitInputTest, Reply)
{
    std::tuple<QString, QString> params = this->GetParam();
    auto [inputText, expected] = params;
    ASSERT_EQ("", this->input.getInputText());
    this->input.setInputText(inputText);
    ASSERT_EQ(inputText, this->input.getInputText());

    auto *message = new Message();
    message->displayName = "forsen";
    auto reply = MessagePtr(message);
    this->input.setReply(reply);
    QString actual = this->input.getInputText();
    ASSERT_EQ(expected, actual) << "Input text after setReply should be '"
                                << expected << "', but got '" << actual << "'";
}

TEST(SplitInput, EscapeCancelsReply)
{
    MockApplication app;
    Split split(nullptr);
    SplitInput input(&split);
    auto *edit = input.findChild<QTextEdit *>();
    ASSERT_NE(edit, nullptr);

    auto message = std::make_shared<Message>();
    message->displayName = "forsen";
    input.setReply(message);
    input.insertText("FeelsWeirdMan");
    ASSERT_EQ(input.getInputText(), "@forsen FeelsWeirdMan");

    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(edit, &escape);
    EXPECT_EQ(input.getInputText(), "FeelsWeirdMan");
}

TEST(SplitInput, ReplyCommandCompletion)
{
    MockApplication app;
    app.commands.items.append(Command{"/slashreplycompletion", "test"});
    app.commands.items.append(Command{".dotreplycompletion", "test"});
    Split split(nullptr);
    SplitInput input(&split);
    auto *textEdit = input.findChild<QTextEdit *>();
    // NOLINTNEXTLINE(clazy-unneeded-cast)
    auto *edit = dynamic_cast<ResizingTextEdit *>(textEdit);
    ASSERT_NE(edit, nullptr);
    mock::MockChannel channel("forsen");
    edit->setCompleter(new QCompleter(channel.completionModel, edit));

    // Slash and dot commands can be completed in replies
    for (const auto &trigger :
         {QString("/slashreplycomp"), QString(".dotreplycomp")})
    {
        auto message = std::make_shared<Message>();
        message->displayName = "forsen";
        input.setInputText("");
        input.setReply(message);
        input.insertText(trigger);

        // Commands are completed at the first word after the reply prefix `@username `
        QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
        QApplication::sendEvent(edit, &tab);
        EXPECT_EQ(input.getInputText(), "@forsen " + trigger + "letion ");

        // Commands are not completed later than the first word after the prefix
        input.setInputText("@forsen test " + trigger);
        edit->moveCursor(QTextCursor::End);
        QApplication::sendEvent(edit, &tab);
        EXPECT_EQ(input.getInputText(), "@forsen test " + trigger);

        // Manually restoring an edited prefix does not restore reply completion
        input.setInputText("@forsenX " + trigger);
        input.setInputText("@forsen " + trigger);
        edit->moveCursor(QTextCursor::End);
        QApplication::sendEvent(edit, &tab);
        EXPECT_EQ(input.getInputText(), "@forsen " + trigger);

        // Normal command completion works after cancelling a reply
        input.setReply(message);
        input.setReply(nullptr);
        input.setInputText(trigger);
        edit->moveCursor(QTextCursor::End);
        QApplication::sendEvent(edit, &tab);
        EXPECT_EQ(input.getInputText(), trigger + "letion ");
    }
}

TEST(SplitInput, EmptyReplyUsernameCompletion)
{
    MockApplication app;
    app.settings.mentionUsersWithComma = true;
    app.settings.alwaysIncludeBroadcasterInUserCompletions = true;
    app.commands.items.append(Command{"/slashreplycompletion", "test"});
    Split split(nullptr);
    SplitInput input(&split);
    auto *textEdit = input.findChild<QTextEdit *>();
    // NOLINTNEXTLINE(clazy-unneeded-cast)
    auto *edit = dynamic_cast<ResizingTextEdit *>(textEdit);
    ASSERT_NE(edit, nullptr);
    TwitchChannel channel("forsen");
    edit->setCompleter(new QCompleter(channel.completionModel, edit));

    auto message = std::make_shared<Message>();
    message->displayName = "forsen";
    input.setReply(message);

    // Completing the username in an empty reply does not add a comma
    QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
    QApplication::sendEvent(edit, &tab);
    EXPECT_EQ(input.getInputText(), "@forsen ");

    // Reply completion still works after completing the prefix username
    input.insertText("/slashreplycomp");
    QApplication::sendEvent(edit, &tab);
    EXPECT_EQ(input.getInputText(), "@forsen /slashreplycompletion ");
}

TEST(SplitInput, ReplyBodyUsernameCompletion)
{
    MockApplication app;
    Split split(nullptr);
    SplitInput input(&split);
    auto *textEdit = input.findChild<QTextEdit *>();
    // NOLINTNEXTLINE(clazy-unneeded-cast)
    auto *edit = dynamic_cast<ResizingTextEdit *>(textEdit);
    ASSERT_NE(edit, nullptr);
    TwitchChannel channel("forsen");
    channel.addRecentChatter("pajlada");
    edit->setCompleter(new QCompleter(channel.completionModel, edit));

    auto message = std::make_shared<Message>();
    message->displayName = "forsen";
    input.setReply(message);

    for (bool mentionComma : {true, false})
    {
        app.settings.mentionUsersWithComma = mentionComma;

        // The first username in the reply body respects the comma setting
        input.setInputText("@forsen @paj");
        edit->moveCursor(QTextCursor::End);
        EXPECT_TRUE(input.isEditFirstWord());
        QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
        QApplication::sendEvent(edit, &tab);
        EXPECT_EQ(input.getInputText(),
                  mentionComma ? "@forsen @pajlada, " : "@forsen @pajlada ");

        // Usernames later in the reply body do not get a comma
        input.setInputText("@forsen test @paj");
        edit->moveCursor(QTextCursor::End);
        EXPECT_FALSE(input.isEditFirstWord());
        QApplication::sendEvent(edit, &tab);
        EXPECT_EQ(input.getInputText(), "@forsen test @pajlada ");
    }
}

TEST(SplitInput, HeightOverflowFormatting)
{
    MockApplication app;
    app.settings.showMessageHeight = true;
    Split split(nullptr);
    split.setChannel(
        IndirectChannel{std::make_shared<TwitchChannel>("forsen")});
    TestSplitInput input(&split);
    setMessageHeightLimit("forsen", 50);
    input.setInputText("hello world");
    auto *edit = input.findChild<QTextEdit *>();
    ASSERT_NE(edit, nullptr);

    input.showMessageHeight({.pixels = 74, .overflowStart = 6}, false);
    const auto selections = edit->extraSelections();
    ASSERT_FALSE(selections.isEmpty());
    EXPECT_EQ(selections.last().cursor.selectedText(), "world");
    EXPECT_EQ(selections.last().format.foreground().color(), QColor("orange"));

    // Keep the orange text while the next height is being calculated
    edit->moveCursor(QTextCursor::End);
    edit->insertPlainText("!");
    const auto selectionsWhileTyping = edit->extraSelections();
    ASSERT_FALSE(selectionsWhileTyping.isEmpty());
    EXPECT_EQ(selectionsWhileTyping.last().cursor.selectedText(), "world!");
    EXPECT_EQ(selectionsWhileTyping.last().format.foreground().color(),
              QColor("orange"));

    // Going back under the height limit clears the orange text
    input.showMessageHeight({.pixels = 30}, false);
    const auto selectionsUnderLimit = edit->extraSelections();
    for (const auto &selection : selectionsUnderLimit)
    {
        EXPECT_NE(selection.format.foreground().color(), QColor("orange"));
    }

    input.showMessageHeight({.pixels = 74, .overflowStart = 6}, false);
    input.setInputText("hi");
    EXPECT_EQ(edit->extraSelections().constLast().cursor.selectedText(), "");
    input.setInputText("");
    EXPECT_TRUE(edit->extraSelections().isEmpty());

    // The character limit stays red even when the height limit is exceeded
    app.settings.messageOverflow = MessageOverflow::Highlight;
    input.setInputText(QString(502, 'a'));
    input.showMessageHeight({.pixels = 74, .overflowStart = 6}, false);
    const auto selectionsOverBothLimits = edit->extraSelections();
    bool foundCharacterOverflow = false;
    bool foundHeightOverflow = false;
    for (const auto &selection : selectionsOverBothLimits)
    {
        const auto color = selection.format.foreground().color();
        if (color == QColor(Qt::red))
        {
            foundCharacterOverflow = true;
            EXPECT_EQ(selection.cursor.selectionStart(), 500);
            EXPECT_EQ(selection.cursor.selectionEnd(), 502);
        }
        if (color == QColor("orange"))
        {
            foundHeightOverflow = true;
            EXPECT_EQ(selection.cursor.selectionStart(), 6);
            EXPECT_EQ(selection.cursor.selectionEnd(), 500);
        }
    }
    EXPECT_TRUE(foundCharacterOverflow);
    EXPECT_TRUE(foundHeightOverflow);
}

TEST(SplitInput, HeightCounterUsesCalculatedHeight)
{
    MockApplication app;
    app.settings.showMessageHeight = true;
    Split split(nullptr);
    split.setChannel(
        IndirectChannel{std::make_shared<TwitchChannel>("forsen")});
    TestSplitInput input(&split);
    input.setInputText("hello");
    input.showMessageHeight({.pixels = 52.1875F}, false);
    const QLabel *counter = nullptr;
    const auto labels = input.findChildren<QLabel *>();
    for (auto *label : labels)
    {
        if (label->toolTip().startsWith("Message height:"))
        {
            counter = label;
            break;
        }
    }
    ASSERT_NE(counter, nullptr);

    // A taller single line still stays blank below two normal text lines
    input.showMessageHeight({.pixels = 42}, false);
    EXPECT_TRUE(counter->text().isEmpty());
    input.showMessageHeight({.pixels = 52}, false);
    EXPECT_TRUE(counter->text().isEmpty());
    input.showMessageHeight({.pixels = 52.1875F}, false);

    // Show the counter even if the input fits on one line
    EXPECT_EQ(counter->text(), "53");

    // Input line breaks don't decide whether the counter is shown
    input.setInputText("hello\nworld");
    input.showMessageHeight({.pixels = 30.1875F}, false);
    EXPECT_TRUE(counter->text().isEmpty());
    input.showMessageHeight({.pixels = 52.1875F}, true);
    EXPECT_EQ(counter->text(), "~53");
}

TEST(SplitInput, HeightCounterPreservesInputGeometry)
{
    MockApplication app;
    app.settings.showMessageLength = true;
    app.settings.showEmptyInput = true;
    const auto geometry = [&](bool enabled, const QString &text) {
        app.settings.showMessageHeight = enabled;
        Split split(nullptr);
        split.setChannel(
            IndirectChannel{std::make_shared<TwitchChannel>("forsen")});
        auto &input = split.getInput();
        input.setInputText(text);
        split.resize(400, 600);
        split.show();
        QApplication::processEvents();
        QApplication::processEvents();
        const auto *edit = input.findChild<QTextEdit *>();
        return std::pair{input.height(), edit->mapTo(&input, QPoint{}).y()};
    };
    for (const auto &text : {"", "a", "a\na\na"})
    {
        SCOPED_TRACE(text);
        EXPECT_EQ(geometry(false, text), geometry(true, text));
    }
}

TEST(SplitInput, HeightCounterDoesNotTakeSpareWidth)
{
    MockApplication app;
    app.settings.showMessageHeight = true;
    app.settings.showMessageLength = true;
    app.settings.showEmptyInput = true;
    Split split(nullptr);
    split.setChannel(
        IndirectChannel{std::make_shared<TwitchChannel>("forsen")});
    auto &input = split.getInput();
    input.setInputText("how long is he going to play this shit forsenY");
    split.show();
    const auto *edit = input.findChild<QTextEdit *>();
    ASSERT_NE(edit, nullptr);
    for (const auto width : {400, 800})
    {
        split.resize(width, 600);
        QApplication::processEvents();
        QApplication::processEvents();

        // Spare width belongs to the input, not the counters and buttons
        EXPECT_GT(edit->width(), input.width() - 100);
    }
}

TEST(SplitInput, HeightOverflowBoundary)
{
    if (!QFileInfo::exists(QCoreApplication::applicationDirPath() +
                           "/message-height/upstream-revision"))
    {
        GTEST_SKIP() << "MessageHeightTwitch isn't installed";
    }
    const auto text = QString("test ").repeated(80);
    QJsonArray offsets;
    for (int index = 0; index <= text.size(); ++index)
    {
        offsets.append(index);
    }
    QJsonObject request{{"text", text},
                        {"login", "iore"},
                        {"displayName", "Iore"},
                        {"badges", 2},
                        {"providers", QJsonObject{}},
                        {"heightLimit", 50},
                        {"sourceOffsets", offsets}};
    const auto result = measureMessageHeight(QJsonDocument(request).toJson());
    ASSERT_TRUE(result.error.isEmpty()) << result.error.toStdString();
    ASSERT_GT(result.pixels, 50);
    ASSERT_GE(result.overflowStart, 0);
    ASSERT_LT(result.overflowStart, text.size());

    // Removing the overflowing part brings the message under the limit
    request.insert("heightLimit", 0);
    request.insert("text", text.left(result.overflowStart));
    EXPECT_LE(measureMessageHeight(QJsonDocument(request).toJson()).pixels, 50);
    request.insert("text", text.left(result.overflowStart + 1));
    EXPECT_GT(measureMessageHeight(QJsonDocument(request).toJson()).pixels, 50);
}

TEST(SplitInput, HeightHighlightsWholeEmote)
{
    if (!QFileInfo::exists(QCoreApplication::applicationDirPath() +
                           "/message-height/upstream-revision"))
    {
        GTEST_SKIP() << "MessageHeightTwitch isn't installed";
    }
    const QString text = "hello BAND";
    QJsonArray offsets;
    for (int index = 0; index <= text.size(); ++index)
    {
        offsets.append(index);
    }
    QJsonObject request{
        {"text", text},
        {"login", "iore"},
        {"displayName", "Iore"},
        {"badges", 1},
        {"providers",
         QJsonObject{
             {"bttv", QJsonArray{QJsonObject{
                          {"name", "BAND"}, {"width", 75}, {"height", 100}}}}}},
        {"heightLimit", 50},
        {"sourceOffsets", offsets}};
    for (const bool duplicates : {false, true})
    {
        request.insert("duplicateVariants", duplicates);
        const auto result =
            measureMessageHeight(QJsonDocument(request).toJson());
        ASSERT_TRUE(result.error.isEmpty());
        EXPECT_GT(result.pixels, 50);

        // Highlight BAND, not just the D that completes it
        EXPECT_EQ(result.overflowStart, 6);
    }
}

TEST(SplitInput, HeightCounterUsesCommandOutput)
{
    ::testing::NiceMock<mock::Helix> helix;
    initializeHelix(&helix);
    MockApplication app;
    auto &accounts = app.accounts.twitch;
    accounts.load();
    accounts.accounts.append(
        std::make_shared<TwitchAccount>("iore", "", "", "123"));
    accounts.currentUsername = "iore";
    app.settings.showMessageHeight = true;
    const auto output = QString("NaM BAND ").repeated(10).trimmed();
    app.commands.items.append(Command{"/heighttest", output});
    Split split(nullptr);
    split.setChannel(
        IndirectChannel{std::make_shared<TwitchChannel>("forsen")});
    TestSplitInput input(&split);
    input.setInputText("/heighttest");
    input.updateMessageHeight();
    ASSERT_FALSE(input.heightRequest().isEmpty());
    const auto json = QJsonDocument::fromJson(input.heightRequest()).object();

    EXPECT_EQ(json.value("text").toString(), output);
    // Highlight the command, not positions from its expanded output
    const auto offsets = json.value("sourceOffsets").toArray();
    for (const auto &offset : offsets)
    {
        EXPECT_EQ(offset.toInt(), 0);
    }
}

TEST(SplitInput, HeightRequestUsesSendWhitespace)
{
    ::testing::NiceMock<mock::Helix> helix;
    initializeHelix(&helix);
    MockApplication app;
    auto &accounts = app.accounts.twitch;
    accounts.load();
    accounts.accounts.append(
        std::make_shared<TwitchAccount>("iore", "", "", "123"));
    accounts.currentUsername = "iore";
    ASSERT_FALSE(accounts.getCurrent()->isAnon());
    auto channel = std::make_shared<TwitchChannel>("forsen");
    app.settings.allowDuplicateMessages = true;
    const auto request = prepareMessageHeight(*channel, "  a \t b \n c  ");
    ASSERT_TRUE(request.error.isEmpty());
    const auto json = QJsonDocument::fromJson(request.json).object();

    // Use the text that sending produces, but keep the original highlight positions
    EXPECT_EQ(json.value("text").toString(), "a b c");
    EXPECT_EQ(json.value("sourceOffsets").toArray(),
              (QJsonArray{2, 3, 6, 7, 10, 11}));
    EXPECT_TRUE(json.value("duplicateVariants").toBool());

    // Don't add suffixes when duplicate bypass isn't used
    app.settings.allowDuplicateMessages = false;
    EXPECT_FALSE(
        QJsonDocument::fromJson(prepareMessageHeight(*channel, "a").json)
            .object()
            .value("duplicateVariants")
            .toBool());
    app.settings.allowDuplicateMessages = true;
    auto ownChannel = std::make_shared<TwitchChannel>("iore");
    EXPECT_FALSE(
        QJsonDocument::fromJson(prepareMessageHeight(*ownChannel, "a").json)
            .object()
            .value("duplicateVariants")
            .toBool());
}

TEST(SplitInput, HeightIncludesDuplicateSuffixes)
{
    if (!QFileInfo::exists(QCoreApplication::applicationDirPath() +
                           "/message-height/upstream-revision"))
    {
        GTEST_SKIP() << "MessageHeightTwitch isn't installed";
    }
    const auto text = QString(30, 'a');
    QJsonArray offsets;
    for (int index = 0; index <= text.size(); ++index)
    {
        offsets.append(index);
    }
    QJsonObject request{{"text", text},
                        {"login", "iore"},
                        {"displayName", "Iore"},
                        {"badges", 1},
                        {"providers", QJsonObject{}},
                        {"heightLimit", 40},
                        {"sourceOffsets", offsets}};
    const auto original = measureMessageHeight(QJsonDocument(request).toJson());
    ASSERT_TRUE(original.error.isEmpty());
    EXPECT_LT(original.pixels, 40);

    // The invisible suffix can push an otherwise fitting message onto another line
    request.insert("duplicateVariants", true);
    const auto worst = measureMessageHeight(QJsonDocument(request).toJson());
    ASSERT_TRUE(worst.error.isEmpty());
    EXPECT_GT(worst.pixels, 40);
    EXPECT_GE(worst.overflowStart, 0);
    EXPECT_LT(worst.overflowStart, text.size());

    request.insert("duplicateVariants", false);
    request.insert("heightLimit", 0);
    float maximum = original.pixels;
    for (const auto &suffix : {QString(" \u034f"), QString("  \u034f")})
    {
        request.insert("text", text + suffix);
        maximum = std::max(
            maximum,
            measureMessageHeight(QJsonDocument(request).toJson()).pixels);
    }
    EXPECT_EQ(worst.pixels, maximum);

    // Don't measure duplicate variants that exceed Twitch's character limit
    request.insert("text", QString(500, 'a'));
    request.insert("duplicateVariants", true);
    EXPECT_TRUE(
        measureMessageHeight(QJsonDocument(request).toJson()).error.isEmpty());
}

TEST(SplitInput, ReplyPrefixFormatting)
{
    MockApplication app;
    Split split(nullptr);
    SplitInput input(&split);
    auto *edit = input.findChild<QTextEdit *>();
    ASSERT_NE(edit, nullptr);

    auto message = std::make_shared<Message>();
    message->displayName = "forsen";
    input.setReply(message);
    input.insertText("pretty much everywhere, it's gonna be hot");

    auto prefixFormatting = [&](const QString &prefix) {
        QList<QTextEdit::ExtraSelection> result;
        const auto selections = edit->extraSelections();
        for (const auto &selection : selections)
        {
            if (selection.cursor.selectionStart() == 0 &&
                selection.cursor.selectionEnd() == prefix.size())
            {
                result.append(selection);
            }
        }
        return result;
    };

    // Reply prefix is set when replying
    ASSERT_EQ(prefixFormatting("@forsen ").size(), 1);
    EXPECT_EQ(prefixFormatting("@forsen ").constFirst().cursor.selectedText(),
              "@forsen ");

    // Reply prefix uses the theme's placeholder colour
    EXPECT_EQ(
        prefixFormatting("@forsen ").constFirst().format.foreground().color(),
        app.theme.messages.textColors.chatPlaceholder);

    // The cursor stays in position after the message
    EXPECT_EQ(edit->textCursor().position(), input.getInputText().size());

    // Changing the reply target updates the prefix
    auto other = std::make_shared<Message>();
    other->displayName = "pajlada";
    input.setReply(other);
    ASSERT_EQ(prefixFormatting("@pajlada ").size(), 1);
    EXPECT_EQ(prefixFormatting("@pajlada ").constFirst().cursor.selectedText(),
              "@pajlada ");

    // Editing the prefix invalidates it
    input.setInputText("@pajladaX pajaBing");
    EXPECT_TRUE(prefixFormatting("@pajlada ").isEmpty());

    // Restoring the prefix manually does not restore it on our end
    input.setInputText("@pajlada pajaBing");
    EXPECT_TRUE(prefixFormatting("@pajlada ").isEmpty());

    // Cancelling the reply invalidates the prefix
    input.setReply(message);
    input.setReply(nullptr);
    EXPECT_TRUE(prefixFormatting("@forsen ").isEmpty());

    // Clearing the input invalidates the reply prefix
    input.setReply(message);
    input.setInputText("");
    EXPECT_TRUE(prefixFormatting("@forsen ").isEmpty());
}

INSTANTIATE_TEST_SUITE_P(
    SplitInput, SplitInputTest,
    testing::Values(
        // Ensure message is retained
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "Test message",
            // Expected text after replying to forsen
            "@forsen Test message "),

        // Ensure mention is stripped, no message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "@forsen",
            // Expected text after replying to forsen
            "@forsen "),

        // Ensure mention with space is stripped, no message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "@forsen ",
            // Expected text after replying to forsen
            "@forsen "),

        // Ensure mention is stripped, retain message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "@forsen Test message",
            // Expected text after replying to forsen
            "@forsen Test message "),

        // Ensure mention with comma is stripped, no message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "@forsen,",
            // Expected text after replying to forsen
            "@forsen "),

        // Ensure mention with comma is stripped, retain message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "@forsen Test message",
            // Expected text after replying to forsen
            "@forsen Test message "),

        // Ensure mention with comma and space is stripped, no message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "@forsen, ",
            // Expected text after replying to forsen
            "@forsen "),

        // Ensure it works with no message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "",
            // Expected text after replying to forsen
            "@forsen ")));
