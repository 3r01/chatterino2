// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"

#include <QSize>
#include <QString>

#include <functional>
#include <vector>

class QObject;

namespace chatterino::twitchgifs {

struct PickerConfig {
    bool isEnabled{};
    bool isAllowlisted{};
    bool canSend{};
    QString apiKey;
    QString contentRating;
};

struct SearchResult {
    QString id;
    QString title;
    QString searchTerm;
    Url url;
    Url previewUrl;
    QSize previewSize;
};

struct SendResult {
    QString messageID;
    int secondsUntilCanSend{};
};

struct SendError {
    QString message;
    int secondsUntilCanSend{};
};

using ConfigCallback = std::function<void(PickerConfig)>;
using SearchCallback = std::function<void(std::vector<SearchResult>)>;
using SendCallback = std::function<void(SendResult)>;
using SendErrorCallback = std::function<void(SendError)>;
using ErrorCallback = std::function<void(QString)>;

void loadPickerConfig(const QString &channelID, const QString &webOAuthToken,
                      const QObject *caller, ConfigCallback onSuccess,
                      ErrorCallback onError);

void search(const QString &query, const PickerConfig &config,
            const QObject *caller, SearchCallback onSuccess,
            ErrorCallback onError);

void send(const QString &channelID, const QString &gifID, const QString &gifURL,
          const QString &searchTerm, const QString &webOAuthToken,
          const QObject *caller, SendCallback onSuccess,
          SendErrorCallback onError);

}  // namespace chatterino::twitchgifs
