// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/PinnedMessageWidget.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Emote.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "widgets/buttons/DrawnButton.hpp"
#include "widgets/dialogs/UserInfoPopup.hpp"
#include "widgets/splits/Split.hpp"

#include <QCursor>
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPaintEvent>
#include <QPixmap>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QShowEvent>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>

using namespace std::chrono_literals;
using namespace Qt::Literals;

#include <chrono>
#include <memory>
#include <optional>

// QTextBrowser subclass that async-loads emote images and detects URLs.
// Defined outside the chatterino namespace so it doesn't need Q_OBJECT.
class PinnedTextBrowser : public QTextBrowser
{
public:
    explicit PinnedTextBrowser(QWidget *parent = nullptr)
        : QTextBrowser(parent)
    {
        this->setOpenLinks(false);
        this->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        this->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        this->setFrameShape(QFrame::NoFrame);
        this->setFocusPolicy(Qt::NoFocus);
        this->setStyleSheet(
            "QTextBrowser { background: transparent; border: none; } "
            "QTextBrowser > QWidget > QWidget { background: transparent; }");
        this->viewport()->setAutoFillBackground(false);
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        this->document()->setDocumentMargin(2);
        this->document()->setDefaultStyleSheet(
            "a { color: #7fc0e0; text-decoration: none; }");
    }

    void setHtmlContent(const QString &html)
    {
        this->html_ = html;
        this->setHtml(html);
    }

    // Async image loading: returns cached pixmap immediately, or fires a
    // network request and re-sets the HTML when the image arrives.
    QVariant loadResource(int type, const QUrl &name) override
    {
        if (type != QTextDocument::ImageResource)
        {
            return QTextBrowser::loadResource(type, name);
        }
        const auto key = name.toString();

        auto it = this->images_.find(key);
        if (it != this->images_.end())
        {
            return it.value();
        }

        if (this->fetching_.contains(key))
        {
            return {};
        }
        this->fetching_.insert(key);

        QPointer<PinnedTextBrowser> self(this);
        chatterino::NetworkRequest(key)
            .onSuccess([self, key](const chatterino::NetworkResult &result) {
                if (!self)
                {
                    return;
                }
                QPixmap px;
                if (px.loadFromData(result.getData()) && !px.isNull())
                {
                    self->images_[key] = px;
                    self->setHtml(self->html_);
                }
            })
            .execute();

        return {};
    }

private:
    QString html_;
    QHash<QString, QPixmap> images_;
    QSet<QString> fetching_;
};

namespace chatterino {

namespace {

constexpr auto MUTED_STYLE = "color: #adadb8;";

/// Look up a third-party (BTTV/FFZ/7TV) emote by exact word match, checking
/// the channel's own emotes before falling back to each provider's global set.
EmotePtr findThirdPartyEmote(TwitchChannel *channel, const QString &word)
{
    if (!channel)
    {
        return nullptr;
    }

    const EmoteName name{word};

    if (auto emotes = channel->bttvEmotes())
    {
        auto it = emotes->find(name);
        if (it != emotes->end())
        {
            return it->second;
        }
    }
    if (auto emotes = channel->ffzEmotes())
    {
        auto it = emotes->find(name);
        if (it != emotes->end())
        {
            return it->second;
        }
    }
    if (auto emotes = channel->seventvEmotes())
    {
        auto it = emotes->find(name);
        if (it != emotes->end())
        {
            return it->second;
        }
    }

    if (auto emote = getApp()->getBttvEmotes()->emote(name))
    {
        return *emote;
    }
    if (auto emote = getApp()->getFfzEmotes()->emote(name))
    {
        return *emote;
    }
    if (auto emotes = getApp()->getSeventvEmotes()->globalEmotes())
    {
        auto it = emotes->find(name);
        if (it != emotes->end())
        {
            return it->second;
        }
    }

    return nullptr;
}

}  // namespace

PinnedMessageWidget::PinnedMessageWidget(QWidget *parent)
    : BaseWidget(parent)
    , pinnedByLabel_(new QLabel(this))
    , countdownLabel_(new QLabel(this))
    , menuButton_(new DrawnButton(DrawnButton::Symbol::Kebab, {}, this))
    , messageText_(new PinnedTextBrowser(this))
    , footerLabel_(new QLabel(this))
    , progressTimer_(new QTimer(this))
    , autoHideTimer_(new QTimer(this))
{
    this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

    auto *outerBox = new QVBoxLayout(this);
    outerBox->setContentsMargins(0, 0, 0, 0);
    outerBox->setSpacing(0);

    auto *contentBox = new QVBoxLayout();
    contentBox->setContentsMargins(8, 6, 8, 6);
    contentBox->setSpacing(3);

    // Header row: "Pinned by <user>"  [⋮]
    auto *headerRow = new QHBoxLayout();
    headerRow->setSpacing(4);

    headerRow->addWidget(this->pinnedByLabel_);
    headerRow->addStretch(1);
    this->menuButton_->setScaleIndependentSize(28, 28);
    this->menuButton_->setToolTip(u"Mod options"_s);
    this->menuButton_->setMenu(this->buildModMenu());
    this->menuButton_->hide();
    headerRow->addWidget(this->menuButton_);

    contentBox->addLayout(headerRow);

    // Message body — QTextBrowser renders rich text with clickable links and
    // async-loaded emote images.
    QObject::connect(this->messageText_, &QTextBrowser::anchorClicked,
                     [this](const QUrl &url) {
                         if (url.scheme() == u"usercard"_s)
                         {
                             this->openUserCard(url.path());
                             return;
                         }
                         QDesktopServices::openUrl(url);
                     });
    contentBox->addWidget(this->messageText_);

    // Footer: [sender · time] ... [countdown]
    auto *footerRow = new QHBoxLayout();
    footerRow->setContentsMargins(0, 2, 0, 0);
    footerRow->setSpacing(4);

    this->footerLabel_->setStyleSheet(MUTED_STYLE);
    footerRow->addWidget(this->footerLabel_);
    footerRow->addStretch(1);

    this->countdownLabel_->setStyleSheet(MUTED_STYLE);
    this->countdownLabel_->hide();
    footerRow->addWidget(this->countdownLabel_);
    contentBox->addLayout(footerRow);

    outerBox->addLayout(contentBox);

    // 1px bottom border - separates pin widget from the chat view below
    auto *bottomBorder = new QWidget(this);
    bottomBorder->setFixedHeight(1);
    bottomBorder->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    bottomBorder->setAutoFillBackground(true);
    {
        QPalette pal = bottomBorder->palette();
        pal.setColor(QPalette::Window, pal.color(QPalette::Mid));
        bottomBorder->setPalette(pal);
    }
    outerBox->addWidget(bottomBorder);

    // Countdown timer (fires every second)
    this->progressTimer_->setInterval(1s);
    QObject::connect(this->progressTimer_, &QTimer::timeout, this, [this] {
        this->tickProgress();
    });

    // auto-hide timer
    this->autoHideTimer_->setSingleShot(true);
    QObject::connect(this->autoHideTimer_, &QTimer::timeout, this, [this] {
        if (!this->userToggled_.value_or(false))
        {
            this->hide();
        }
    });

    getSettings()->pinnedMessageScale.connect(
        [this](const float &, auto) {
            this->scaleChangedEvent(this->scale());
            this->updateGeometry();
        },
        this->settingsSignalHolder_);

    this->scaleChangedEvent(this->scale());
    this->hide();
}

void PinnedMessageWidget::tickProgress()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 endsMs = this->pinEndsAt_.toMSecsSinceEpoch();

    if (nowMs >= endsMs)
    {
        this->progressTimer_->stop();
        this->countdownLabel_->hide();
        if (this->channel_)
        {
            this->channel_->clearPinnedMessage();
        }
        return;
    }

    const qint64 remainingMs = endsMs - nowMs;
    const qint64 totalSecs = (remainingMs + 999) / 1000;  // round up
    const qint64 hours = totalSecs / 3600;
    const qint64 mins = (totalSecs % 3600) / 60;
    const qint64 secs = totalSecs % 60;

    QString timeStr;
    if (hours > 0)
    {
        timeStr = u"\u23F1 %1:%2:%3"_s.arg(hours)
                      .arg(mins, 2, 10, QChar(u'0'))
                      .arg(secs, 2, 10, QChar(u'0'));
    }
    else
    {
        timeStr = u"\u23F1 %1:%2"_s.arg(mins, 2, 10, QChar(u'0'))
                      .arg(secs, 2, 10, QChar(u'0'));
    }

    this->countdownLabel_->setText(timeStr);
    this->countdownLabel_->show();
}

void PinnedMessageWidget::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    auto *theme = getTheme();

    // Fill background (same color as the split header above)
    painter.fillRect(event->rect(), theme->splits.header.background);

    // Draw 1px top border
    painter.setPen(theme->splits.header.border);
    painter.drawLine(0, 0, this->width() - 1, 0);
}

void PinnedMessageWidget::openUserCard(const QString &login)
{
    if (login.isEmpty())
    {
        return;
    }

    auto *split = qobject_cast<Split *>(this->parentWidget());
    if (!split)
    {
        return;
    }

    auto *userPopup = new UserInfoPopup(getSettings()->autoCloseUserPopup, split);
    userPopup->setData(login, split->getChannel());

    QPoint offset(userPopup->width() / 3, userPopup->height() / 5);
    userPopup->moveTo(QCursor::pos() - offset,
                      widgets::BoundsChecking::CursorPosition);
    userPopup->show();
}

void PinnedMessageWidget::setChannel(TwitchChannel *channel)
{
    this->signalHolder_.clear();
    this->channel_ = channel;
    this->autoHideTimer_->stop();

    if (channel)
    {
        this->signalHolder_.managedConnect(channel->pinnedMessageChanged,
                                           [this] {
                                               this->refresh();
                                           });
        this->signalHolder_.managedConnect(channel->userStateChanged, [this] {
            this->refresh();
        });
    }

    this->refresh();
}

std::unique_ptr<QMenu> PinnedMessageWidget::buildModMenu()
{
    auto menu = std::make_unique<QMenu>(this);

    menu->addAction(u"Unpin this Message"_s, this, [this] {
        if (this->channel_)
        {
            this->channel_->unpinCurrentMessage();
        }
    });

    auto *unpinAfterMenu = menu->addMenu(u"Unpin After"_s);

    const auto addDuration = [&](const QString &label,
                                 std::optional<std::chrono::seconds> duration) {
        unpinAfterMenu->addAction(label, this, [this, duration] {
            if (!this->channel_)
            {
                return;
            }
            const auto *pin = this->channel_->getPinnedMessage();
            if (!pin)
            {
                return;
            }
            auto currentAccount = getApp()->getAccounts()->twitch.getCurrent();
            if (!currentAccount || currentAccount->isAnon())
            {
                return;
            }
            this->channel_->updatePinnedMessageAs(
                pin->messageID, duration, *currentAccount, pin->messageText);
        });
    };

    addDuration(u"1 minute"_s, 1min);
    addDuration(u"5 minutes"_s, 5min);
    addDuration(u"10 minutes"_s, 10min);
    addDuration(u"20 minutes"_s, 20min);
    addDuration(u"30 minutes"_s, 30min);
    unpinAfterMenu->addSeparator();
    addDuration(u"End of stream"_s, std::nullopt);

    menu->addSeparator();

    menu->addAction(u"Hide for Yourself"_s, this, [this] {
        this->hide();
    });

    return menu;
}

void PinnedMessageWidget::refresh()
{
    if (!this->channel_)
    {
        this->progressTimer_->stop();
        this->autoHideTimer_->stop();
        this->hide();
        return;
    }

    const auto *pin = this->channel_->getPinnedMessage();
    if (!pin)
    {
        this->progressTimer_->stop();
        this->autoHideTimer_->stop();
        this->userToggled_ = std::nullopt;
        this->hide();
        return;
    }

    const auto mode = static_cast<UsernameDisplayMode>(
        getSettings()->usernameDisplayMode.getValue());
    this->pinnedByLabel_->setText(u"Pinned by <b>%1</b>"_s.arg(
        pin->pinnedBy.formatted(mode).toHtmlEscaped()));

    // Build rich-text HTML from message fragments so that URLs become
    // clickable links and Twitch native emotes render as inline images.
    static const QRegularExpression urlRe(
        u"(https?://[^\\s<>\"]+)"_s,
        QRegularExpression::CaseInsensitiveOption);

    // Match the rest of chat's emote size setting, on top of the line height
    // (so emotes stay in proportion to the body font set in scaleChangedEvent).
    const int emoteH = qRound(
        (this->messageText_->fontMetrics().height() +
         this->messageText_->fontMetrics().leading()) *
        getSettings()->emoteScale.getValue());

    QString html;
    html.reserve(pin->messageText.size() * 2);

    // Prefix the body with the sender, like a regular chat message, linking
    // to their usercard.
    {
        const QColor senderColor =
            this->channel_->getUserColor(pin->sender.login);
        const QString colorStyle = senderColor.isValid()
                                       ? u" style=\"color:%1;\""_s.arg(
                                             senderColor.name())
                                       : QString();
        html += u"<a href=\"usercard:%1\"%2><b>%3</b></a>: "_s.arg(
            pin->sender.login.toHtmlEscaped(), colorStyle,
            pin->sender.formatted(mode).toHtmlEscaped());
    }

    const auto appendTextWithLinks = [&](const QString &text) {
        int last = 0;
        auto it = urlRe.globalMatch(text);
        while (it.hasNext())
        {
            const auto match = it.next();
            html += text.mid(last, match.capturedStart() - last).toHtmlEscaped();
            const auto url = match.captured(1).toHtmlEscaped();
            html += u"<a href=\"%1\">%2</a>"_s.arg(url, url);
            last = match.capturedEnd();
        }
        html += text.mid(last).toHtmlEscaped();
    };

    // Splits on spaces (matching how the rest of the app tokenizes emotes)
    // so third-party emote codes can be substituted with images while URLs
    // are still linkified.
    const auto appendWordsWithEmotesAndLinks = [&](const QString &text) {
        const auto words = text.split(u' ');
        for (int i = 0; i < words.size(); ++i)
        {
            if (i > 0)
            {
                html += u' ';
            }
            const auto &word = words[i];
            if (word.isEmpty())
            {
                continue;
            }

            if (auto emote = findThirdPartyEmote(this->channel_, word))
            {
                const auto &image = emote->images.getImageOrLoaded(1.0);
                if (!image->isEmpty())
                {
                    html +=
                        u"<img src=\"%1\" height=\"%2\" alt=\"%3\" title=\"%3\">"_s
                            .arg(image->url().string, QString::number(emoteH),
                                 emote->name.string.toHtmlEscaped());
                    continue;
                }
            }

            appendTextWithLinks(word);
        }
    };

    if (pin->fragments.empty())
    {
        appendWordsWithEmotesAndLinks(pin->messageText);
    }
    else
    {
        for (const auto &frag : pin->fragments)
        {
            if (frag.type == HelixMessageFragment::Type::Emote &&
                !frag.emoteId.isEmpty())
            {
                const auto src =
                    u"https://static-cdn.jtvnw.net/emoticons/v2/%1/default/dark/1.0"_s
                        .arg(frag.emoteId);
                html +=
                    u"<img src=\"%1\" height=\"%2\" alt=\"%3\" title=\"%3\">"_s
                        .arg(src, QString::number(emoteH),
                             frag.text.toHtmlEscaped());
            }
            else
            {
                appendWordsWithEmotesAndLinks(frag.text);
            }
        }
    }

    static_cast<PinnedTextBrowser *>(this->messageText_)->setHtmlContent(html);
    this->updateMessageHeight();

    {
        const QString sentAt = pin->startsAt.toLocalTime().toString(
            getSettings()->timestampFormat);
        this->footerLabel_->setText(u"Sent by %1 \u00B7 %2"_s.arg(
            pin->sender.formatted(mode).toHtmlEscaped(), sentAt));
    }

    this->progressTimer_->stop();
    this->countdownLabel_->hide();
    if (pin->endsAt.has_value() && pin->endsAt->isValid())
    {
        this->pinEndsAt_ = *pin->endsAt;
        this->tickProgress();  // set initial text immediately
        this->progressTimer_->start();
    }

    const bool isMod = this->channel_->hasModRights();
    this->menuButton_->setVisible(isMod);

    if (this->userToggled_.value_or(true))
    {
        this->show();
        this->updateMessageHeightIfNeeded();
    }

    this->autoHideTimer_->stop();
    if (!getSettings()->alwaysShowPinnedMessage &&
        !this->userToggled_.value_or(false))
    {
        this->autoHideTimer_->start(30s);
    }
}

bool PinnedMessageWidget::hasMessage() const
{
    return this->channel_ && this->channel_->getPinnedMessage() != nullptr;
}

void PinnedMessageWidget::toggleUserPinned()
{
    if (this->isVisible())
    {
        this->userToggled_ = false;
        this->autoHideTimer_->stop();
        this->hide();
    }
    else
    {
        this->userToggled_ = true;
        this->autoHideTimer_->stop();
        this->show();
        this->updateMessageHeightIfNeeded();
    }
}

void PinnedMessageWidget::updateMessageHeight()
{
    if (!this->messageText_)
    {
        return;
    }

    // Reflow the document at the current viewport width, then measure its height.
    const int vw = this->messageText_->viewport()->width();
    if (vw <= 0)
    {
        return;
    }
    this->lastViewportWidth_ = vw;
    auto *doc = this->messageText_->document();
    doc->setTextWidth(vw);
    const int contentH = qMax(1, qRound(doc->size().height()));

    // Size to content, but never taller than the cap.
    this->messageText_->setFixedHeight(qBound(1, contentH, this->messageMaxHeight_));
}

void PinnedMessageWidget::updateMessageHeightIfNeeded()
{
    if (this->lastViewportWidth_ != this->messageText_->viewport()->width())
    {
        this->updateMessageHeight();
    }
}

void PinnedMessageWidget::resizeEvent(QResizeEvent *event)
{
    BaseWidget::resizeEvent(event);
    this->updateMessageHeight();
}

void PinnedMessageWidget::showEvent(QShowEvent *event)
{
    BaseWidget::showEvent(event);
    this->visibilityChanged.invoke();
}

void PinnedMessageWidget::hideEvent(QHideEvent *event)
{
    BaseWidget::hideEvent(event);
    this->visibilityChanged.invoke();
}

void PinnedMessageWidget::scaleChangedEvent(float newScale)
{
    // Track the same font the rest of chat uses, so the pinned banner
    // doesn't drift out of sync when the user changes their chat font size.
    const float chatFontPt = float(getSettings()->chatFontSize.getValue());
    const float s = newScale * std::clamp(float(getSettings()->pinnedMessageScale),
                                          0.5F, 2.0F);

    QFont headerFont = this->pinnedByLabel_->font();
    headerFont.setFamily(getSettings()->chatFontFamily.getValue());
    headerFont.setPointSizeF(chatFontPt * 0.86F * s);
    this->pinnedByLabel_->setFont(headerFont);
    this->countdownLabel_->setFont(headerFont);

    QFont bodyFont = this->messageText_->font();
    bodyFont.setFamily(getSettings()->chatFontFamily.getValue());
    bodyFont.setWeight(
        static_cast<QFont::Weight>(getSettings()->chatFontWeight.getValue()));
    bodyFont.setPointSizeF(chatFontPt * s);
    this->messageText_->setFont(bodyFont);
    this->messageText_->document()->setDefaultFont(bodyFont);
    this->messageMaxHeight_ = int(110 * s);
    this->updateMessageHeight();

    QFont footerFont = this->footerLabel_->font();
    footerFont.setFamily(getSettings()->chatFontFamily.getValue());
    footerFont.setPointSizeF(chatFontPt * 0.82F * s);
    this->footerLabel_->setFont(footerFont);
}

void PinnedMessageWidget::mousePressEvent(QMouseEvent *event)
{
    // ignore to disable the parent's right click menu
}

}  // namespace chatterino
