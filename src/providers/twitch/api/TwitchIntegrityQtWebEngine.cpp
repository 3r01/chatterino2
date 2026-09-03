// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/api/TwitchIntegrity.hpp"

#ifdef CHATTERINO_HAS_QT_WEBENGINE

#    include <QCoreApplication>
#    include <QDir>
#    include <QFile>
#    include <QJsonDocument>
#    include <QPointer>
#    include <QRegularExpression>
#    include <QStandardPaths>
#    include <QTimer>
#    include <QUuid>
#    include <QVariant>
#    include <QWebEnginePage>
#    include <QWebEngineProfile>
#    include <QWebEngineView>
#    include <QWidget>

#    include <deque>
#    include <optional>
#    include <utility>

namespace chatterino::twitchgifs {

namespace {

constexpr auto CLIENT_ID = "kimne78kx3ncx6brgo4mv6wki5h1ko";
constexpr auto PAGE_URL = "https://www.twitch.tv/";
constexpr auto KASADA_SCRIPT_URL =
    "https://k.twitchcdn.net/149e9513-01fa-4fb0-aad4-566afd725d1b/"
    "2d206a39-8ed7-437e-a3be-862e0f06eea3/p.js";

struct PendingRequest {
    QJsonObject input;
    QString webOAuthToken;
    QPointer<QObject> caller;
    IntegritySuccessCallback onSuccess;
    IntegrityErrorCallback onError;
    QString nonce;
};

class TwitchIntegritySession final : public QObject
{
public:
    enum class State {
        Idle,
        Starting,
        Ready,
        Failed,
    };

    explicit TwitchIntegritySession(QObject *parent)
        : QObject(parent)
    {
        this->startupTimeout_.setSingleShot(true);
        this->startupTimeout_.setInterval(60000);
        QObject::connect(&this->startupTimeout_, &QTimer::timeout, this,
                         [this] {
                             this->failAll(QStringLiteral(
                                 "Twitch integrity initialization timed out"));
                         });

        this->requestTimeout_.setSingleShot(true);
        this->requestTimeout_.setInterval(60000);
        QObject::connect(
            &this->requestTimeout_, &QTimer::timeout, this, [this] {
                this->finishError(
                    QStringLiteral("Twitch integrity request timed out"));
            });

        this->messagePoll_.setInterval(25);
        QObject::connect(&this->messagePoll_, &QTimer::timeout, this, [this] {
            this->pollMessage();
        });

        QObject::connect(QCoreApplication::instance(),
                         &QCoreApplication::aboutToQuit, this, [this] {
                             this->startupTimeout_.stop();
                             this->requestTimeout_.stop();
                             this->messagePoll_.stop();
                             this->destroyBrowser();
                             delete this->profile_;
                             this->profile_ = nullptr;
                         });
    }

    ~TwitchIntegritySession() override
    {
        this->destroyBrowser();
    }

    void warm()
    {
        if (this->state_ == State::Idle)
        {
            this->start();
        }
    }

    void enqueue(QJsonObject input, QString webOAuthToken,
                 const QObject *caller, IntegritySuccessCallback onSuccess,
                 IntegrityErrorCallback onError)
    {
        if (this->state_ == State::Failed)
        {
            this->resetSession();
        }
        this->queue_.push_back({
            .input = std::move(input),
            .webOAuthToken = std::move(webOAuthToken),
            .caller = QPointer<QObject>{const_cast<QObject *>(caller)},
            .onSuccess = std::move(onSuccess),
            .onError = std::move(onError),
            .nonce = QUuid::createUuid().toString(QUuid::Id128),
        });
        this->warm();
        this->processNext();
    }

private:
    void start()
    {
        this->state_ = State::Starting;
        const auto generation = ++this->generation_;

        this->host_ = new QWidget;
        this->host_->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
        this->host_->resize(800, 600);
        this->host_->move(-32000, -32000);

        if (this->profile_ == nullptr)
        {
            this->profile_ =
                new QWebEngineProfile(QStringLiteral("twitch-integrity"));
            const auto profileRoot =
                QStandardPaths::writableLocation(
                    QStandardPaths::CacheLocation) +
                QStringLiteral("/twitch-integrity-webengine");
            if (!QDir{}.mkpath(profileRoot))
            {
                this->failAll(QStringLiteral(
                    "Unable to create Twitch integrity profile"));
                return;
            }
            this->profile_->setCachePath(profileRoot +
                                         QStringLiteral("/cache"));
            this->profile_->setPersistentStoragePath(
                profileRoot + QStringLiteral("/storage"));
            this->profile_->setPersistentCookiesPolicy(
                QWebEngineProfile::AllowPersistentCookies);
            this->profile_->setHttpCacheMaximumSize(32 * 1024 * 1024);

            auto userAgent = this->profile_->httpUserAgent();
            userAgent.remove(
                QRegularExpression{QStringLiteral(R"(QtWebEngine/[^ ]+\s*)")});
            this->profile_->setHttpUserAgent(userAgent);
        }

        this->view_ = new QWebEngineView(this->profile_, this->host_);
        this->view_->setGeometry(this->host_->rect());
        this->page_ = this->view_->page();
        this->host_->show();
        this->view_->show();

        const QPointer self{this};
        QObject::connect(
            this->view_, &QWebEngineView::loadFinished, this,
            [self, generation](bool succeeded) {
                if (!self || self->generation_ != generation ||
                    self->pageInitialized_)
                {
                    return;
                }
                if (!succeeded)
                {
                    self->failAll(QStringLiteral(
                        "Unable to open Twitch integrity context"));
                    return;
                }
                self->pageInitialized_ = true;
                self->initializePage(generation);
            });

        this->startupTimeout_.start();
        this->view_->load(QUrl{QString::fromLatin1(PAGE_URL)});
    }

    void initializePage(quint64 generation)
    {
        const QJsonObject setup{{"kasadaScriptURL", KASADA_SCRIPT_URL}};
        QFile scriptFile(QStringLiteral(":/twitch-integrity.js"));
        if (!scriptFile.open(QIODevice::ReadOnly))
        {
            this->failAll(
                QStringLiteral("Unable to read Twitch integrity code"));
            return;
        }
        auto script = QString::fromUtf8(scriptFile.readAll());
        script.replace(QStringLiteral("__SETUP__"),
                       QString::fromUtf8(QJsonDocument{setup}.toJson(
                           QJsonDocument::Compact)));
        script += QStringLiteral("\ntrue;");

        this->messagePoll_.start();
        const QPointer self{this};
        this->page_->runJavaScript(
            script, [self, generation](const QVariant &result) {
                if (!self || self->generation_ != generation)
                {
                    return;
                }
                if (!result.toBool())
                {
                    self->failAll(QStringLiteral(
                        "Unable to initialize Twitch integrity code"));
                }
            });
    }

    void pollMessage()
    {
        if (this->page_ == nullptr || this->pollInFlight_)
        {
            return;
        }
        this->pollInFlight_ = true;
        const auto generation = this->generation_;
        const QPointer self{this};
        this->page_->runJavaScript(
            QStringLiteral("window.__chatterinoTakeMessage?.() ?? ''"),
            [self, generation](const QVariant &result) {
                if (!self || self->generation_ != generation)
                {
                    return;
                }
                self->pollInFlight_ = false;
                const auto message = result.toString();
                if (!message.isEmpty())
                {
                    self->handleMessage(message);
                }
            });
    }

    void processNext()
    {
        if (this->state_ != State::Ready || this->current_ ||
            this->page_ == nullptr)
        {
            return;
        }
        while (!this->queue_.empty() && this->queue_.front().caller.isNull())
        {
            this->queue_.pop_front();
        }
        if (this->queue_.empty())
        {
            return;
        }

        this->current_ = std::move(this->queue_.front());
        this->queue_.pop_front();
        const auto &request = *this->current_;
        const QJsonObject gqlBody{
            {"operationName", "sendGifMessage"},
            {"query",
             "mutation sendGifMessage($input: SendGifMessageInput!) { "
             "sendGifMessage(input: $input) { error secondsUntilCanSend "
             "message { id } } }"},
            {"variables", QJsonObject{{"input", request.input}}},
        };
        const QJsonObject headers{
            {"Client-ID", CLIENT_ID},
            {"Authorization", "OAuth " + request.webOAuthToken},
            {"X-Device-ID", this->deviceID_},
            {"Client-Session-ID", this->sessionID_},
            {"Client-Request-ID", QUuid::createUuid().toString(QUuid::Id128)},
        };
        const QJsonObject config{
            {"authorization", "OAuth " + request.webOAuthToken},
            {"nonce", request.nonce},
            {"headers", headers},
            {"gqlBody", gqlBody},
        };
        const auto script =
            QStringLiteral("window.__chatterinoSendGif(%1); true;")
                .arg(QString::fromUtf8(
                    QJsonDocument{config}.toJson(QJsonDocument::Compact)));
        const auto nonce = request.nonce;
        const QPointer self{this};
        this->requestTimeout_.start();
        this->page_->runJavaScript(
            script, [self, nonce](const QVariant &result) {
                if (self && self->current_ && self->current_->nonce == nonce &&
                    !result.toBool())
                {
                    self->finishError(QStringLiteral(
                        "Unable to execute Twitch integrity code"));
                }
            });
    }

    void handleMessage(const QString &message)
    {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(message.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
        {
            this->finishError(QStringLiteral(
                "Twitch integrity returned an invalid response"));
            return;
        }
        const auto root = document.object();
        const auto type = root.value("type").toString();
        if (type == u"ready")
        {
            this->startupTimeout_.stop();
            this->state_ = State::Ready;
            this->processNext();
            return;
        }
        if (type == u"startup-error")
        {
            auto errorMessage = root.value("error").toString();
            if (errorMessage.isEmpty())
            {
                errorMessage =
                    QStringLiteral("Unable to load Twitch integrity code");
            }
            this->failAll(std::move(errorMessage));
            return;
        }
        if (type != u"result" || !this->current_ ||
            root.value("nonce").toString() != this->current_->nonce)
        {
            return;
        }
        const auto body = root.value("body").toString();
        const auto status = root.value("status").toInt();
        if (!root.value("ok").toBool())
        {
            auto errorMessage = root.value("error").toString();
            const auto gqlDocument =
                QJsonDocument::fromJson(body.toUtf8(), &error);
            if (error.error == QJsonParseError::NoError &&
                gqlDocument.isObject())
            {
                this->finishSuccess(gqlDocument.object());
                return;
            }
            if (errorMessage.isEmpty() && status > 0)
            {
                errorMessage = QStringLiteral("Twitch request failed (HTTP %1)")
                                   .arg(status);
            }
            else if (errorMessage.isEmpty())
            {
                errorMessage = QStringLiteral("Twitch integrity failed");
            }
            this->finishError(std::move(errorMessage));
            return;
        }

        const auto gqlDocument = QJsonDocument::fromJson(body.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !gqlDocument.isObject())
        {
            this->finishError(
                QStringLiteral("Twitch returned an invalid GIF response"));
            return;
        }
        this->finishSuccess(gqlDocument.object());
    }

    void finishSuccess(QJsonObject response)
    {
        if (!this->current_)
        {
            return;
        }
        this->requestTimeout_.stop();
        auto request = std::move(*this->current_);
        this->current_.reset();
        QTimer::singleShot(
            0, this,
            [caller = request.caller, callback = std::move(request.onSuccess),
             response = std::move(response)]() mutable {
                if (!caller.isNull() && callback)
                {
                    callback(std::move(response));
                }
            });
        QTimer::singleShot(0, this, [this] {
            this->processNext();
        });
    }

    void finishError(QString error)
    {
        if (!this->current_)
        {
            return;
        }
        this->requestTimeout_.stop();
        auto request = std::move(*this->current_);
        this->current_.reset();
        QTimer::singleShot(
            0, this,
            [caller = request.caller, callback = std::move(request.onError),
             error = std::move(error)]() mutable {
                if (!caller.isNull() && callback)
                {
                    callback(std::move(error));
                }
            });
        QTimer::singleShot(0, this, [this] {
            this->processNext();
        });
    }

    void failAll(QString error)
    {
        ++this->generation_;
        this->startupTimeout_.stop();
        this->requestTimeout_.stop();
        this->messagePoll_.stop();
        this->pollInFlight_ = false;
        this->state_ = State::Failed;
        if (this->current_)
        {
            auto request = std::move(*this->current_);
            this->current_.reset();
            QTimer::singleShot(
                0, this,
                [caller = request.caller, callback = std::move(request.onError),
                 error]() mutable {
                    if (!caller.isNull() && callback)
                    {
                        callback(std::move(error));
                    }
                });
        }
        while (!this->queue_.empty())
        {
            auto request = std::move(this->queue_.front());
            this->queue_.pop_front();
            QTimer::singleShot(
                0, this,
                [caller = request.caller, callback = std::move(request.onError),
                 error]() mutable {
                    if (!caller.isNull() && callback)
                    {
                        callback(std::move(error));
                    }
                });
        }
    }

    void resetSession()
    {
        ++this->generation_;
        this->startupTimeout_.stop();
        this->requestTimeout_.stop();
        this->messagePoll_.stop();
        this->pollInFlight_ = false;
        this->destroyBrowser();
        this->pageInitialized_ = false;
        this->state_ = State::Idle;
    }

    void destroyBrowser()
    {
        delete this->view_;
        this->view_ = nullptr;
        this->page_ = nullptr;
        delete this->host_;
        this->host_ = nullptr;
    }

    QString deviceID_{QUuid::createUuid().toString(QUuid::Id128)};
    QString sessionID_{QUuid::createUuid().toString(QUuid::Id128)};
    QTimer startupTimeout_;
    QTimer requestTimeout_;
    QTimer messagePoll_;
    QWidget *host_{};
    QWebEngineView *view_{};
    QWebEnginePage *page_{};
    QWebEngineProfile *profile_{};
    std::deque<PendingRequest> queue_;
    std::optional<PendingRequest> current_;
    bool pageInitialized_{};
    bool pollInFlight_{};
    State state_{State::Idle};
    quint64 generation_{};
};

TwitchIntegritySession *getSession()
{
    static auto *session =
        new TwitchIntegritySession(QCoreApplication::instance());
    return session;
}

}  // namespace

void warmGifIntegritySession()
{
    getSession()->warm();
}

void sendGifWithIntegrity(const QJsonObject &input,
                          const QString &webOAuthToken, const QObject *caller,
                          IntegritySuccessCallback onSuccess,
                          IntegrityErrorCallback onError)
{
    getSession()->enqueue(input, webOAuthToken, caller, std::move(onSuccess),
                          std::move(onError));
}

}  // namespace chatterino::twitchgifs

#endif
