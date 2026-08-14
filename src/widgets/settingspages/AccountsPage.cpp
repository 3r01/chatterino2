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

    auto *historyGroup = new QGroupBox("Whisper history", this);
    auto *historyLayout = new QVBoxLayout(historyGroup);
    auto *description = new QLabel(
        "Paste Twitch's auth-token cookie for the current account to load "
        "recent whispers on startup.",
        historyGroup);
    description->setWordWrap(true);
    historyLayout->addWidget(description);

    auto *status = new QLabel(historyGroup);
    historyLayout->addWidget(status);

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
        tokenInput->setEnabled(available);
        saveToken->setEnabled(available);
        clearToken->setEnabled(available &&
                               !account->getWebOAuthToken().isEmpty());
        tokenInput->clear();

        if (!available)
        {
            status->setText("Select a Twitch account to configure history.");
        }
        else if (account->getWebOAuthToken().isEmpty())
        {
            status->setText(QString("No web token saved for %1.")
                                .arg(account->getUserName()));
        }
        else
        {
            status->setText(
                QString("Web token saved for %1.").arg(account->getUserName()));
        }
    };

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
        crossPlatformCopy("");
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
