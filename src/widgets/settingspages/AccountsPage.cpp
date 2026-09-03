// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/AccountsPage.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/accounts/AccountModel.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchCommon.hpp"
#include "util/Clipboard.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/dialogs/LoginDialog.hpp"
#include "widgets/dialogs/TwitchWebLoginDialog.hpp"
#include "widgets/helper/EditableModelView.hpp"

#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

#include <algorithm>

namespace chatterino {

AccountsPage::AccountsPage()
{
    auto *app = getApp();

    LayoutCreator<AccountsPage> layoutCreator(this);
    auto layout = layoutCreator.emplace<QVBoxLayout>().withoutMargin();

    EditableModelView *view =
        layout
            .emplace<EditableModelView>(
                app->getAccounts()->createModel(nullptr), false)
            .getElement();

    view->getTableView()->horizontalHeader()->setVisible(false);
    view->getTableView()->horizontalHeader()->setStretchLastSection(true);

    // We can safely ignore this signal connection since we own the view
    std::ignore = view->addButtonPressed.connect([this] {
        LoginDialog d(this);
        d.exec();
    });

    view->getTableView()->setStyleSheet("background: #333");

    auto *historyGroup = new QGroupBox("Twitch sign-in", this);
    auto *historyLayout = new QVBoxLayout(historyGroup);
    auto *description = new QLabel(
        "Sign in here to set up chat, recent whisper history, and GIF search "
        "and sending for the selected account. You do not need to copy any "
        "tokens from your browser.",
        historyGroup);
    description->setWordWrap(true);
    historyLayout->addWidget(description);

    auto *status = new QLabel(historyGroup);
    historyLayout->addWidget(status);

    auto *signIn = new QPushButton("Sign in with Twitch", historyGroup);
    historyLayout->addWidget(signIn);

    auto *manualDescription = new QLabel(
        "If the sign-in window is unavailable, copy the auth-token cookie "
        "from a signed-in twitch.tv browser session and paste it here.",
        historyGroup);
    manualDescription->setWordWrap(true);
    historyLayout->addWidget(manualDescription);

    auto *tokenRow = new QHBoxLayout;
    auto *tokenInput = new QLineEdit(historyGroup);
    tokenInput->setEchoMode(QLineEdit::Password);
    tokenInput->setPlaceholderText("Twitch auth-token cookie");
    tokenRow->addWidget(tokenInput);
    auto *saveToken = new QPushButton("Save token", historyGroup);
    auto *clearToken = new QPushButton("Clear token", historyGroup);
    tokenRow->addWidget(saveToken);
    tokenRow->addWidget(clearToken);
    historyLayout->addLayout(tokenRow);
    layout->addWidget(historyGroup);

    const auto updateHistoryControls = [=] {
        const auto account = app->getAccounts()->twitch.getCurrent();
        const auto available = account && !account->isAnon();
        signIn->setEnabled(available);
        tokenInput->setEnabled(available);
        saveToken->setEnabled(available);
        clearToken->setEnabled(available &&
                               !account->getWebOAuthToken().isEmpty());

        if (!available)
        {
            status->setText("Select a Twitch account to sign in.");
        }
        else if (account->getWebOAuthToken().isEmpty())
        {
            status->setText(QString("Web access is not set up for %1.")
                                .arg(account->getUserName()));
        }
        else
        {
            status->setText(
                QString("Signed in as %1.").arg(account->getUserName()));
        }
    };

    QObject::connect(
        signIn, &QPushButton::clicked, this,
        [this, app, status, updateHistoryControls] {
            const auto selected = app->getAccounts()->twitch.getCurrent();
            if (!selected || selected->isAnon())
            {
                return;
            }
            const auto expectedUserID = selected->getUserId();
            openTwitchWebLogin(this, [this, app, status, selected,
                                      expectedUserID, updateHistoryControls](
                                         TwitchWebCredentials credentials) {
                if (credentials.userID != expectedUserID)
                {
                    status->setText(
                        QString(
                            "You signed in as %1. Sign in as %2 to update the "
                            "selected account.")
                            .arg(credentials.username,
                                 selected->getUserName()));
                    return;
                }
                app->getAccounts()->twitch.saveUser({
                    .username = credentials.username,
                    .userID = credentials.userID,
                    .clientID = credentials.clientID,
                    .oauthToken = credentials.oauthToken,
                    .webOAuthToken = credentials.webOAuthToken,
                });
                updateHistoryControls();
            });
        });

    QObject::connect(saveToken, &QPushButton::clicked, this, [=] {
        auto token = tokenInput->text().trimmed();
        if (token.startsWith("auth-token=", Qt::CaseInsensitive))
        {
            token.remove(0, 11);
        }
        if (token.startsWith("oauth:", Qt::CaseInsensitive))
        {
            token.remove(0, 6);
        }
        token = token.trimmed();
        if (token.isEmpty())
        {
            return;
        }
        app->getAccounts()->twitch.setCurrentWebOAuthToken(token);
        tokenInput->clear();
        crossPlatformCopy({});
        updateHistoryControls();
    });
    QObject::connect(clearToken, &QPushButton::clicked, this, [=] {
        app->getAccounts()->twitch.setCurrentWebOAuthToken({});
        updateHistoryControls();
    });

    this->managedConnections_.managedConnect(
        app->getAccounts()->twitch.currentUserChanged, updateHistoryControls);
    this->managedConnections_.managedConnect(
        app->getAccounts()->twitch.webOAuthTokenChanged, updateHistoryControls);
    updateHistoryControls();

    //    auto buttons = layout.emplace<QDialogButtonBox>();
    //    {
    //        this->addButton = buttons->addButton("Add",
    //        QDialogButtonBox::YesRole); this->removeButton =
    //        buttons->addButton("Remove", QDialogButtonBox::NoRole);
    //    }

    //    layout.emplace<AccountSwitchWidget>(this).assign(&this->accSwitchWidget);

    // ----
    //    QObject::connect(this->addButton, &QPushButton::clicked, []() {
    //        static auto loginWidget = new LoginWidget();
    //        loginWidget->show();
    //    });

    //    QObject::connect(this->removeButton, &QPushButton::clicked, [this] {
    //        auto selectedUser = this->accSwitchWidget->currentItem()->text();
    //        if (selectedUser == ANONYMOUS_USERNAME_LABEL) {
    //            // Do nothing
    //            return;
    //        }

    //        getApp()->getAccounts()->Twitch.removeUser(selectedUser);
    //    });
}

}  // namespace chatterino
