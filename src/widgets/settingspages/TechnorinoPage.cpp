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

