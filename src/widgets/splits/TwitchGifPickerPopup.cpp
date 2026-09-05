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
#include <QMenu>
#include <QPainter>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTabBar>
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
        if (twitchgifs::isFavourite(this->result_.id))
        {
            painter->drawText(rect.adjusted(0, 4, -8, 0),
                              Qt::AlignRight | Qt::AlignTop,
                              QStringLiteral("★"));
        }
    }

    const twitchgifs::SearchResult &result() const
    {
        return this->result_;
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
    auto tabs = layout.emplace<QTabBar>().assign(&this->tabs_);
    tabs->addTab(QStringLiteral("Search"));
    tabs->addTab(QStringLiteral("Favourites"));
    tabs->addTab(QStringLiteral("Recent"));
    tabs->hide();
    auto search = layout.emplace<QLineEdit>().assign(&this->searchInput_);
    search->setPlaceholderText(QStringLiteral("Search GIFs"));
    search->hide();
    this->searchInput_->installEventFilter(this);
    layout.emplace<GenericListView>().assign(&this->listView_);
    this->listView_->setModel(&this->model_);
    this->listView_->setInvokeActionOnTab(false);
    this->listView_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->listView_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    this->listView_->setContextMenuPolicy(Qt::CustomContextMenu);
    this->resizeToFit(440);

    QObject::connect(
        this->searchInput_, &QLineEdit::textChanged, this,
        [this](const QString &query) {
            this->query_ = query;
            if (this->config_ &&
                this->tabs_->currentIndex() == static_cast<int>(Page::Search))
            {
                ++this->requestVersion_;
                this->searchTimer_.start();
            }
        });

    QObject::connect(this->listView_, &GenericListView::closeRequested, this,
                     &QWidget::hide);
    QObject::connect(this->listView_, &QWidget::customContextMenuRequested,
                     this, &TwitchGifPickerPopup::showGifMenu);
    QObject::connect(this->tabs_, &QTabBar::currentChanged, this,
                     [this](int index) {
                         this->showPage(static_cast<Page>(index));
                     });
    QObject::connect(this->listView_->verticalScrollBar(),
                     &QScrollBar::actionTriggered, this, [this] {
                         QTimer::singleShot(0, this, [this] {
                             auto *bar = this->listView_->verticalScrollBar();
                             if (this->hasMore_ && !this->loadingMore_ &&
                                 bar->maximum() > 0 &&
                                 bar->value() <= ITEM_HEIGHT)
                             {
                                 this->startSearch(true);
                             }
                         });
                     });

    this->searchTimer_.setSingleShot(true);
    this->searchTimer_.setInterval(250);
    QObject::connect(&this->searchTimer_, &QTimer::timeout, this, [this] {
        this->startSearch();
    });

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
    this->setWindowFlag(Qt::WindowDoesNotAcceptFocus, false);
    this->tabs_->show();
    this->searchInput_->show();
    {
        const QSignalBlocker tabsBlocker{this->tabs_};
        const QSignalBlocker searchBlocker{this->searchInput_};
        this->tabs_->setCurrentIndex(static_cast<int>(Page::Search));
        this->searchInput_->setText(query);
    }
    this->resizeToFit(this->width());
    this->query_ = query;
    this->setContext(channelID, webOAuthToken);
    if (this->configState_ == ConfigState::Loading)
    {
        return;
    }
    if (this->configState_ != ConfigState::Loaded)
    {
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
    this->tabs_->show();
    this->searchInput_->clear();
    this->query_.clear();
    this->setContext(channelID, webOAuthToken);

    if (this->configState_ == ConfigState::Loading)
    {
        return;
    }
    if (this->configState_ != ConfigState::Loaded)
    {
        ++this->requestVersion_;
        this->loadConfig();
    }
    else
    {
        ++this->requestVersion_;
        this->showPage(static_cast<Page>(this->tabs_->currentIndex()));
    }
}

void TwitchGifPickerPopup::setContext(const QString &channelID,
                                      const QString &webOAuthToken)
{
    if (this->channelID_ == channelID && this->webOAuthToken_ == webOAuthToken)
    {
        return;
    }
    this->channelID_ = channelID;
    this->webOAuthToken_ = webOAuthToken;
    this->config_.reset();
    this->configState_ = ConfigState::Unloaded;
    ++this->requestVersion_;
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
    const auto height = (MAX_VISIBLE_RESULTS * ITEM_HEIGHT) +
                        this->tabs_->sizeHint().height() +
                        this->searchInput_->sizeHint().height();
    const auto bottom = this->y() + this->height();
    this->setFixedSize(width, height);
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
    if (this->configState_ == ConfigState::Loading)
    {
        return;
    }
    this->configState_ = ConfigState::Loading;
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
            this->configState_ = ConfigState::Loaded;
            if (!this->isAvailable())
            {
                this->showStatus(
                    QStringLiteral("GIF messages are not available here."));
                return;
            }
            this->showPage(static_cast<Page>(this->tabs_->currentIndex()));
        },
        [this, version](QString error) {
            if (version == this->requestVersion_)
            {
                this->config_.reset();
                this->configState_ = ConfigState::Failed;
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

void TwitchGifPickerPopup::startSearch(bool loadMore)
{
    if (!this->isAvailable())
    {
        return;
    }

    if ((loadMore && this->loadingMore_) ||
        (!loadMore &&
         this->tabs_->currentIndex() != static_cast<int>(Page::Search)))
    {
        return;
    }
    if (!loadMore)
    {
        this->nextOffset_ = 0;
        this->hasMore_ = false;
        this->showStatus(QStringLiteral("Searching GIFs..."));
    }
    this->loadingMore_ = true;
    const auto version = this->requestVersion_;
    twitchgifs::search(
        this->query_, *this->config_, this->nextOffset_, this,
        [this, version, loadMore](twitchgifs::SearchPage page) {
            if (version == this->requestVersion_)
            {
                this->nextOffset_ = page.nextOffset;
                this->hasMore_ = page.hasMore;
                if (loadMore)
                {
                    this->appendResults(std::move(page.results));
                }
                else
                {
                    this->showResults(std::move(page.results));
                }
                this->loadingMore_ = false;
            }
        },
        [this, version, loadMore](QString error) {
            if (version == this->requestVersion_)
            {
                this->loadingMore_ = false;
                this->hasMore_ = false;
                if (!loadMore)
                {
                    this->showStatus(QStringLiteral("Unable to search GIFs: ") +
                                     error);
                }
            }
        });
}

void TwitchGifPickerPopup::showPage(Page page)
{
    ++this->requestVersion_;
    this->searchTimer_.stop();
    this->loadingMore_ = false;
    this->hasMore_ = false;
    this->searchInput_->setVisible(page == Page::Search);
    if (page == Page::Search)
    {
        this->startSearch();
        return;
    }
    auto results = page == Page::Favourites ? twitchgifs::favouriteGifs()
                                            : twitchgifs::recentlySentGifs();
    if (results.empty())
    {
        this->showStatus(page == Page::Favourites
                             ? QStringLiteral("No favourite GIFs yet.")
                             : QStringLiteral("No recently sent GIFs yet."));
        return;
    }
    this->showResults(std::move(results));
}

bool TwitchGifPickerPopup::isAvailable() const
{
    return this->config_ && this->config_->isEnabled &&
           this->config_->isAllowlisted && this->config_->canSend &&
           !this->config_->apiKey.isEmpty();
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

    const auto count = results.size();
    for (auto i = count; i > 0; --i)
    {
        this->model_.addItem(std::make_unique<GifPickerItem>(
            std::move(results[i - 1]), this->callback_));
    }
    this->resizeForContent(int(std::min<size_t>(count, MAX_VISIBLE_RESULTS)) *
                           ITEM_HEIGHT);
    this->listView_->setCurrentIndex(this->model_.index(int(count - 1)));
    const auto version = this->requestVersion_;
    QTimer::singleShot(0, this, [this, version] {
        if (version == this->requestVersion_)
        {
            this->listView_->scrollToBottom();
        }
    });
}

void TwitchGifPickerPopup::appendResults(
    std::vector<twitchgifs::SearchResult> results)
{
    if (results.empty())
    {
        return;
    }
    auto *bar = this->listView_->verticalScrollBar();
    const auto oldMaximum = bar->maximum();
    const auto oldValue = bar->value();
    std::vector<std::unique_ptr<GenericListItem>> items;
    items.reserve(results.size());
    for (auto i = results.size(); i > 0; --i)
    {
        items.emplace_back(std::make_unique<GifPickerItem>(
            std::move(results[i - 1]), this->callback_));
    }
    this->model_.prependItems(std::move(items));
    bar->setValue(oldValue + (bar->maximum() - oldMaximum));
}

void TwitchGifPickerPopup::showGifMenu(const QPoint &position)
{
    const auto index = this->listView_->indexAt(position);
    if (!index.isValid())
    {
        return;
    }
    auto *item = dynamic_cast<GifPickerItem *>(
        GenericListItem::fromVariant(index.data()));
    if (item == nullptr)
    {
        return;
    }
    const auto gif = item->result();
    QMenu menu(this);
    auto *action = menu.addAction(QStringLiteral("Favourite GIF"));
    action->setCheckable(true);
    action->setChecked(twitchgifs::isFavourite(gif.id));
    if (menu.exec(this->listView_->viewport()->mapToGlobal(position)) == action)
    {
        twitchgifs::setFavourite(gif, action->isChecked());
        if (!this->commandMode_ &&
            this->tabs_->currentIndex() == static_cast<int>(Page::Favourites))
        {
            this->showPage(Page::Favourites);
        }
        else
        {
            this->listView_->viewport()->update();
        }
    }
}

}  // namespace chatterino
