// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QByteArray>
#include <QString>

namespace chatterino {

class TwitchChannel;

struct MessageHeightRequest {
    QByteArray json;
    QString error;
    bool estimated = false;
};

struct MessageHeightResult {
    float pixels = -1;
    QString error;
};

// Read channel and account data on the GUI thread, then measure on a worker thread
MessageHeightRequest prepareMessageHeight(const TwitchChannel &channel,
                                          QString text);
MessageHeightResult measureMessageHeight(const QByteArray &request);

int messageHeightLimit(const QString &channel);
void setMessageHeightLimit(const QString &channel, int pixels);

}  // namespace chatterino
