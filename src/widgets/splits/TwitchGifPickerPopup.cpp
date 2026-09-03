// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/TwitchGifPickerPopup.hpp"

#include "messages/Image.hpp"
#include "providers/twitch/api/TwitchIntegrity.hpp"
#include "singletons/Theme.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/listview/GenericListItem.hpp"
#include "widgets/listview/GenericListView.hpp"

#include <QLineEdit>
#include <QPainter>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <utility>

namespace chatterino {

namespace {

constexpr int ITEM_HEIGHT = 76;
constexpr int MAX_VISIBLE_RESULTS = 4;

class GifPickerItem : public GenericListItem
{
public:
    GifPickerItem(twitchgifs::SearchResult result,
                  std::function<void(twitchgifs::SearchResult)> action)
        : result_(std::move(result))
        , image_(Image::fromUrlAnimated(this->result_.previewUrl, 1,
                                        this->result_.previewSize))
        , action_(std::move(action))
    {
    }

    void action() override
    {
        auto action = this->action_;
        if (action)
        {
            action(this->result_);
        }
    }

    void paint(QPainter *painter, const QRect &rect) const override
    {
        constexpr int margin = 4;
        const auto imageHeight = rect.height() - (margin * 2);
        auto imageWidth = imageHeight;
        if (this->result_.previewSize.height() > 0)
        {
            imageWidth = std::min(
                rect.width() / 2,
                int(double(imageHeight) * this->result_.previewSize.width() /
                    this->result_.previewSize.height()));
        }

        const QRect imageRect{rect.topLeft() + QPoint{margin, margin},
                              QSize{imageWidth, imageHeight}};
        if (const auto pixmap = this->image_->pixmapOrLoad())
        {
            painter->setRenderHint(QPainter::SmoothPixmapTransform);
            painter->drawPixmap(imageRect, *pixmap);
        }

        const QRect textRect{
            imageRect.topRight() + QPoint{8, 0},
            QSize{rect.width() - imageRect.width() - 16, imageRect.height()}};
        painter->drawText(textRect,
                          Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap,
                          this->result_.title);
    }

    QSize sizeHint(const QRect &rect) const override
    {
        return {rect.width(), ITEM_HEIGHT};
    }

private:
    twitchgifs::SearchResult result_;
    ImagePtr image_;
    std::function<void(twitchgifs::SearchResult)> action_;
};

class StatusItem : public GenericListItem
{
public:
    explicit StatusItem(QString text)
        : text_(std::move(text))
    {
    }

    void action() override
    {
    }

    void paint(QPainter *painter, const QRect &rect) const override
    {
        painter->drawText(rect.adjusted(8, 0, -8, 0),
                          Qt::AlignLeft | Qt::AlignVCenter, this->text_);
    }

    QSize sizeHint(const QRect &rect) const override
    {
        return {rect.width(), 40};
    }

private:
    QString text_;
};

}  // namespace

TwitchGifPickerPopup::TwitchGifPickerPopup(QWidget *parent)
    : BasePopup({BasePopup::EnableCustomFrame, BasePopup::Frameless,
                 BaseWindow::DisableLayoutSave},
                parent)
{
    this->windowDeactivateAction = WindowDeactivateAction::Hide;

    LayoutCreator creator{this};
    auto layout = creator.setLayoutType<QVBoxLayout>().withoutMargin();
    auto search = layout.emplace<QLineEdit>().assign(&this->searchInput_);
    search->setPlaceholderText(QStringLiteral("Search GIFs"));
    search->hide();
    layout.emplace<GenericListView>().assign(&this->listView_);
    this->listView_->setModel(&this->model_);
    this->listView_->setInvokeActionOnTab(false);
    this->resizeToFit(440);

    QObject::connect(this->searchInput_, &QLineEdit::textChanged, this,
                     [this](const QString &query) {
                         if (!this->commandMode_)
                         {
                             this->query_ = query;
                             if (this->config_)
                             {
                                 ++this->requestVersion_;
                                 this->searchTimer_.start();
                             }
                         }
                     });

    QObject::connect(this->listView_, &GenericListView::closeRequested, this,
                     &QWidget::hide);

    this->searchTimer_.setSingleShot(true);
    this->searchTimer_.setInterval(250);
    QObject::connect(&this->searchTimer_, &QTimer::timeout, this,
                     &TwitchGifPickerPopup::startSearch);

    this->redrawTimer_.setInterval(33);
    QObject::connect(&this->redrawTimer_, &QTimer::timeout, this, [this] {
        if (this->isVisible())
        {
            this->listView_->viewport()->update();
        }
    });

    this->themeChangedEvent();
}

void TwitchGifPickerPopup::updateSearch(const QString &query,
                                        const QString &channelID,
                                        const QString &webOAuthToken)
{
    this->commandMode_ = true;
    this->setAttribute(Qt::WA_ShowWithoutActivating, true);
    this->setWindowFlag(Qt::WindowDoesNotAcceptFocus, true);
    this->searchInput_->hide();
    this->resizeToFit(this->width());
    this->query_ = query;
    if (this->channelID_ != channelID || this->webOAuthToken_ != webOAuthToken)
    {
        this->channelID_ = channelID;
        this->webOAuthToken_ = webOAuthToken;
        this->config_.reset();
        ++this->requestVersion_;
        this->loadConfig();
        return;
    }

    if (this->config_)
    {
        ++this->requestVersion_;
        this->searchTimer_.start();
    }
}

void TwitchGifPickerPopup::openPicker(const QString &channelID,
                                      const QString &webOAuthToken)
{
    this->commandMode_ = false;
    this->setWindowFlag(Qt::WindowDoesNotAcceptFocus, false);
    this->setAttribute(Qt::WA_ShowWithoutActivating, false);
    this->searchInput_->show();
    this->searchInput_->clear();
    this->query_.clear();

    if (this->channelID_ != channelID ||
        this->webOAuthToken_ != webOAuthToken || !this->config_)
    {
        this->channelID_ = channelID;
        this->webOAuthToken_ = webOAuthToken;
        this->config_.reset();
        ++this->requestVersion_;
        this->loadConfig();
    }
    else
    {
        ++this->requestVersion_;
        this->startSearch();
    }
}

void TwitchGifPickerPopup::prepare(const QString &channelID,
                                   const QString &webOAuthToken,
                                   AvailabilityCallback callback,
                                   bool forceRefresh)
{
    this->availabilityCallback_ = std::move(callback);
    if (!forceRefresh && this->channelID_ == channelID &&
        this->webOAuthToken_ == webOAuthToken && this->config_)
    {
        this->notifyAvailability();
        return;
    }

    this->channelID_ = channelID;
    this->webOAuthToken_ = webOAuthToken;
    this->config_.reset();
    ++this->requestVersion_;
    this->loadConfig();
}

void TwitchGifPickerPopup::resizeToFit(int availableWidth)
{
    this->availableWidth_ = availableWidth;
    this->resizeForContent(this->contentHeight_);
}

void TwitchGifPickerPopup::resizeForContent(int contentHeight)
{
    this->contentHeight_ = contentHeight;
    const auto width = std::max(1, std::min(440, this->availableWidth_));
    const auto searchHeight = this->commandMode_ ? 0 : 32;
    const auto bottom = this->y() + this->height();
    this->setFixedSize(width, this->contentHeight_ + searchHeight);
    if (this->isVisible())
    {
        this->move(this->x(), bottom - this->height());
    }
}

void TwitchGifPickerPopup::setInputAction(ActionCallback callback)
{
    this->callback_ = std::move(callback);
}

void TwitchGifPickerPopup::showMessage(const QString &text)
{
    ++this->requestVersion_;
    this->searchTimer_.stop();
    this->showStatus(text);
}

bool TwitchGifPickerPopup::eventFilter(QObject *watched, QEvent *event)
{
    return this->listView_->eventFilter(watched, event);
}

void TwitchGifPickerPopup::showEvent(QShowEvent *event)
{
    this->redrawTimer_.start();
    BasePopup::showEvent(event);
    if (!this->commandMode_)
    {
        this->searchInput_->setFocus(Qt::PopupFocusReason);
    }
}

void TwitchGifPickerPopup::hideEvent(QHideEvent *event)
{
    this->searchTimer_.stop();
    this->redrawTimer_.stop();
    BasePopup::hideEvent(event);
}

void TwitchGifPickerPopup::themeChangedEvent()
{
    BasePopup::themeChangedEvent();
    if (this->listView_)
    {
        this->listView_->refreshTheme(*getTheme());
    }
}

void TwitchGifPickerPopup::loadConfig()
{
    this->searchTimer_.stop();
    this->showStatus(QStringLiteral("Checking GIF availability..."));
    const auto version = this->requestVersion_;
    twitchgifs::loadPickerConfig(
        this->channelID_, this->webOAuthToken_, this,
        [this, version](twitchgifs::PickerConfig config) {
            if (version != this->requestVersion_)
            {
                return;
            }
            this->config_ = std::move(config);
            this->notifyAvailability();
            if (!this->isAvailable())
            {
                this->showStatus(
                    QStringLiteral("GIF messages are not available here."));
                return;
            }
            this->startSearch();
        },
        [this, version](QString error) {
            if (version == this->requestVersion_)
            {
                this->config_.reset();
                this->notifyAvailability();
                if (error.contains("token", Qt::CaseInsensitive) ||
                    error.contains("unauthorized", Qt::CaseInsensitive))
                {
                    error +=
                        QStringLiteral(". Replace it in Settings > Accounts");
                }
                this->showStatus(QStringLiteral("Unable to load GIF picker: ") +
                                 error);
            }
        });
}

void TwitchGifPickerPopup::startSearch()
{
    if (!this->isAvailable())
    {
        return;
    }

    const auto version = this->requestVersion_;
    this->showStatus(QStringLiteral("Searching GIFs..."));
    twitchgifs::search(
        this->query_, *this->config_, this,
        [this, version](std::vector<twitchgifs::SearchResult> results) {
            if (version == this->requestVersion_)
            {
                this->showResults(std::move(results));
            }
        },
        [this, version](QString error) {
            if (version == this->requestVersion_)
            {
                this->showStatus(QStringLiteral("Unable to search GIFs: ") +
                                 error);
            }
        });
}

bool TwitchGifPickerPopup::isAvailable() const
{
    return this->config_ && this->config_->isEnabled &&
           this->config_->isAllowlisted && this->config_->canSend &&
           !this->config_->apiKey.isEmpty();
}

void TwitchGifPickerPopup::notifyAvailability()
{
    if (this->availabilityCallback_)
    {
        this->availabilityCallback_(this->isAvailable());
    }
    if (this->isAvailable())
    {
        twitchgifs::warmGifIntegritySession();
    }
}

void TwitchGifPickerPopup::showStatus(const QString &text)
{
    this->model_.clear();
    this->model_.addItem(std::make_unique<StatusItem>(text));
    this->listView_->setCurrentIndex(this->model_.index(0));
    this->resizeForContent(40);
}

void TwitchGifPickerPopup::showResults(
    std::vector<twitchgifs::SearchResult> results)
{
    this->model_.clear();
    if (results.empty())
    {
        this->showStatus(QStringLiteral("No GIFs found."));
        return;
    }

    const auto count = std::min<size_t>(results.size(), MAX_VISIBLE_RESULTS);
    for (auto i = count; i > 0; --i)
    {
        this->model_.addItem(std::make_unique<GifPickerItem>(
            std::move(results[i - 1]), this->callback_));
    }
    this->listView_->setCurrentIndex(this->model_.index(int(count - 1)));
    this->listView_->scrollToBottom();
    this->resizeForContent(int(count) * ITEM_HEIGHT);
}

}  // namespace chatterino
