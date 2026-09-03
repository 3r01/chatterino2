// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/api/TwitchIntegrity.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QPointer>
#include <QTimer>
#include <QUuid>
#include <QWidget>

#if defined(Q_OS_WIN) && defined(CHATTERINO_3R01_BUILD)
#    include <Shlwapi.h>
#    include <WebView2.h>
#    include <Windows.h>
#    include <wrl.h>
#endif

#include <deque>
#include <optional>
#include <utility>

namespace chatterino::twitchgifs {

#if defined(Q_OS_WIN) && defined(CHATTERINO_3R01_BUILD)

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

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
    explicit TwitchIntegritySession(QObject *parent)
        : QObject(parent)
        , userDataDirectory_(QDir::temp().filePath(
              QStringLiteral("chatterino-twci-%1")
                  .arg(QUuid::createUuid().toString(QUuid::Id128))))
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
    }

    ~TwitchIntegritySession() override
    {
        if (this->controller_)
        {
            this->controller_->Close();
        }
        this->webView_.Reset();
        this->controller_.Reset();
        this->environment_.Reset();
        delete this->host_;
        QDir{this->userDataDirectory_}.removeRecursively();
        if (this->comInitialized_)
        {
            CoUninitialize();
        }
    }

    void warm()
    {
        if (!this->started_)
        {
            this->start();
        }
    }

    void enqueue(QJsonObject input, QString webOAuthToken,
                 const QObject *caller, IntegritySuccessCallback onSuccess,
                 IntegrityErrorCallback onError)
    {
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
        this->started_ = true;
        const auto result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(result) && result != RPC_E_CHANGED_MODE)
        {
            this->failAll(
                QStringLiteral("Unable to initialize Twitch integrity"));
            return;
        }
        this->comInitialized_ = SUCCEEDED(result);

        if (!QDir{}.mkpath(this->userDataDirectory_))
        {
            this->failAll(
                QStringLiteral("Unable to create Twitch integrity profile"));
            return;
        }

        this->host_ = new QWidget;
        this->host_->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
        this->host_->resize(800, 600);
        this->host_->move(-32000, -32000);
        this->host_->show();
        const auto window = reinterpret_cast<HWND>(this->host_->winId());
        const auto profile = this->userDataDirectory_.toStdWString();
        const QPointer self{this};

        this->startupTimeout_.start();
        const auto createResult = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, profile.c_str(), nullptr,
            Callback<
                ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [self, window](
                    HRESULT result,
                    ICoreWebView2Environment *environment) -> HRESULT {
                    if (!self)
                    {
                        return S_OK;
                    }
                    if (FAILED(result) || environment == nullptr)
                    {
                        self->failAll(QStringLiteral(
                            "Unable to start the Microsoft WebView2 runtime"));
                        return S_OK;
                    }
                    self->environment_ = environment;
                    environment->CreateCoreWebView2Controller(
                        window,
                        Callback<
                            ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [self](HRESULT controllerResult,
                                   ICoreWebView2Controller *controller)
                                -> HRESULT {
                                if (!self)
                                {
                                    return S_OK;
                                }
                                if (FAILED(controllerResult) ||
                                    controller == nullptr)
                                {
                                    self->failAll(QStringLiteral(
                                        "Unable to create Twitch integrity "
                                        "view"));
                                    return S_OK;
                                }
                                self->controller_ = controller;
                                self->initializeController();
                                return S_OK;
                            })
                            .Get());
                    return S_OK;
                })
                .Get());
        if (FAILED(createResult))
        {
            this->failAll(QStringLiteral(
                "Unable to launch the Microsoft WebView2 runtime"));
        }
    }

    void initializeController()
    {
        RECT bounds{0, 0, 800, 600};
        this->controller_->put_Bounds(bounds);
        this->controller_->put_IsVisible(TRUE);
        if (FAILED(this->controller_->get_CoreWebView2(
                this->webView_.ReleaseAndGetAddressOf())))
        {
            this->failAll(
                QStringLiteral("Unable to initialize Twitch integrity view"));
            return;
        }

        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(this->webView_->get_Settings(&settings)))
        {
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->put_AreDefaultScriptDialogsEnabled(FALSE);
            settings->put_AreDevToolsEnabled(FALSE);
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_IsZoomControlEnabled(FALSE);
        }

        const QPointer self{this};
        this->webView_->AddWebResourceRequestedFilter(
            L"https://www.twitch.tv/",
            COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT);
        EventRegistrationToken resourceToken{};
        this->webView_->add_WebResourceRequested(
            Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                [self](ICoreWebView2 *,
                       ICoreWebView2WebResourceRequestedEventArgs *args)
                    -> HRESULT {
                    if (!self)
                    {
                        return S_OK;
                    }
                    ComPtr<ICoreWebView2WebResourceRequest> request;
                    LPWSTR uri = nullptr;
                    if (FAILED(args->get_Request(
                            request.ReleaseAndGetAddressOf())) ||
                        FAILED(request->get_Uri(&uri)))
                    {
                        return S_OK;
                    }
                    const auto matches = QString::fromWCharArray(uri) ==
                                         QString::fromLatin1(PAGE_URL);
                    CoTaskMemFree(uri);
                    if (!matches)
                    {
                        return S_OK;
                    }
                    static constexpr unsigned char HTML[] =
                        "<!doctype html><body></body>";
                    ComPtr<IStream> stream;
                    stream.Attach(SHCreateMemStream(HTML, sizeof(HTML) - 1));
                    ComPtr<ICoreWebView2WebResourceResponse> response;
                    if (stream &&
                        SUCCEEDED(self->environment_->CreateWebResourceResponse(
                            stream.Get(), 200, L"OK",
                            L"Content-Type: text/html; charset=utf-8",
                            response.ReleaseAndGetAddressOf())))
                    {
                        args->put_Response(response.Get());
                    }
                    return S_OK;
                })
                .Get(),
            &resourceToken);

        EventRegistrationToken messageToken{};
        this->webView_->add_WebMessageReceived(
            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [self](
                    ICoreWebView2 *,
                    ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                    if (!self)
                    {
                        return S_OK;
                    }
                    LPWSTR source = nullptr;
                    LPWSTR message = nullptr;
                    if (FAILED(args->get_Source(&source)) ||
                        FAILED(args->TryGetWebMessageAsString(&message)))
                    {
                        CoTaskMemFree(source);
                        CoTaskMemFree(message);
                        self->finishError(QStringLiteral(
                            "Twitch integrity returned an invalid response"));
                        return S_OK;
                    }
                    const auto sourceValue = QString::fromWCharArray(source);
                    const auto messageValue = QString::fromWCharArray(message);
                    CoTaskMemFree(source);
                    CoTaskMemFree(message);
                    if (sourceValue == QString::fromLatin1(PAGE_URL))
                    {
                        try
                        {
                            self->handleMessage(messageValue);
                        }
                        catch (...)
                        {
                            self->finishError(QStringLiteral(
                                "Unable to process Twitch's GIF response"));
                        }
                    }
                    return S_OK;
                })
                .Get(),
            &messageToken);

        EventRegistrationToken navigationToken{};
        this->webView_->add_NavigationCompleted(
            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [self](ICoreWebView2 *,
                       ICoreWebView2NavigationCompletedEventArgs *args)
                    -> HRESULT {
                    if (!self)
                    {
                        return S_OK;
                    }
                    BOOL succeeded = FALSE;
                    args->get_IsSuccess(&succeeded);
                    if (succeeded)
                    {
                        self->initializePage();
                    }
                    else
                    {
                        self->failAll(QStringLiteral(
                            "Unable to open Twitch integrity context"));
                    }
                    return S_OK;
                })
                .Get(),
            &navigationToken);
        this->webView_->Navigate(L"https://www.twitch.tv/");
    }

    void initializePage()
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

        const QPointer self{this};
        this->webView_->ExecuteScript(
            script.toStdWString().c_str(),
            Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                [self](HRESULT result, LPCWSTR) -> HRESULT {
                    if (self && FAILED(result))
                    {
                        self->failAll(QStringLiteral(
                            "Unable to initialize Twitch integrity code"));
                    }
                    return S_OK;
                })
                .Get());
    }

    void processNext()
    {
        if (!this->ready_ || this->current_ || !this->webView_)
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
        auto script = QStringLiteral("window.__chatterinoSendGif(%1);")
                          .arg(QString::fromUtf8(QJsonDocument{config}.toJson(
                              QJsonDocument::Compact)));
        const auto nonce = request.nonce;
        const QPointer self{this};
        this->requestTimeout_.start();
        this->webView_->ExecuteScript(
            script.toStdWString().c_str(),
            Callback<ICoreWebView2ExecuteScriptCompletedHandler>(
                [self, nonce](HRESULT result, LPCWSTR) -> HRESULT {
                    if (self && FAILED(result) && self->current_ &&
                        self->current_->nonce == nonce)
                    {
                        self->finishError(QStringLiteral(
                            "Unable to execute Twitch integrity code"));
                    }
                    return S_OK;
                })
                .Get());
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
            this->ready_ = true;
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
        if (!root.value("ok").toBool())
        {
            auto errorMessage = root.value("error").toString();
            if (errorMessage.isEmpty())
            {
                errorMessage = QStringLiteral("Twitch integrity failed");
            }
            this->finishError(std::move(errorMessage));
            return;
        }

        const auto gqlDocument = QJsonDocument::fromJson(
            root.value("body").toString().toUtf8(), &error);
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
        this->startupTimeout_.stop();
        this->requestTimeout_.stop();
        this->ready_ = false;
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

    QString userDataDirectory_;
    QString deviceID_{QUuid::createUuid().toString(QUuid::Id128)};
    QString sessionID_{QUuid::createUuid().toString(QUuid::Id128)};
    QTimer startupTimeout_;
    QTimer requestTimeout_;
    QWidget *host_{};
    ComPtr<ICoreWebView2Environment> environment_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> webView_;
    std::deque<PendingRequest> queue_;
    std::optional<PendingRequest> current_;
    bool comInitialized_{};
    bool started_{};
    bool ready_{};
};

TwitchIntegritySession *getSession()
{
    static auto *session =
        new TwitchIntegritySession(QCoreApplication::instance());
    return session;
}

}  // namespace

#endif

#if defined(Q_OS_WIN) && defined(CHATTERINO_3R01_BUILD)
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
#else
void warmGifIntegritySession()
{
}

void sendGifWithIntegrity(const QJsonObject &input,
                          const QString &webOAuthToken, const QObject *caller,
                          IntegritySuccessCallback onSuccess,
                          IntegrityErrorCallback onError)
{
    (void)input;
    (void)webOAuthToken;
    (void)caller;
    (void)onSuccess;
    onError(
        QStringLiteral("GIF sending is currently only available on Windows"));
}
#endif

}  // namespace chatterino::twitchgifs
