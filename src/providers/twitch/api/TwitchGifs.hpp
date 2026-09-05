// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"
#include "util/RapidjsonHelpers.hpp"

#include <pajlada/serialize.hpp>
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

    bool operator==(const SearchResult &other) const = default;
};

struct SearchPage {
    std::vector<SearchResult> results;
    int nextOffset{};
    bool hasMore{};
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
using SearchCallback = std::function<void(SearchPage)>;
using SendCallback = std::function<void(SendResult)>;
using SendErrorCallback = std::function<void(SendError)>;
using ErrorCallback = std::function<void(QString)>;

void loadPickerConfig(const QString &channelID, const QString &webOAuthToken,
                      const QObject *caller, ConfigCallback onSuccess,
                      ErrorCallback onError);

void search(const QString &query, const PickerConfig &config, int offset,
            const QObject *caller, SearchCallback onSuccess,
            ErrorCallback onError);

std::vector<SearchResult> favouriteGifs();
std::vector<SearchResult> recentlySentGifs();
bool isFavourite(const QString &id);
void setFavourite(const SearchResult &gif, bool favourite);
void recordSent(const SearchResult &gif);

void send(const QString &channelID, const QString &gifID, const QString &gifURL,
          const QString &searchTerm, const QString &webOAuthToken,
          const QObject *caller, SendCallback onSuccess,
          SendErrorCallback onError);

}  // namespace chatterino::twitchgifs

namespace pajlada {

template <>
struct Serialize<chatterino::twitchgifs::SearchResult> {
    static rapidjson::Value get(
        const chatterino::twitchgifs::SearchResult &value,
        rapidjson::Document::AllocatorType &a)
    {
        rapidjson::Value result(rapidjson::kObjectType);
        chatterino::rj::set(result, "id", value.id, a);
        chatterino::rj::set(result, "title", value.title, a);
        chatterino::rj::set(result, "searchTerm", value.searchTerm, a);
        chatterino::rj::set(result, "url", value.url.string, a);
        chatterino::rj::set(result, "previewUrl", value.previewUrl.string, a);
        chatterino::rj::set(result, "previewWidth", value.previewSize.width(),
                            a);
        chatterino::rj::set(result, "previewHeight", value.previewSize.height(),
                            a);
        return result;
    }
};

template <>
struct Deserialize<chatterino::twitchgifs::SearchResult> {
    static chatterino::twitchgifs::SearchResult get(
        const rapidjson::Value &value, bool *error = nullptr)
    {
        QString id;
        QString title;
        QString searchTerm;
        QString url;
        QString previewUrl;
        int previewWidth{};
        int previewHeight{};
        if (!value.IsObject() || !chatterino::rj::getSafe(value, "id", id) ||
            !chatterino::rj::getSafe(value, "title", title) ||
            !chatterino::rj::getSafe(value, "searchTerm", searchTerm) ||
            !chatterino::rj::getSafe(value, "url", url) ||
            !chatterino::rj::getSafe(value, "previewUrl", previewUrl) ||
            !chatterino::rj::getSafe(value, "previewWidth", previewWidth) ||
            !chatterino::rj::getSafe(value, "previewHeight", previewHeight))
        {
            PAJLADA_REPORT_ERROR(error);
            return {};
        }
        return {
            .id = std::move(id),
            .title = std::move(title),
            .searchTerm = std::move(searchTerm),
            .url = chatterino::Url{std::move(url)},
            .previewUrl = chatterino::Url{std::move(previewUrl)},
            .previewSize = QSize{previewWidth, previewHeight},
        };
    }
};

}  // namespace pajlada
