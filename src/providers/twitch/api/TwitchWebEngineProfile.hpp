// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QNetworkCookie>

#include <optional>

class QWebEngineProfile;

namespace chatterino {

QWebEngineProfile *twitchWebEngineProfile();
std::optional<QNetworkCookie> twitchWebEngineAuthCookie();

}  // namespace chatterino
