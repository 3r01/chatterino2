// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/TwitchWebLoginDialog.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"

#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QLabel>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QVBoxLayout>
#include <QWidget>

#if defined(Q_OS_WIN) && defined(CHATTERINO_3R01_BUILD)
#    include <Shlwapi.h>
#    include <WebView2.h>
#    include <Windows.h>
#    include <wrl.h>
#endif

#include <optional>
#include <utility>

namespace chatterino {

#if defined(Q_OS_WIN) && defined(CHATTERINO_3R01_BUILD)

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

class TwitchWebLoginDialog final : public QDialog
{
public:
    TwitchWebLoginDialog(QWidget *parent, TwitchWebLoginCallback onSuccess)
        : QDialog(parent)
        , onSuccess_(std::move(onSuccess))
        , userDataDirectory_(QDir::temp().filePath(
              QStringLiteral("chatterino-twitch-login-%1")
                  .arg(QUuid::createUuid().toString(QUuid::Id128))))
    {
        this->setAttribute(Qt::WA_DeleteOnClose);
        this->setWindowTitle(QStringLiteral("Sign in with Twitch"));
        this->resize(900, 700);

        auto *layout = new QVBoxLayout(this);
        this->status_ = new QLabel(
            QStringLiteral("Sign in to Twitch. Chatterino will finish setting "
                           "up the account automatically."),
            this);
        this->status_->setWordWrap(true);
        layout->addWidget(this->status_);

        this->host_ = new QWidget(this);
        this->host_->setMinimumSize(640, 480);
        this->host_->installEventFilter(this);
        layout->addWidget(this->host_, 1);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
        QObject::connect(buttons, &QDialogButtonBox::rejected, this,
                         &QDialog::reject);
        layout->addWidget(buttons);

        this->cookiePoll_.setInterval(1000);
        QObject::connect(&this->cookiePoll_, &QTimer::timeout, this, [this] {
            this->checkForWebToken();
        });

        QTimer::singleShot(0, this, [this] {
            this->start();
        });
    }

    ~TwitchWebLoginDialog() override
    {
        if (this->controller_)
        {
            this->controller_->Close();
        }
        this->webView_.Reset();
        this->controller_.Reset();
        this->environment_.Reset();
        QDir{this->userDataDirectory_}.removeRecursively();
        if (this->comInitialized_)
        {
            CoUninitialize();
        }
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == this->host_ && event->type() == QEvent::Resize)
        {
            this->updateBounds();
        }
        return QDialog::eventFilter(watched, event);
    }

private:
    void start()
    {
        const auto result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(result) && result != RPC_E_CHANGED_MODE)
        {
            this->showError(QStringLiteral(
                "Unable to initialize the Twitch sign-in window."));
            return;
        }
        this->comInitialized_ = SUCCEEDED(result);

        if (!QDir{}.mkpath(this->userDataDirectory_))
        {
            this->showError(QStringLiteral(
                "Unable to create a temporary sign-in profile."));
            return;
        }

        const auto window = reinterpret_cast<HWND>(this->host_->winId());
        const auto profile = this->userDataDirectory_.toStdWString();
        const QPointer self{this};
        const auto createResult = CreateCoreWebView2EnvironmentWithOptions(
            nullptr, profile.c_str(), nullptr,
            Callback<
                ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [self, window](
                    HRESULT environmentResult,
                    ICoreWebView2Environment *environment) -> HRESULT {
                    if (!self)
                    {
                        return S_OK;
                    }
                    if (FAILED(environmentResult) || environment == nullptr)
                    {
                        self->showError(QStringLiteral(
                            "Unable to start the Microsoft WebView2 runtime."));
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
                                    self->showError(QStringLiteral(
                                        "Unable to create the Twitch sign-in "
                                        "window."));
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
            this->showError(QStringLiteral(
                "Unable to launch the Microsoft WebView2 runtime."));
        }
    }

    void initializeController()
    {
        this->updateBounds();
        this->controller_->put_IsVisible(TRUE);
        if (FAILED(this->controller_->get_CoreWebView2(
                this->webView_.ReleaseAndGetAddressOf())))
        {
            this->showError(QStringLiteral(
                "Unable to initialize the Twitch sign-in window."));
            return;
        }

        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(this->webView_->get_Settings(
                settings.ReleaseAndGetAddressOf())))
        {
            settings->put_AreDevToolsEnabled(FALSE);
            settings->put_IsStatusBarEnabled(FALSE);
        }

        const QPointer self{this};
        EventRegistrationToken navigationStartingToken{};
        this->webView_->add_NavigationStarting(
            Callback<ICoreWebView2NavigationStartingEventHandler>(
                [self](
                    ICoreWebView2 *,
                    ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                    if (!self)
                    {
                        return S_OK;
                    }
                    LPWSTR uri = nullptr;
                    if (FAILED(args->get_Uri(&uri)))
                    {
                        return S_OK;
                    }
                    const QUrl url{QString::fromWCharArray(uri)};
                    CoTaskMemFree(uri);
                    if (url.host() == QStringLiteral("chatterino.com"))
                    {
                        const QUrlQuery fragment{url.fragment()};
                        const auto token = fragment.queryItemValue(
                            QStringLiteral("access_token"));
                        if (!token.isEmpty())
                        {
                            self->validateChatToken(token);
                        }
                    }
                    return S_OK;
                })
                .Get(),
            &navigationStartingToken);

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
                        self->checkForWebToken();
                    }
                    return S_OK;
                })
                .Get(),
            &navigationToken);

        this->webView_->Navigate(L"https://chatterino.com/client_login");
        this->cookiePoll_.start();
    }

    void updateBounds()
    {
        if (!this->controller_ || this->host_ == nullptr)
        {
            return;
        }
        const auto size = this->host_->size();
        const RECT bounds{0, 0, size.width(), size.height()};
        this->controller_->put_Bounds(bounds);
    }

    void checkForWebToken()
    {
        if (this->validatingWebToken_ || !this->webView_ ||
            !this->webToken_.isEmpty())
        {
            return;
        }
        ComPtr<ICoreWebView2_2> webView2;
        if (FAILED(this->webView_.As(&webView2)))
        {
            return;
        }
        ComPtr<ICoreWebView2CookieManager> cookieManager;
        if (FAILED(webView2->get_CookieManager(
                cookieManager.ReleaseAndGetAddressOf())))
        {
            return;
        }

        const QPointer self{this};
        cookieManager->GetCookies(
            L"https://www.twitch.tv/",
            Callback<ICoreWebView2GetCookiesCompletedHandler>(
                [self](HRESULT result,
                       ICoreWebView2CookieList *cookies) -> HRESULT {
                    if (!self || FAILED(result) || cookies == nullptr)
                    {
                        return S_OK;
                    }
                    UINT count = 0;
                    cookies->get_Count(&count);
                    for (UINT index = 0; index < count; ++index)
                    {
                        ComPtr<ICoreWebView2Cookie> cookie;
                        if (FAILED(cookies->GetValueAtIndex(
                                index, cookie.ReleaseAndGetAddressOf())))
                        {
                            continue;
                        }
                        LPWSTR name = nullptr;
                        LPWSTR value = nullptr;
                        if (FAILED(cookie->get_Name(&name)) ||
                            FAILED(cookie->get_Value(&value)))
                        {
                            CoTaskMemFree(name);
                            CoTaskMemFree(value);
                            continue;
                        }
                        const auto cookieName = QString::fromWCharArray(name);
                        const auto cookieValue = QString::fromWCharArray(value);
                        CoTaskMemFree(name);
                        CoTaskMemFree(value);
                        if (cookieName == QStringLiteral("auth-token") &&
                            !cookieValue.isEmpty())
                        {
                            self->validateWebToken(cookieValue);
                            break;
                        }
                    }
                    return S_OK;
                })
                .Get());
    }

    void validateChatToken(const QString &token)
    {
        if (this->validatingChatToken_ || !this->chatToken_.isEmpty() ||
            token == this->lastRejectedChatToken_)
        {
            return;
        }
        this->validatingChatToken_ = true;
        this->status_->setText(QStringLiteral("Finishing account setup..."));
        const QPointer self{this};
        NetworkRequest("https://id.twitch.tv/oauth2/validate")
            .header("Authorization", "OAuth " + token)
            .timeout(20000)
            .caller(this)
            .onSuccess([self, token](const NetworkResult &result) mutable {
                if (!self)
                {
                    return;
                }
                const auto json = result.parseJson();
                TwitchWebCredentials credentials{
                    .username = json.value("login").toString(),
                    .userID = json.value("user_id").toString(),
                    .clientID = json.value("client_id").toString(),
                    .oauthToken = std::move(token),
                };
                if (credentials.username.isEmpty() ||
                    credentials.userID.isEmpty() ||
                    credentials.clientID.isEmpty())
                {
                    self->validatingChatToken_ = false;
                    self->showError(QStringLiteral(
                        "Twitch returned incomplete account information."));
                    return;
                }
                self->chatToken_ = credentials.oauthToken;
                self->credentials_ = std::move(credentials);
                self->validatingChatToken_ = false;
                self->finishIfReady();
            })
            .onError([self, token](const NetworkResult &) {
                if (!self)
                {
                    return;
                }
                self->validatingChatToken_ = false;
                self->lastRejectedChatToken_ = token;
                self->showError(QStringLiteral("Twitch rejected the Chatterino "
                                               "authorization. Try again."));
            })
            .execute();
    }

    void validateWebToken(const QString &token)
    {
        if (this->validatingWebToken_ || !this->webToken_.isEmpty() ||
            token == this->lastRejectedWebToken_)
        {
            return;
        }
        this->validatingWebToken_ = true;
        const QPointer self{this};
        NetworkRequest("https://id.twitch.tv/oauth2/validate")
            .header("Authorization", "OAuth " + token)
            .timeout(20000)
            .caller(this)
            .onSuccess([self, token](const NetworkResult &result) mutable {
                if (!self)
                {
                    return;
                }
                const auto userID =
                    result.parseJson().value("user_id").toString();
                if (userID.isEmpty())
                {
                    self->validatingWebToken_ = false;
                    self->showError(QStringLiteral(
                        "Twitch returned incomplete web account information."));
                    return;
                }
                self->webUserID_ = userID;
                self->webToken_ = std::move(token);
                self->validatingWebToken_ = false;
                self->finishIfReady();
            })
            .onError([self, token](const NetworkResult &) {
                if (!self)
                {
                    return;
                }
                self->validatingWebToken_ = false;
                self->lastRejectedWebToken_ = token;
                self->showError(
                    QStringLiteral("Twitch rejected the web sign-in. Sign out "
                                   "and try again."));
            })
            .execute();
    }

    void finishIfReady()
    {
        if (!this->credentials_.has_value() || this->webToken_.isEmpty())
        {
            return;
        }
        if (this->credentials_->userID != this->webUserID_)
        {
            this->showError(QStringLiteral(
                "The Chatterino authorization and Twitch web sign-in belong "
                "to different accounts. Sign out and try again."));
            return;
        }

        this->credentials_->webOAuthToken = this->webToken_;
        auto credentials = std::move(*this->credentials_);
        auto callback = std::move(this->onSuccess_);
        this->accept();
        callback(std::move(credentials));
    }

    void showError(const QString &message)
    {
        this->status_->setText(message);
    }

    TwitchWebLoginCallback onSuccess_;
    QString userDataDirectory_;
    std::optional<TwitchWebCredentials> credentials_;
    QString chatToken_;
    QString webToken_;
    QString webUserID_;
    QString lastRejectedChatToken_;
    QString lastRejectedWebToken_;
    QLabel *status_{};
    QWidget *host_{};
    QTimer cookiePoll_;
    ComPtr<ICoreWebView2Environment> environment_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> webView_;
    bool comInitialized_{};
    bool validatingChatToken_{};
    bool validatingWebToken_{};
};

}  // namespace

#endif

void openTwitchWebLogin(QWidget *parent, TwitchWebLoginCallback onSuccess)
{
#if defined(Q_OS_WIN) && defined(CHATTERINO_3R01_BUILD)
    auto *dialog = new TwitchWebLoginDialog(parent, std::move(onSuccess));
    dialog->show();
#else
    (void)onSuccess;
    auto *dialog = new QDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Sign in with Twitch"));
    auto *layout = new QVBoxLayout(dialog);
    auto *label = new QLabel(
        QStringLiteral("Integrated Twitch sign-in is currently only available "
                       "on Windows."),
        dialog);
    label->setWordWrap(true);
    layout->addWidget(label);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog,
                     &QDialog::reject);
    layout->addWidget(buttons);
    dialog->show();
#endif
}

}  // namespace chatterino
