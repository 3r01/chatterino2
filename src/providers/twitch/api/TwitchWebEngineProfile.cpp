// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/api/TwitchWebEngineProfile.hpp"

#ifdef CHATTERINO_HAS_QT_WEBENGINE

#    include <QCoreApplication>
#    include <QDir>
#    include <QRegularExpression>
#    include <QStandardPaths>
#    include <QWebEngineCookieStore>
#    include <QWebEngineProfile>

namespace chatterino {

namespace {

std::optional<QNetworkCookie> &authCookie()
{
    static std::optional<QNetworkCookie> cookie;
    return cookie;
}

bool isTwitchAuthCookie(const QNetworkCookie &cookie)
{
    const auto domain = cookie.domain().toLower();
    return cookie.name() == "auth-token" &&
           (domain == u"twitch.tv" || domain.endsWith(u".twitch.tv"));
}

}  // namespace

QWebEngineProfile *twitchWebEngineProfile()
{
    static auto *profile = [] {
        const auto profileRoot =
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
            QStringLiteral("/twitch-integrity-webengine");
        if (!QDir{}.mkpath(profileRoot))
        {
            return static_cast<QWebEngineProfile *>(nullptr);
        }

        auto *result = new QWebEngineProfile(QStringLiteral("twitch-integrity"),
                                             QCoreApplication::instance());
        result->setCachePath(profileRoot + QStringLiteral("/cache"));
        result->setPersistentStoragePath(profileRoot +
                                         QStringLiteral("/storage"));
        result->setPersistentCookiesPolicy(
            QWebEngineProfile::AllowPersistentCookies);
        result->setHttpCacheMaximumSize(32 * 1024 * 1024);

        auto userAgent = result->httpUserAgent();
        userAgent.remove(
            QRegularExpression{QStringLiteral(R"(QtWebEngine/[^ ]+\s*)")});
        result->setHttpUserAgent(userAgent);

        auto *cookies = result->cookieStore();
        QObject::connect(
            cookies, &QWebEngineCookieStore::cookieAdded, result,
            [](const QNetworkCookie &cookie) {
                if (isTwitchAuthCookie(cookie))
                {
                    authCookie() = cookie;
                }
            },
            Qt::QueuedConnection);
        QObject::connect(
            cookies, &QWebEngineCookieStore::cookieRemoved, result,
            [](const QNetworkCookie &cookie) {
                if (authCookie() == cookie)
                {
                    authCookie().reset();
                }
            },
            Qt::QueuedConnection);
        return result;
    }();
    return profile;
}

std::optional<QNetworkCookie> twitchWebEngineAuthCookie()
{
    return authCookie();
}

}  // namespace chatterino

#endif
