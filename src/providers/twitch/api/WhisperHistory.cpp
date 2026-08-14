// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/api/WhisperHistory.hpp"

#include "common/Channel.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"

#include <IrcMessage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>

#include <algorithm>
#include <memory>
#include <utility>

namespace {

using namespace chatterino;
using namespace chatterino::whisperhistory;

constexpr auto GQL_URL = "https://gql.twitch.tv/gql";
constexpr auto TWITCH_WEB_CLIENT_ID = "kimne78kx3ncx6brgo4mv6wki5h1ko";
constexpr int THREADS_PER_PAGE = 100;
constexpr int MESSAGES_PER_PAGE = 100;
constexpr qsizetype MAX_MESSAGES = 1000;

const QString THREADS_QUERY = QStringLiteral(R"(
query ChatterinoWhisperThreads($threadsFirst: Int!, $messagesFirst: Int!) {
  currentUser {
    id
    whisperThreads(first: $threadsFirst) {
      edges {
        node {
          id
          participants { id login displayName chatColor }
          messages(first: $messagesFirst) {
            edges {
              node {
                id nonce sentAt editedAt deletedAt
                from { id }
                content {
                  content
                  emotes { id emoteID setID from to }
                }
              }
            }
          }
        }
      }
    }
  }
}
)");

QDateTime parseDate(const QJsonValue &value)
{
    auto date = QDateTime::fromString(value.toString(), Qt::ISODateWithMs);
    if (!date.isValid())
    {
        date = QDateTime::fromString(value.toString(), Qt::ISODate);
    }
    return date;
}

Participant parseParticipant(const QJsonObject &object)
{
    return {
        .id = object.value("id").toString(),
        .login = object.value("login").toString(),
        .displayName = object.value("displayName").toString(),
        .color = QColor(object.value("chatColor").toString()),
    };
}

QString graphqlError(const QJsonObject &root)
{
    const auto errors = root.value("errors").toArray();
    if (errors.isEmpty())
    {
        return {};
    }

    const auto message = errors.first().toObject().value("message").toString();
    return message.isEmpty() ? QStringLiteral("Twitch rejected the request")
                             : message;
}

QJsonObject makeRequest(const QString &operationName, const QString &query,
                        QJsonObject variables)
{
    return {
        {"operationName", operationName},
        {"query", query},
        {"variables", std::move(variables)},
    };
}

class Loader : public std::enable_shared_from_this<Loader>
{
public:
    Loader(QString userID, QString token, const QObject *caller,
           SuccessCallback onSuccess, ErrorCallback onError)
        : userID_(std::move(userID))
        , token_(std::move(token))
        , caller_(caller)
        , onSuccess_(std::move(onSuccess))
        , onError_(std::move(onError))
        , cutoff_(QDateTime::currentDateTimeUtc().addSecs(-24 * 60 * 60))
    {
    }

    void start()
    {
        QJsonObject variables{
            {"threadsFirst", THREADS_PER_PAGE},
            {"messagesFirst", MESSAGES_PER_PAGE},
        };
        this->execute(
            makeRequest("ChatterinoWhisperThreads", THREADS_QUERY,
                        std::move(variables)),
            [self = this->shared_from_this()](const QJsonObject &root) {
                self->handleThreadPage(root);
            });
    }

private:
    void execute(QJsonObject request,
                 std::function<void(const QJsonObject &)> onSuccess)
    {
        NetworkRequest(GQL_URL, NetworkRequestType::Post)
            .header("Client-ID", TWITCH_WEB_CLIENT_ID)
            .header("Authorization", "OAuth " + this->token_)
            .header("Accept", "application/json")
            .json(request)
            .timeout(20000)
            .caller(this->caller_)
            .onSuccess([self = this->shared_from_this(),
                        onSuccess =
                            std::move(onSuccess)](const NetworkResult &result) {
                const auto root = result.parseJson();
                if (const auto error = graphqlError(root); !error.isEmpty())
                {
                    self->fail(error);
                    return;
                }
                onSuccess(root);
            })
            .onError(
                [self = this->shared_from_this()](const NetworkResult &result) {
                    if (result.status() == 401)
                    {
                        self->fail(QStringLiteral("Twitch web token expired"));
                    }
                    else
                    {
                        self->fail(QStringLiteral("Network error: %1")
                                       .arg(result.formatError()));
                    }
                })
            .execute();
    }

    void handleThreadPage(const QJsonObject &root)
    {
        const auto currentUser =
            root.value("data").toObject().value("currentUser").toObject();
        if (currentUser.value("id").toString() != this->userID_)
        {
            this->fail(QStringLiteral("Twitch web token belongs to another "
                                      "account"));
            return;
        }

        for (const auto &edgeValue : currentUser.value("whisperThreads")
                                         .toObject()
                                         .value("edges")
                                         .toArray())
        {
            const auto thread = edgeValue.toObject().value("node").toObject();
            const auto participants = this->parseParticipants(thread);
            this->collectMessages(thread.value("messages").toObject(),
                                  participants);
        }
        this->finish();
    }

    std::vector<Participant> parseParticipants(const QJsonObject &thread) const
    {
        std::vector<Participant> participants;
        for (const auto &value : thread.value("participants").toArray())
        {
            participants.push_back(parseParticipant(value.toObject()));
        }
        return participants;
    }

    void collectMessages(const QJsonObject &connection,
                         const std::vector<Participant> &participants)
    {
        for (const auto &edgeValue : connection.value("edges").toArray())
        {
            const auto node = edgeValue.toObject().value("node").toObject();
            const auto sentAt = parseDate(node.value("sentAt"));
            if (!sentAt.isValid() || sentAt < this->cutoff_)
            {
                continue;
            }

            const auto id = node.value("id").toString();
            if (id.isEmpty() || this->seenMessageIDs_.contains(id) ||
                this->messages_.size() >= MAX_MESSAGES)
            {
                continue;
            }
            this->seenMessageIDs_.insert(id);

            Whisper message;
            message.id = id;
            message.nonce = node.value("nonce").toString();
            message.sentAt = sentAt;
            message.editedAt = parseDate(node.value("editedAt"));
            message.deletedAt = parseDate(node.value("deletedAt"));
            message.text =
                node.value("content").toObject().value("content").toString();

            const auto senderID =
                node.value("from").toObject().value("id").toString();
            for (const auto &participant : participants)
            {
                if (participant.id == senderID)
                {
                    message.sender = participant;
                }
                else
                {
                    message.recipient = participant;
                }
            }

            for (const auto &emoteValue :
                 node.value("content").toObject().value("emotes").toArray())
            {
                const auto emote = emoteValue.toObject();
                auto emoteID = emote.value("emoteID").toString();
                if (emoteID.isEmpty())
                {
                    emoteID = emote.value("id").toString();
                }
                message.emotes.push_back({
                    .id = std::move(emoteID),
                    .from = emote.value("from").toInt(),
                    .to = emote.value("to").toInt(),
                });
            }
            this->messages_.push_back(std::move(message));
        }
    }

    void finish()
    {
        if (std::exchange(this->finished_, true))
        {
            return;
        }
        std::ranges::sort(this->messages_, {}, &Whisper::sentAt);
        this->token_.clear();
        this->onSuccess_(std::move(this->messages_));
    }

    void fail(QString error)
    {
        if (std::exchange(this->finished_, true))
        {
            return;
        }
        this->token_.clear();
        this->onError_(std::move(error));
    }

    QString userID_;
    QString token_;
    const QObject *caller_;
    SuccessCallback onSuccess_;
    ErrorCallback onError_;
    QDateTime cutoff_;
    QSet<QString> seenMessageIDs_;
    std::vector<Whisper> messages_;
    bool finished_{};
};

QString escapeTag(QString value)
{
    value.replace('\\', "\\\\");
    value.replace(';', "\\:");
    value.replace(' ', "\\s");
    value.replace('\r', "\\r");
    value.replace('\n', "\\n");
    return value;
}

QString emoteTag(const std::vector<EmoteOccurrence> &emotes)
{
    QMap<QString, QStringList> occurrences;
    for (const auto &emote : emotes)
    {
        if (!emote.id.isEmpty())
        {
            occurrences[emote.id].append(
                QString("%1-%2").arg(emote.from).arg(emote.to));
        }
    }

    QStringList groups;
    for (auto it = occurrences.cbegin(); it != occurrences.cend(); ++it)
    {
        groups.append(it.key() + ':' + it.value().join(','));
    }
    return groups.join('/');
}

QString participantName(const Participant &participant)
{
    return participant.displayName.isEmpty() ? participant.login
                                             : participant.displayName;
}

}  // namespace

namespace chatterino::whisperhistory {

void load(const QString &userID, const QString &webOAuthToken,
          const QObject *caller, SuccessCallback onSuccess,
          ErrorCallback onError)
{
    std::make_shared<Loader>(userID, webOAuthToken, caller,
                             std::move(onSuccess), std::move(onError))
        ->start();
}

std::vector<MessagePtr> buildMessages(const std::vector<Whisper> &messages,
                                      Channel *channel,
                                      const QString &currentUserID,
                                      const QString &currentUserName)
{
    std::vector<MessagePtr> built;
    built.reserve(messages.size());

    for (const auto &item : messages)
    {
        auto content = item.text;
        MessageParseArgs args;
        args.disablePingSounds = true;
        args.isReceivedWhisper = item.sender.id != currentUserID;
        args.isSentWhisper = !args.isReceivedWhisper;
        args.sentWhisperRecipient = participantName(item.recipient);
        if (content.startsWith(QChar(0x01) + QStringLiteral("ACTION ")) &&
            content.endsWith(QChar(0x01)))
        {
            content = content.sliced(8, content.size() - 9);
            args.isAction = true;
        }

        auto login = item.sender.login;
        if (login.isEmpty())
        {
            login = item.sender.id;
        }
        QStringList tags{
            "id=" + escapeTag(item.id),
            "user-id=" + escapeTag(item.sender.id),
            "display-name=" + escapeTag(participantName(item.sender)),
            "tmi-sent-ts=" + QString::number(item.sentAt.toMSecsSinceEpoch()),
            "historical=1",
        };
        if (item.sender.color.isValid())
        {
            tags.append("color=" + item.sender.color.name());
        }
        if (!item.emotes.empty())
        {
            tags.append("emotes=" + emoteTag(item.emotes));
        }
        if (item.deletedAt.isValid())
        {
            tags.append("rm-deleted=1");
        }

        const auto raw =
            QString("@%1 :%2!%2@%2.tmi.twitch.tv WHISPER %3 :%4")
                .arg(tags.join(';'), login, currentUserName, content);
        auto *ircMessage = Communi::IrcMessage::fromData(raw.toUtf8(), nullptr);
        auto [message, alert] = MessageBuilder::makeIrcMessage(
            channel, ircMessage, args, content, 0);
        ircMessage->deleteLater();
        if (!message)
        {
            continue;
        }

        message->flags.set(MessageFlag::Whisper, MessageFlag::RecentMessage,
                           MessageFlag::DoNotTriggerNotification,
                           MessageFlag::DoNotLog);
        built.push_back(std::move(message));
    }
    return built;
}

}  // namespace chatterino::whisperhistory
