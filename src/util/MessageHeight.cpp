// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/MessageHeight.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/emoji/Emojis.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibrary>
#include <QMutex>
#include <QMutexLocker>

#include <cmath>
#include <memory>
#include <vector>

namespace chatterino {

namespace {

constexpr auto REVISION = "d4ac1cbd3b4fec54223a6cc8ccf5ca78ddb0338e";

class HeightEngine
{
public:
    MessageHeightResult measure(const QByteArray &request)
    {
        const QMutexLocker lock(&this->mutex_);
        if (!this->initialized_)
        {
            this->initialize();
            this->initialized_ = true;
        }
        if (!this->error_.isEmpty())
        {
            return {-1, this->error_};
        }
        const auto height = this->measure_(request.constData());
        if (!std::isfinite(height) || height < 0)
        {
            return {-1, "Couldn't measure this message."};
        }
        return {height, {}};
    }

private:
    void initialize()
    {
        const QDir directory(qEnvironmentVariable(
            "CHATTERINO_MESSAGE_HEIGHT_PATH",
            QCoreApplication::applicationDirPath() + "/message-height"));
        QFile revision(directory.filePath("upstream-revision"));
        if (!revision.open(QIODevice::ReadOnly) ||
            revision.readAll().trimmed() != REVISION)
        {
            this->error_ =
                "MessageHeightTwitch is missing or the wrong version. "
                "Build it with tools/message-height/build.ps1, "
                "then restart Chatterino.";
            return;
        }
#ifdef Q_OS_WIN
        // ICU looks for loaded DLLs by name, so load these first
        for (const auto *name : {"icudt78", "icuuc78"})
        {
            auto library = std::make_unique<QLibrary>(directory.filePath(name));
            library->setLoadHints(QLibrary::PreventUnloadHint);
            if (!library->load())
            {
                this->error_ = library->errorString();
                return;
            }
            this->dependencies_.push_back(std::move(library));
        }
#endif
        this->library_.setFileName(directory.filePath("MessageHeightTwitch"));
        this->library_.setLoadHints(QLibrary::PreventUnloadHint);
        const auto init = reinterpret_cast<int (*)(const char *)>(
            this->library_.resolve("ChatterinoHeightInit"));
        this->measure_ = reinterpret_cast<float (*)(const char *)>(
            this->library_.resolve("ChatterinoHeightMeasure"));
        if (init == nullptr || this->measure_ == nullptr)
        {
            this->error_ = this->library_.errorString();
            return;
        }
        if (init(directory.absolutePath().toUtf8().constData()) != 1)
        {
            this->error_ = "Couldn't load the MessageHeightTwitch fonts.";
        }
    }

    QMutex mutex_;
    bool initialized_ = false;
    QString error_;
    QLibrary library_;
    std::vector<std::unique_ptr<QLibrary>> dependencies_;
    float (*measure_)(const char *) = nullptr;
};

QJsonArray emoteSizes(const std::shared_ptr<const EmoteMap> &emotes,
                      const QString &text, bool &estimated, bool twitch = false)
{
    QJsonArray result;
    for (const auto &[name, emote] : *emotes)
    {
        if (!text.contains(name.string))
        {
            continue;
        }
        const auto &image = emote->images.getImage1();
        auto size = image->size();
        if (image->isEmpty())
        {
            size = QSizeF(28, 28);
            estimated = true;
        }
        else if (!image->loaded())
        {
            // Load the image so we can measure again with its actual size
            image->load();
            estimated = true;
        }
        QJsonObject entry{{"name", name.string},
                          {"width", size.width()},
                          {"height", size.height()},
                          {"zeroWidth", emote->zeroWidth}};
        if (twitch)
        {
            entry.insert("url", image->url().string);
        }
        result.append(entry);
    }
    return result;
}

QString channelKey(const QString &channel)
{
    auto key = channel.toLower();
    if (key.startsWith('#'))
    {
        key.remove(0, 1);
    }
    return key;
}

}  // namespace

MessageHeightRequest prepareMessageHeight(const TwitchChannel &channel,
                                          QString text)
{
    MessageHeightRequest request;
    const auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (account->isAnon())
    {
        request.error = "Log in to measure message height.";
        return request;
    }
    bool action = false;
    if (text.startsWith("/me ") || text.startsWith(".me "))
    {
        text.remove(0, 4);
        action = true;
    }
    else if (text.startsWith('/') || text.startsWith('.'))
    {
        request.error = "Can't measure commands other than /me.";
        return request;
    }
    text.replace('\n', ' ');
    text = getApp()->getEmotes()->getEmojis()->replaceShortCodes(text);
    if (text.toUcs4().size() > 500)
    {
        request.error = "Can't measure messages over 500 characters.";
        return request;
    }
    const auto info = channel.currentUserMessageInfo();
    request.estimated = !info.has_value();
    auto twitch =
        emoteSizes(*account->accessEmotes(), text, request.estimated, true);
    QJsonObject providers{
        {"twitch", twitch},
        {"bttv", emoteSizes(channel.bttvEmotes(), text, request.estimated)},
        {"ffz", emoteSizes(channel.ffzEmotes(), text, request.estimated)},
        {"seventv",
         emoteSizes(channel.seventvEmotes(), text, request.estimated)},
        {"bttvGlobal", emoteSizes(getApp()->getBttvEmotes()->emotes(), text,
                                  request.estimated)},
        {"ffzGlobal", emoteSizes(getApp()->getFfzEmotes()->emotes(), text,
                                 request.estimated)},
        {"seventvGlobal",
         emoteSizes(getApp()->getSeventvEmotes()->globalEmotes(), text,
                    request.estimated)},
    };
    const auto login = account->getUserName();
    const auto displayName =
        info && !info->displayName.isEmpty() ? info->displayName : login;
    QJsonObject json{{"text", text},
                     {"login", login},
                     {"displayName", displayName},
                     {"badges", info ? info->badgeCount : 0},
                     {"action", action},
                     {"providers", providers}};
    request.json = QJsonDocument(json).toJson(QJsonDocument::Compact);
    return request;
}

MessageHeightResult measureMessageHeight(const QByteArray &request)
{
    static HeightEngine engine;
    return engine.measure(request);
}

int messageHeightLimit(const QString &channel)
{
    const auto limits =
        QJsonDocument::fromJson(
            getSettings()->messageHeightLimits.getValue().toUtf8())
            .object();
    return std::max(0, limits.value(channelKey(channel)).toInt());
}

void setMessageHeightLimit(const QString &channel, int pixels)
{
    auto limits = QJsonDocument::fromJson(
                      getSettings()->messageHeightLimits.getValue().toUtf8())
                      .object();
    const auto key = channelKey(channel);
    if (pixels > 0)
    {
        limits.insert(key, pixels);
    }
    else
    {
        limits.remove(key);
    }
    getSettings()->messageHeightLimits =
        QString::fromUtf8(QJsonDocument(limits).toJson(QJsonDocument::Compact));
}

}  // namespace chatterino
