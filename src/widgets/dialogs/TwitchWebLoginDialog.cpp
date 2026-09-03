// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/TwitchWebLoginDialog.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
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

#include <memory>
#include <optional>
#include <utility>

namespace chatterino {

#if defined(Q_OS_WIN) && defined(CHATTERINO_3R01_BUILD)

namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr auto LOGIN_PROFILE_PREFIX = "chatterino-twitch-login-";

void removeProfileWithRetries(QString path, int retries = 20)
{
    if (!QFileInfo::exists(path) || QDir{path}.removeRecursively())
    {
        return;
    }
    if (retries <= 0)
    {
        return;
    }
    QTimer::singleShot(500, QCoreApplication::instance(),
                       [path = std::move(path), retries] {
                           removeProfileWithRetries(path, retries - 1);
                       });
}

void removeAbandonedLoginProfiles()
{
    const QDir temporaryDirectory{QDir::tempPath()};
    const auto cutoff = QDateTime::currentDateTimeUtc().addDays(-1);
    const auto entries = temporaryDirectory.entryInfoList(
        {QString::fromLatin1(LOGIN_PROFILE_PREFIX) + u'*'},
        QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto &entry : entries)
    {
        if (entry.lastModified().toUTC() < cutoff)
        {
            removeProfileWithRetries(entry.absoluteFilePath());
        }
    }
}

struct ProfileCleanupState {
    ComPtr<ICoreWebView2Environment5> environment;
    EventRegistrationToken processExitedToken{};
    QString path;
    bool uninitializeCom{};
};

bool removeProfileAfterBrowserExit(
    const ComPtr<ICoreWebView2Environment> &environment, const QString &path,
    bool uninitializeCom)
{
    auto state = std::make_shared<ProfileCleanupState>();
    if (FAILED(environment.As(&state->environment)))
    {
        return false;
    }
    state->path = path;
    state->uninitializeCom = uninitializeCom;
    const auto result = state->environment->add_BrowserProcessExited(
        Callback<ICoreWebView2BrowserProcessExitedEventHandler>(
            [state](ICoreWebView2Environment *,
                    ICoreWebView2BrowserProcessExitedEventArgs *) -> HRESULT {
                state->environment->remove_BrowserProcessExited(
                    state->processExitedToken);
                state->environment.Reset();
                const auto path = std::move(state->path);
                const auto uninitializeCom = state->uninitializeCom;
                state->uninitializeCom = false;
                QTimer::singleShot(0, QCoreApplication::instance(),
                                   [path, uninitializeCom] {
                                       removeProfileWithRetries(path);
                                       if (uninitializeCom)
                                       {
                                           CoUninitialize();
                                       }
                                   });
                return S_OK;
            })
            .Get(),
        &state->processExitedToken);
    return SUCCEEDED(result);
}

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
        removeAbandonedLoginProfiles();
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
        this->resetButton_ =
            buttons->addButton("Start over", QDialogButtonBox::ResetRole);
        this->resetButton_->hide();
        QObject::connect(this->resetButton_, &QPushButton::clicked, this,
                         [this] {
                             this->resetAuthentication();
                         });
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
        this->cookiePoll_.stop();
        const auto cleanupAfterExit =
            this->environment_ && this->controller_ &&
            removeProfileAfterBrowserExit(this->environment_,
                                          this->userDataDirectory_,
                                          this->comInitialized_);
        if (this->controller_)
        {
            this->controller_->Close();
        }
        this->webView_.Reset();
        this->controller_.Reset();
        this->environment_.Reset();
        if (!cleanupAfterExit)
        {
            removeProfileWithRetries(this->userDataDirectory_);
            if (this->comInitialized_)
            {
                CoUninitialize();
            }
        }
        this->comInitialized_ = false;
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
            token == this->lastRejectedChatToken_ ||
            this->nextChatValidation_ > QDateTime::currentDateTimeUtc())
        {
            return;
        }
        this->validatingChatToken_ = true;
        const auto generation = this->authGeneration_;
        this->status_->setText(QStringLiteral("Finishing account setup..."));
        const QPointer self{this};
        NetworkRequest("https://id.twitch.tv/oauth2/validate")
            .header("Authorization", "OAuth " + token)
            .timeout(20000)
            .caller(this)
            .onSuccess(
                [self, token, generation](const NetworkResult &result) mutable {
                    if (!self || self->authGeneration_ != generation)
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
            .onError([self, token, generation](const NetworkResult &result) {
                if (!self || self->authGeneration_ != generation)
                {
                    return;
                }
                self->validatingChatToken_ = false;
                const auto status = result.status();
                if (status == 401 || status == 403)
                {
                    self->lastRejectedChatToken_ = token;
                    self->showError(QStringLiteral(
                        "Twitch rejected the Chatterino authorization. Start "
                        "over and try again."));
                    self->resetButton_->show();
                }
                else
                {
                    self->nextChatValidation_ =
                        QDateTime::currentDateTimeUtc().addSecs(5);
                    self->showError(
                        QStringLiteral("Could not validate the Chatterino "
                                       "authorization (%1). Retrying...")
                            .arg(result.formatError()));
                    QTimer::singleShot(5100, self, [self, token, generation] {
                        if (self && self->authGeneration_ == generation)
                        {
                            self->validateChatToken(token);
                        }
                    });
                }
            })
            .execute();
    }

    void validateWebToken(const QString &token)
    {
        if (this->validatingWebToken_ || !this->webToken_.isEmpty() ||
            token == this->lastRejectedWebToken_ ||
            this->nextWebValidation_ > QDateTime::currentDateTimeUtc())
        {
            return;
        }
        this->validatingWebToken_ = true;
        const auto generation = this->authGeneration_;
        const QPointer self{this};
        NetworkRequest("https://id.twitch.tv/oauth2/validate")
            .header("Authorization", "OAuth " + token)
            .timeout(20000)
            .caller(this)
            .onSuccess([self, token,
                        generation](const NetworkResult &result) mutable {
                if (!self || self->authGeneration_ != generation)
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
            .onError([self, token, generation](const NetworkResult &result) {
                if (!self || self->authGeneration_ != generation)
                {
                    return;
                }
                self->validatingWebToken_ = false;
                const auto status = result.status();
                if (status == 401 || status == 403)
                {
                    self->lastRejectedWebToken_ = token;
                    self->showError(QStringLiteral(
                        "Twitch rejected the web sign-in. Start over and try "
                        "again."));
                    self->resetButton_->show();
                }
                else
                {
                    self->nextWebValidation_ =
                        QDateTime::currentDateTimeUtc().addSecs(5);
                    self->showError(
                        QStringLiteral("Could not validate the Twitch web "
                                       "sign-in (%1). Retrying...")
                            .arg(result.formatError()));
                }
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
                "to different accounts. Start over and sign in to the same "
                "account."));
            this->resetButton_->show();
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

    void resetAuthentication()
    {
        ++this->authGeneration_;
        this->credentials_.reset();
        this->chatToken_.clear();
        this->webToken_.clear();
        this->webUserID_.clear();
        this->lastRejectedChatToken_.clear();
        this->lastRejectedWebToken_.clear();
        this->nextChatValidation_ = {};
        this->nextWebValidation_ = {};
        this->validatingChatToken_ = false;
        this->validatingWebToken_ = false;
        this->resetButton_->hide();

        ComPtr<ICoreWebView2_2> webView2;
        ComPtr<ICoreWebView2CookieManager> cookieManager;
        if (this->webView_ && SUCCEEDED(this->webView_.As(&webView2)) &&
            SUCCEEDED(webView2->get_CookieManager(
                cookieManager.ReleaseAndGetAddressOf())))
        {
            cookieManager->DeleteCookies(L"auth-token",
                                         L"https://www.twitch.tv/");
        }
        this->status_->setText(QStringLiteral(
            "Sign in to Twitch. Chatterino will finish setting up the account "
            "automatically."));
        if (this->webView_)
        {
            this->webView_->Navigate(L"https://chatterino.com/client_login");
        }
    }

    TwitchWebLoginCallback onSuccess_;
    QString userDataDirectory_;
    std::optional<TwitchWebCredentials> credentials_;
    QString chatToken_;
    QString webToken_;
    QString webUserID_;
    QString lastRejectedChatToken_;
    QString lastRejectedWebToken_;
    QDateTime nextChatValidation_;
    QDateTime nextWebValidation_;
    QLabel *status_{};
    QPushButton *resetButton_{};
    QWidget *host_{};
    QTimer cookiePoll_;
    ComPtr<ICoreWebView2Environment> environment_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> webView_;
    bool comInitialized_{};
    bool validatingChatToken_{};
    bool validatingWebToken_{};
    quint64 authGeneration_{};
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
