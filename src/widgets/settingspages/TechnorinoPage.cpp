#include "widgets/settingspages/TechnorinoPage.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/CrashHandler.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/NativeMessaging.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "util/Helpers.hpp"
#include "widgets/BaseWindow.hpp"
#include "widgets/settingspages/GeneralPageView.hpp"
#include "widgets/settingspages/SettingWidget.hpp"

#include <magic_enum/magic_enum.hpp>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFontDialog>
#include <QLabel>
#include <QScrollArea>

namespace {

using namespace chatterino;
using namespace literals;

#ifdef Q_OS_WIN
const QString META_KEY = u"Windows"_s;
#else
const QString META_KEY = u"Meta"_s;
#endif

void addKeyboardModifierSetting(GeneralPageView &layout, const QString &title,
                                EnumSetting<Qt::KeyboardModifier> &setting)
{
    layout.addDropdown<std::underlying_type<Qt::KeyboardModifier>::type>(
        title, {"None", "Shift", "Control", "Alt", META_KEY}, setting,
        [](int index) {
            switch (index)
            {
                case Qt::ShiftModifier:
                    return 1;
                case Qt::ControlModifier:
                    return 2;
                case Qt::AltModifier:
                    return 3;
                case Qt::MetaModifier:
                    return 4;
                default:
                    return 0;
            }
        },
        [](DropdownArgs args) {
            switch (args.index)
            {
                case 1:
                    return Qt::ShiftModifier;
                case 2:
                    return Qt::ControlModifier;
                case 3:
                    return Qt::AltModifier;
                case 4:
                    return Qt::MetaModifier;
                default:
                    return Qt::NoModifier;
            }
        },
        false);
}
}  // namespace

namespace chatterino {

TechnorinoPage::TechnorinoPage()
{
    auto *y = new QVBoxLayout;
    auto *x = new QHBoxLayout;
    auto *view = GeneralPageView::withNavigation(this);
    this->view_ = view;
    x->addWidget(view);
    auto *z = new QFrame;
    z->setLayout(x);
    y->addWidget(z);
    this->setLayout(y);

    this->initLayout(*view);

    this->initExtra();
}

bool TechnorinoPage::filterElements(const QString &query)
{
    if (this->view_)
    {
        return this->view_->filterElements(query) || query.isEmpty();
    }
    else
    {
        return false;
    }
}

void TechnorinoPage::initLayout(GeneralPageView &layout)
{
    auto &s = *getSettings();

    layout.addTitle("Chat");
    // SettingWidget::checkbox("", s.hideModerated)->setTooltip("")->addTo(layout);
    SettingWidget::checkbox(
        "Show placeholder in text input box (requires restart)",
        s.showTextInputPlaceholder)
        ->addTo(layout);
    SettingWidget::checkbox("Convert #text to channel links", s.channelLinks)
        ->addTo(layout);

    layout.addTitle("Client detection");
    SettingWidget::checkbox("Client detection highlights. ",
                            s.normalNonceDetection)
        ->setTooltip("Highlights messages sent from specified clients "
                     "using the specified color below.")
        ->addTo(layout);
    SettingWidget::colorButton("Webchat color", s.webchatColor)->addTo(layout);
    SettingWidget::colorButton("Android color", s.androidColor)->addTo(layout);
    SettingWidget::colorButton("iOS color", s.iosColor)->addTo(layout);
    SettingWidget::checkbox("Client detection icons. ", s.clientDetectionIcon)
        ->setTooltip("Displays client icons beside messages")
        ->addTo(layout);

    layout.addTitle("Miscellaneous");
    SettingWidget::checkbox("Fake messages as webchat", s.fakeWebChat)
        ->addTo(layout);
    SettingWidget::checkbox("Use bot limits for messages",
                            s.useBotLimitsMessage)
        ->addTo(layout);
    SettingWidget::checkbox("Use bot limits for JOINs", s.useBotLimitsJoin)
        ->addTo(layout);
    SettingWidget::checkbox(
        "Enable. Required for abnormal nonce and webchat detection to work!",
        s.nonceFuckeryEnabled)
        ->addTo(layout);
    SettingWidget::checkbox("Abnormal nonce detection",
                            s.abnormalNonceDetection)
        ->addTo(layout);
    SettingWidget::checkbox("\"7TV User\" usercard button", s.stvUsercardButton)
        ->setTooltip("Add \"7TV User\" button to usercard directly")
        ->addTo(layout);
    SettingWidget::checkbox("Watching tab live sound", s.watchingTabLiveSound)
        ->addTo(layout);
    SettingWidget::checkbox("Auto detach watching tab (~10s timeout)",
                            s.autoDetachLiveTab)
        ->addTo(layout);
    SettingWidget::checkbox("Markdown parsing (Experimental)",
                            s.markdownParsing)
        ->addTo(layout);
    SettingWidget::checkbox(
        "Anon read connection (requires restart) (Experimental)", s.anonRead)
        ->setTooltip("Use an anon connection for the read connection. NOTE: "
                     "This will NOT guarantee exclusion from viewerlists.")
        ->addTo(layout);

    layout.addTitle("Pinned Messages");
    SettingWidget::checkbox("Show pinned message banner",
                            s.enablePinnedMessages)
        ->setTooltip("Show a banner above chat when a message is pinned.")
        ->addTo(layout);
    SettingWidget::checkbox("Always expand long pinned messages",
                            s.alwaysExpandPinnedMessages)
        ->setTooltip("Automatically expand the full content of long pins.")
        ->addTo(layout);
    SettingWidget::checkbox("Show unpin notifications in chat",
                            s.showUnpinNotifications)
        ->setTooltip("Show a system message when someone unpins a message.")
        ->addTo(layout);
    layout.addDropdown<int>(
        "Close button action",
        {"Hide banner here", "Unpin for everyone"},
        s.pinCloseButtonAction,
        [](int val) {
            return val == 1 ? QString("Unpin for everyone")
                            : QString("Hide banner here");
        },
        [](DropdownArgs args) {
            return args.value.startsWith("Unpin") ? 1 : 0;
        },
        false)
        ->setToolTip("What the X button on the pin banner does.");
    layout.addDropdown<int>(
        "Timer display",
        {"Time + Countdown", "Time only", "Countdown only", "Hover only",
         "Hidden"},
        s.pinTimerDisplay,
        [](int val) {
            switch (val)
            {
                case 1: return QString("Time only");
                case 2: return QString("Countdown only");
                case 3: return QString("Hover only");
                case 4: return QString("Hidden");
                default: return QString("Time + Countdown");
            }
        },
        [](DropdownArgs args) {
            if (args.value == "Time only") return 1;
            if (args.value == "Countdown only") return 2;
            if (args.value == "Hover only") return 3;
            if (args.value == "Hidden") return 4;
            return 0;
        },
        false)
        ->setToolTip("How pin time is shown on the banner.");

    layout.addTitle("Miscellaneous");
    SettingWidget::checkbox("Use message colors for tab alerts",
                            s.colorTabHighlightsByMessage)
        ->setTooltip("When a message highlights a tab, use the message's "
                     "highlight color for the tab indicator line.")
        ->addTo(layout);
    SettingWidget::checkbox("Hide mod actions on moderator usercards",
                            s.hideModActionsOnModUsercards)
        ->setTooltip(
            "Do not show timeout/ban buttons when clicking a moderator's "
            "name in chat.")
        ->addTo(layout);
    SettingWidget::checkbox("Show mod actions on mod usercards as lead mod",
                            s.showModActionsOnModUsercardsAsLeadMod)
        ->setTooltip("When you are lead moderator, still show mod action "
                     "buttons on other moderators' usercards.")
        ->addTo(layout);

    layout.addTitle("Nuke");
    SettingWidget::checkbox("Enable nuke preview",
                            s.nukePreviewEnabled)
        ->setTooltip("While typing /nuke, highlight matching messages in chat "
                     "as a preview.")
        ->addTo(layout);
    SettingWidget::checkbox("Show nuke summary", s.nukeShowSummary)
        ->setTooltip("Show a summary message in chat when a nuke finishes.")
        ->addTo(layout);
    SettingWidget::checkbox("Skip VIPs in nuke", s.nukeSkipVips)
        ->setTooltip("Do not target VIP users when running /nuke.")
        ->addTo(layout);
    SettingWidget::lineEdit("Nuke moderation reason",
                            s.nukeModerationMessage,
                            "Optional reason for timeout/ban")
        ->setTooltip("Reason sent with timeout or ban actions from /nuke.")
        ->addTo(layout);

    layout.addTitle("Translation");
    SettingWidget::lineEdit("Default translation target language",
                            s.messageTranslationTargetLanguage, "e.g. en, fr, ja")
        ->setTooltip(
            "Language code used by /translate. Use /translateto <lang> "
            "<text> to override per message.")
        ->addTo(layout);

    layout.addTitle("Moderation");
    SettingWidget::checkbox("Show repeated-message counters",
                            s.enableRepeatedMessageDetector)
        ->setTooltip("Show repeated or very similar messages with an inline "
                     "counter such as x2, x3.")
        ->addTo(layout);
    SettingWidget::checkbox("Show only in moderation mode",
                            s.repeatedMessagesShowOnlyModerationMode)
        ->setTooltip("Only show repetition counters when inline mod buttons "
                     "are visible.")
        ->addTo(layout);
    SettingWidget::checkbox("Show counters in usercards",
                            s.repeatedMessagesShowInUsercards)
        ->setTooltip(
            "Show already-detected repeat counters in usercards when clicking "
            "a user.")
        ->addTo(layout);
    SettingWidget::checkbox("Only in channels where I can moderate",
                            s.repeatedMessagesOnlyModChannels)
        ->setTooltip("Only show repeat counters in channels where you can "
                     "moderate.")
        ->addTo(layout);
    SettingWidget::checkbox("Ignore VIPs", s.repeatedMessagesIgnoreVips)
        ->setTooltip("Do not mark repeated messages from VIPs.")
        ->addTo(layout);

    layout.addDropdown<int>(
        "Similarity sensitivity",
        {"Loose", "Soft", "Default", "Strict", "Exact only"},
        s.repeatedMessagesSensitivity,
        [](auto val) {
            switch (val)
            {
                case 0: return QString("Loose");
                case 1: return QString("Soft");
                case 3: return QString("Strict");
                case 4: return QString("Exact only");
                default: return QString("Default");
            }
        },
        [](auto args) {
            if (args.value == "Loose") return 0;
            if (args.value == "Soft") return 1;
            if (args.value == "Strict") return 3;
            if (args.value == "Exact only") return 4;
            return 2;
        },
        false)
        ->setToolTip(
            "How similar two messages need to be before they count as "
            "repeated.");

    SettingWidget::intInput("Repetition threshold",
                            s.repeatedMessagesRepetitionThreshold,
                            {.min = 2, .max = 20})
        ->setTooltip(
            "How many matching messages are required before the counter "
            "appears.")
        ->addTo(layout);

    SettingWidget::colorButton("Repeat counter color",
                               s.repeatedMessagesCounterColor)
        ->setTooltip("Text color for the inline repeated-message counter.")
        ->addTo(layout);

    layout.addStretch();

    // invisible element for width
    auto *inv = new BaseWidget(this);
    //    inv->setScaleIndependantWidth(600);
    layout.addWidget(inv);
}

void TechnorinoPage::initExtra()
{
    /// update cache path
    if (this->cachePath_)
    {
        getSettings()->cachePath.connect(
            [cachePath = this->cachePath_](const auto &, auto) mutable {
                QString newPath = getApp()->getPaths().cacheDirectory();

                QString pathShortened = "Current location: <a href=\"file:///" +
                                        newPath + "\">" +
                                        shortenString(newPath, 50) + "</a>";

                cachePath->setText(pathShortened);
                cachePath->setToolTip(newPath);
            });
    }
}

}  // namespace chatterino

