// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QJsonObject>
#include <QString>

#include <functional>

class QObject;

namespace chatterino::twitchgifs {

using IntegritySuccessCallback = std::function<void(QJsonObject)>;
using IntegrityErrorCallback = std::function<void(QString)>;

void warmGifIntegritySession();

void sendGifWithIntegrity(const QJsonObject &input,
                          const QString &webOAuthToken, const QObject *caller,
                          IntegritySuccessCallback onSuccess,
                          IntegrityErrorCallback onError);

}  // namespace chatterino::twitchgifs
