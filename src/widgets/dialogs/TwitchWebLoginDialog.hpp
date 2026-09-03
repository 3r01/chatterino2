// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

#include <functional>

class QWidget;

namespace chatterino {

struct TwitchWebCredentials {
    QString username;
    QString userID;
    QString clientID;
    QString oauthToken;
    QString webOAuthToken;
};

using TwitchWebLoginCallback = std::function<void(TwitchWebCredentials)>;

void openTwitchWebLogin(QWidget *parent, TwitchWebLoginCallback onSuccess);

}  // namespace chatterino
