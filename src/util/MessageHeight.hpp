// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QByteArray>
#include <QString>

#include <vector>

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
    int overflowStart = -1;
};

// Read channel and account data on the GUI thread, then measure on a worker thread
MessageHeightRequest prepareMessageHeight(
    const TwitchChannel &channel, QString text,
    const std::vector<qsizetype> &sourceOffsets = {});
MessageHeightResult measureMessageHeight(const QByteArray &request);

int messageHeightLimit(const QString &channel);
void setMessageHeightLimit(const QString &channel, int pixels);

}  // namespace chatterino
