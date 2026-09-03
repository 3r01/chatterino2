// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/api/TwitchGifs.hpp"

#include "common/QLogging.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "providers/twitch/api/TwitchIntegrity.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace chatterino::twitchgifs {

namespace {

constexpr auto GQL_URL = "https://gql.twitch.tv/gql";
constexpr auto TWITCH_WEB_CLIENT_ID = "kimne78kx3ncx6brgo4mv6wki5h1ko";
constexpr auto GIPHY_SEARCH_URL = "https://api.giphy.com/v1/gifs/search";
constexpr auto GIPHY_TRENDING_URL = "https://api.giphy.com/v1/gifs/trending";
constexpr int GIPHY_REQUEST_LIMIT = 50;
constexpr int SEARCH_RESULT_LIMIT = 12;

const QString PICKER_CONFIG_QUERY = QStringLiteral(R"(
query getGifPickerConfig($channelID: ID!) {
  gifPickerConfig(channelID: $channelID) {
    isEnabled
    isAllowlisted
    apiKey
    contentRating
  }
  user(id: $channelID) {
    self {
      subscriptionBenefit {
        tier
      }
    }
  }
}
)");

QJsonObject makeGqlRequest(const QString &operationName, const QString &query,
                           QJsonObject variables)
{
    return {
        {"operationName", operationName},
        {"query", query},
        {"variables", std::move(variables)},
    };
}

QString gqlError(const QJsonObject &root)
{
    const auto errors = root.value("errors").toArray();
    if (errors.isEmpty())
    {
        return {};
    }

    qCWarning(chatterinoTwitch).noquote()
        << "Twitch GIF GraphQL error response:"
        << QJsonDocument{root}.toJson(QJsonDocument::Compact);

    QStringList messages;
    for (const auto &value : errors)
    {
        const auto error = value.toObject();
        auto message = error.value("message").toString();
        if (message.isEmpty())
        {
            message = QStringLiteral("Twitch rejected the request");
        }

        const auto extensions = error.value("extensions").toObject();
        if (!extensions.isEmpty())
        {
            message += QStringLiteral(" (%1)")
                           .arg(QString::fromUtf8(
                               QJsonDocument{extensions}.toJson(
                                   QJsonDocument::Compact)));
        }
        messages.emplace_back(std::move(message));
    }
    return messages.join(QStringLiteral("; "));
}

QString giphyRating(QStringView contentRating)
{
    if (contentRating == u"PG_13")
    {
        return QStringLiteral("pg-13");
    }
    if (contentRating == u"G_PG")
    {
        return QStringLiteral("pg");
    }
    return QStringLiteral("g");
}

bool isPickerRating(QStringView rating)
{
    return rating.compare(u"y", Qt::CaseInsensitive) == 0 ||
           rating.compare(u"g", Qt::CaseInsensitive) == 0 ||
           rating.compare(u"pg", Qt::CaseInsensitive) == 0;
}

const QString &giphyPingbackID()
{
    static const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return id;
}

template <typename Success>
void gqlRequest(QJsonObject body, const QString &token, const QObject *caller,
                Success onSuccess, ErrorCallback onError)
{
    NetworkRequest(GQL_URL, NetworkRequestType::Post)
        .header("Client-ID", TWITCH_WEB_CLIENT_ID)
        .header("Authorization", "OAuth " + token)
        .header("Accept", "application/json")
        .json(body)
        .timeout(20000)
        .caller(caller)
        .onSuccess([onSuccess = std::move(onSuccess),
                    onError](const NetworkResult &result) {
            const auto root = result.parseJson();
            if (const auto error = gqlError(root); !error.isEmpty())
            {
                onError(error);
                return;
            }
            onSuccess(root);
        })
        .onError([onError = std::move(onError)](const NetworkResult &result) {
            if (result.status() == 401)
            {
                onError(QStringLiteral("Twitch web token expired"));
            }
            else
            {
                onError(QStringLiteral("Network error: %1")
                            .arg(result.formatError()));
            }
        })
        .execute();
}

SearchResult parseSearchResult(const QJsonObject &item)
{
    const auto images = item.value("images").toObject();
    const auto original = images.value("original").toObject();
    auto preview = images.value("fixed_height").toObject();
    if (preview.isEmpty())
    {
        preview = images.value("fixed_width").toObject();
    }

    return {
        .id = item.value("id").toString(),
        .title = item.value("title").toString(),
        .url = Url{original.value("url").toString()},
        .previewUrl = Url{preview.value("url").toString()},
        .previewSize = QSize{preview.value("width").toString().toInt(),
                             preview.value("height").toString().toInt()},
    };
}

}  // namespace

void loadPickerConfig(const QString &channelID, const QString &webOAuthToken,
                      const QObject *caller, ConfigCallback onSuccess,
                      ErrorCallback onError)
{
    gqlRequest(
        makeGqlRequest("getGifPickerConfig", PICKER_CONFIG_QUERY,
                       {{"channelID", channelID}}),
        webOAuthToken, caller,
        [onSuccess = std::move(onSuccess)](const QJsonObject &root) {
            const auto data = root.value("data").toObject();
            const auto config = data.value("gifPickerConfig").toObject();
            const auto tier = data.value("user")
                                  .toObject()
                                  .value("self")
                                  .toObject()
                                  .value("subscriptionBenefit")
                                  .toObject()
                                  .value("tier")
                                  .toString()
                                  .toInt();
            onSuccess({
                .isEnabled = config.value("isEnabled").toBool(),
                .isAllowlisted = config.value("isAllowlisted").toBool(),
                .canSend = tier >= 2000,
                .apiKey = config.value("apiKey").toString(),
                .contentRating = config.value("contentRating").toString(),
            });
        },
        std::move(onError));
}

void search(const QString &query, const PickerConfig &config,
            const QObject *caller, SearchCallback onSuccess,
            ErrorCallback onError)
{
    const auto searchTerm = query.trimmed();
    QUrl url(searchTerm.isEmpty() ? GIPHY_TRENDING_URL : GIPHY_SEARCH_URL);
    QUrlQuery urlQuery;
    urlQuery.addQueryItem("api_key", config.apiKey);
    urlQuery.addQueryItem("pingback_id", giphyPingbackID());
    urlQuery.addQueryItem("rating", giphyRating(config.contentRating));
    urlQuery.addQueryItem("limit", QString::number(GIPHY_REQUEST_LIMIT));
    urlQuery.addQueryItem("offset", "0");
    if (!searchTerm.isEmpty())
    {
        urlQuery.addQueryItem("q", searchTerm);
    }
    url.setQuery(urlQuery);

    NetworkRequest(url)
        .timeout(15000)
        .caller(caller)
        .onSuccess([searchTerm, onSuccess = std::move(onSuccess),
                    onError](const NetworkResult &result) {
            const auto root = result.parseJson();
            const auto data = root.value("data").toArray();
            if (data.isEmpty() && root.contains("message"))
            {
                onError(root.value("message").toString());
                return;
            }

            std::vector<SearchResult> results;
            results.reserve(data.size());
            bool hasAd = false;
            for (const auto &value : data)
            {
                const auto object = value.toObject();
                if (!isPickerRating(object.value("rating").toString()))
                {
                    continue;
                }
                if (object.value("is_ad").toBool())
                {
                    // Twitch omits ads from searches and permits only the first
                    // ad in the trending feed.
                    if (!searchTerm.isEmpty() || hasAd)
                    {
                        continue;
                    }
                    hasAd = true;
                }
                auto item = parseSearchResult(object);
                item.searchTerm = searchTerm;
                if (!item.id.isEmpty() && !item.title.trimmed().isEmpty() &&
                    !item.url.string.isEmpty() &&
                    !item.previewUrl.string.isEmpty())
                {
                    results.emplace_back(std::move(item));
                    if (results.size() == SEARCH_RESULT_LIMIT)
                    {
                        break;
                    }
                }
            }
            onSuccess(std::move(results));
        })
        .onError([onError = std::move(onError)](const NetworkResult &result) {
            onError(QStringLiteral("GIPHY request failed: %1")
                        .arg(result.formatError()));
        })
        .execute();
}

void send(const QString &channelID, const QString &gifID, const QString &gifURL,
          const QString &searchTerm, const QString &webOAuthToken,
          const QObject *caller, SendCallback onSuccess,
          SendErrorCallback onError)
{
    QJsonObject input{
        {"channelID", channelID}, {"gifID", gifID}, {"gifURL", gifURL}};
    if (!searchTerm.isEmpty())
    {
        input.insert("searchTerm", searchTerm);
    }
    auto responseError = onError;
    auto integrityError = std::move(onError);
    sendGifWithIntegrity(
        input, webOAuthToken, caller,
        [onSuccess = std::move(onSuccess),
         onError = std::move(responseError)](
            const QJsonObject &root) mutable {
            if (const auto error = gqlError(root); !error.isEmpty())
            {
                onError({.message = error});
                return;
            }
            const auto result = root.value("data")
                                    .toObject()
                                    .value("sendGifMessage")
                                    .toObject();
            if (result.isEmpty())
            {
                onError({.message =
                             QStringLiteral("Twitch rejected the GIF request")});
                return;
            }
            if (const auto error = result.value("error").toString();
                !error.isEmpty())
            {
                qCWarning(chatterinoTwitch).noquote()
                    << "Twitch GIF send result:"
                    << QJsonDocument{root}.toJson(QJsonDocument::Compact);
                const auto retryAfter =
                    result.value("secondsUntilCanSend").toInt();
                onError({.message = error,
                         .secondsUntilCanSend = std::max(0, retryAfter)});
                return;
            }

            const auto messageID =
                result.value("message").toObject().value("id").toString();
            if (messageID.isEmpty())
            {
                onError(
                    {.message = QStringLiteral("Twitch did not send the GIF")});
                return;
            }
            onSuccess({
                .messageID = messageID,
                .secondsUntilCanSend =
                    std::max(0, result.value("secondsUntilCanSend").toInt()),
            });
        },
        [onError = std::move(integrityError)](QString error) {
            onError({.message = std::move(error)});
        });
}

}  // namespace chatterino::twitchgifs
