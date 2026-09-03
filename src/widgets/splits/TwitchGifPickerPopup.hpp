// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/twitch/api/TwitchGifs.hpp"
#include "widgets/BasePopup.hpp"
#include "widgets/listview/GenericListModel.hpp"

#include <QTimer>

#include <functional>
#include <optional>

class QLineEdit;

namespace chatterino {

class GenericListView;

class TwitchGifPickerPopup : public BasePopup
{
    using ActionCallback = std::function<void(twitchgifs::SearchResult)>;
    using AvailabilityCallback = std::function<void(bool)>;

public:
    explicit TwitchGifPickerPopup(QWidget *parent = nullptr);

    void updateSearch(const QString &query, const QString &channelID,
                      const QString &webOAuthToken);
    void openPicker(const QString &channelID, const QString &webOAuthToken);
    void prepare(const QString &channelID, const QString &webOAuthToken,
                 AvailabilityCallback callback, bool forceRefresh = false);
    void resizeToFit(int availableWidth);
    void showMessage(const QString &text);
    void setInputAction(ActionCallback callback);

    bool eventFilter(QObject *watched, QEvent *event) override;

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void themeChangedEvent() override;

private:
    void resizeForContent(int contentHeight);
    void loadConfig();
    void startSearch();
    void showStatus(const QString &text);
    void showResults(std::vector<twitchgifs::SearchResult> results);
    bool isAvailable() const;
    void notifyAvailability();

    GenericListView *listView_{};
    QLineEdit *searchInput_{};
    GenericListModel model_{this};
    QTimer searchTimer_;
    QTimer redrawTimer_;

    ActionCallback callback_;
    AvailabilityCallback availabilityCallback_;
    QString query_;
    QString channelID_;
    QString webOAuthToken_;
    std::optional<twitchgifs::PickerConfig> config_;
    quint64 requestVersion_{};
    int availableWidth_{440};
    int contentHeight_{40};
    bool commandMode_{true};
};

}  // namespace chatterino
