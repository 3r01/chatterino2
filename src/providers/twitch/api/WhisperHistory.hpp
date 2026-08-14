// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"

#include <QColor>
#include <QDateTime>
#include <QString>

#include <functional>
#include <memory>
#include <vector>

class QObject;

namespace chatterino {

class Channel;
struct Message;
using MessagePtr = std::shared_ptr<const Message>;

namespace whisperhistory {

struct Participant {
    QString id;
    QString login;
    QString displayName;
    QColor color;
};

struct EmoteOccurrence {
    QString id;
    qsizetype from{};
    qsizetype to{};
};

struct Whisper {
    QString id;
    QString nonce;
    Participant sender;
    Participant recipient;
    QString text;
    QDateTime sentAt;
    QDateTime editedAt;
    QDateTime deletedAt;
    std::vector<EmoteOccurrence> emotes;
};

using SuccessCallback = std::function<void(std::vector<Whisper>)>;
using ErrorCallback = std::function<void(QString)>;

void load(const QString &userID, const QString &webOAuthToken,
          const QObject *caller, SuccessCallback onSuccess,
          ErrorCallback onError);

std::vector<MessagePtr> buildMessages(const std::vector<Whisper> &messages,
                                      Channel *channel,
                                      const QString &currentUserID,
                                      const QString &currentUserName);

}  // namespace whisperhistory
}  // namespace chatterino
