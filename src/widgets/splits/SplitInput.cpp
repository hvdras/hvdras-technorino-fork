// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/SplitInput.hpp"

#include "Application.hpp"
#include "common/enums/MessageOverflow.hpp"
#include "common/QLogging.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/spellcheck/SpellChecker.hpp"
#include "messages/Link.hpp"
#include "messages/Message.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchCommon.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Fonts.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/translation/Translator.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Helpers.hpp"
#include "util/LayoutCreator.hpp"
#include "util/MultiChannel.hpp"
#include "widgets/buttons/LabelButton.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/dialogs/EmotePopup.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/CmdDeleteKeyFilter.hpp"
#include "widgets/helper/MessageView.hpp"
#include "widgets/helper/ResizingTextEdit.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/Scrollbar.hpp"
#include "widgets/splits/InputCompletionPopup.hpp"
#include "widgets/splits/InputHighlighter.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"

#include <QActionGroup>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QCompleter>
#include <QPainter>
#include <QSignalBlocker>

#include <functional>
#include <ranges>

using namespace Qt::Literals;

namespace chatterino {

namespace {

// Current function: https://www.desmos.com/calculator/vdyamchjwh
qreal highlightEasingFunction(qreal progress)
{
    if (progress <= 0.1)
    {
        return 1.0 - pow(10.0 * progress, 3.0);
    }
    return 1.0 + pow((20.0 / 9.0) * (0.5 * progress - 0.5), 3.0);
}

}  // namespace

SplitInput::SplitInput(Split *_chatWidget, bool enableInlineReplying)
    : SplitInput(_chatWidget, _chatWidget, _chatWidget->view_,
                 enableInlineReplying)
{
}

SplitInput::SplitInput(QWidget *parent, Split *_chatWidget,
                       ChannelView *_channelView, bool enableInlineReplying)
    : BaseWidget(parent)
    , split_(_chatWidget)
    , channelView_(_channelView)
    , enableInlineReplying_(enableInlineReplying)
    , backgroundColorAnimation(this, "backgroundColor"_ba)
{
    this->installEventFilter(this);
    this->initLayout();

    // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer)
    auto *spellChecker = getApp()->getSpellChecker();
    this->inputHighlighter = new InputHighlighter(*spellChecker, this);
    this->updateChannel();

    this->signalHolder_.managedConnect(this->split_->channelChanged, [this] {
        this->updateChannel();
    });

    getSettings()->enableSpellChecking.connect(
        [this] {
            this->checkSpellingChanged();
        },
        this->signalHolder_);

    // misc
    this->installTextEditEvents();
    this->addShortcuts();
    // The textEdit's signal will be destroyed before this SplitInput is
    // destroyed, so we can safely ignore this signal's connection.
    std::ignore = this->ui_.textEdit->focusLost.connect([this] {
        this->hideCompletionPopup();
    });
    this->scaleChangedEvent(this->scale());
    this->signalHolder_.managedConnect(getApp()->getHotkeys()->onItemsUpdated,
                                       [this]() {
                                           this->clearShortcuts();
                                           this->addShortcuts();
                                       });

    QEasingCurve curve;
    curve.setCustomType(highlightEasingFunction);
    this->backgroundColorAnimation.setDuration(500);
    this->backgroundColorAnimation.setEasingCurve(curve);
}

void SplitInput::initLayout()
{
    auto *app = getApp();
    LayoutCreator<SplitInput> layoutCreator(this);

    auto layout =
        layoutCreator.setLayoutType<QVBoxLayout>().withoutMargin().assign(
            &this->ui_.vbox);
    layout->setSpacing(0);
    this->applyOuterMargin();

    // reply label stuff
    auto replyWrapper =
        layout.emplace<QWidget>().assign(&this->ui_.replyWrapper);
    replyWrapper->setContentsMargins(0, 0, 1, 1);

    auto replyVbox =
        replyWrapper.setLayoutType<QVBoxLayout>().withoutMargin().assign(
            &this->ui_.replyVbox);
    replyVbox->setSpacing(1);

    auto replyHbox =
        replyVbox.emplace<QHBoxLayout>().assign(&this->ui_.replyHbox);

    auto *messageVbox = new QVBoxLayout;
    this->ui_.replyMessage = new MessageView();
    messageVbox->addWidget(this->ui_.replyMessage, 0, Qt::AlignLeft);
    messageVbox->setContentsMargins(10, 0, 0, 0);
    replyVbox->addLayout(messageVbox, 0);

    auto replyLabel = replyHbox.emplace<QLabel>().assign(&this->ui_.replyLabel);
    replyLabel->setAlignment(Qt::AlignLeft);
    replyLabel->setFont(
        app->getFonts()->getFont(FontStyle::ChatMedium, this->scale()));

    replyHbox->addStretch(1);

    auto replyCancelButton = replyHbox
                                 .emplace<SvgButton>(
                                     SvgButton::Src{
                                         .dark = ":/buttons/cancel.svg",
                                         .light = ":/buttons/cancelDark.svg",
                                     },
                                     nullptr, QSize{4, 0})
                                 .assign(&this->ui_.cancelReplyButton);

    replyCancelButton->hide();
    replyLabel->hide();

    // command suggestion strip
    {
        auto suggWidget = layout.emplace<QWidget>().assign(
            &this->ui_.commandSuggestionWidget);
        suggWidget->setVisible(false);
        suggWidget->setContentsMargins(4, 2, 4, 2);
        auto *suggLayout = new QHBoxLayout(suggWidget.getElement());
        suggLayout->setSpacing(6);
        suggLayout->setContentsMargins(0, 0, 0, 0);
        this->ui_.commandSuggestionLayout = suggLayout;
        // Placeholder labels — populated by updateCommandSuggestions
        for (int i = 0; i < 5; ++i)
        {
            auto *label = new QLabel();
            label->setVisible(false);
            label->setStyleSheet(
                "QLabel { padding: 1px 4px; border-radius: 3px; "
                "background: rgba(255,255,255,0.08); cursor: pointer; }");
            label->setAlignment(Qt::AlignCenter);
            label->setCursor(Qt::PointingHandCursor);
            suggLayout->addWidget(label);
        }
        suggLayout->addStretch(1);
    }

    auto inputWrapper =
        layout.emplace<QWidget>().assign(&this->ui_.inputWrapper);
    inputWrapper->setContentsMargins(1, 1, 1, 1);

    // hbox for input, right box
    auto hboxLayout =
        inputWrapper.setLayoutType<QHBoxLayout>().withoutMargin().assign(
            &this->ui_.inputHbox);

    // input
    auto textEdit =
        hboxLayout.emplace<ResizingTextEdit>().assign(&this->ui_.textEdit);
    connect(textEdit.getElement(), &ResizingTextEdit::textChanged, this,
            &SplitInput::editTextChanged);
    textEdit->setFrameStyle(QFrame::NoFrame);

    auto *shortcutFilter = new CmdDeleteKeyFilter(this);
    textEdit->installEventFilter(shortcutFilter);

    hboxLayout.emplace<LabelButton>("SEND").assign(&this->ui_.sendButton);
    this->ui_.sendButton->hide();

    QObject::connect(this->ui_.sendButton, &Button::leftClicked, [this] {
        std::vector<QString> arguments;
        this->handleSendMessage(arguments);
    });

    getSettings()->showSendButton.connect(
        [this](const bool value, auto) {
            if (value)
            {
                this->ui_.sendButton->show();
            }
            else
            {
                this->ui_.sendButton->hide();
            }
        },
        this->managedConnections_);


    // right box
    auto box = hboxLayout.emplace<QVBoxLayout>().withoutMargin();
    box->setSpacing(0);
    {
        // Top row: poll/predict/translate buttons + length label
        auto hbox = box.emplace<QHBoxLayout>().withoutMargin();
        hbox->setSpacing(2);

        this->ui_.pollButton = new LabelButton("Poll", nullptr);
        this->ui_.pollButton->setVisible(false);
        hbox->addWidget(this->ui_.pollButton);

        this->ui_.predictButton = new LabelButton("Predict", nullptr);
        this->ui_.predictButton->setVisible(false);
        hbox->addWidget(this->ui_.predictButton);

        this->ui_.translateButton = new LabelButton("TL", nullptr);
        this->ui_.translateButton->setToolTip(
            "Translate input to target language before sending");
        this->ui_.translateButton->setVisible(false);
        hbox->addWidget(this->ui_.translateButton);

        this->ui_.textEditLength = new QLabel();
        this->ui_.textEditLength->setAlignment(Qt::AlignRight);
        hbox->addWidget(this->ui_.textEditLength);

        this->ui_.sendWaitStatus = new QLabel();
        this->ui_.sendWaitStatus->setAlignment(Qt::AlignRight);
        this->ui_.sendWaitStatus->setHidden(true);
        hbox->addWidget(this->ui_.sendWaitStatus);

        // Bottom row: emote button only
        this->ui_.emoteButton = new SvgButton(
            {
                .dark = ":/buttons/emote.svg",
                .light = ":/buttons/emoteDark.svg",
            },
            nullptr, QSize{6, 3});
        box->addWidget(this->ui_.emoteButton, 0, Qt::AlignRight);
    }

    // ---- misc

    // set edit font
    this->ui_.textEdit->setFont(
        app->getFonts()->getFont(FontStyle::ChatMedium, this->scale()));
    QObject::connect(this->ui_.textEdit, &QTextEdit::cursorPositionChanged,
                     this, &SplitInput::onCursorPositionChanged);
    QObject::connect(this->ui_.textEdit, &QTextEdit::textChanged, this,
                     &SplitInput::onTextChanged);

    this->managedConnections_.managedConnect(app->getFonts()->fontChanged,
                                             [this] {
                                                 this->updateFonts();
                                             });

    // open emote popup
    QObject::connect(this->ui_.emoteButton, &Button::leftClicked, [this] {
        this->openEmotePopup();
    });

    // translate button
    QObject::connect(this->ui_.translateButton, &Button::leftClicked, [this] {
        this->translateInput();
    });

    // poll button
    QObject::connect(this->ui_.pollButton, &Button::leftClicked, [this] {
        this->openPollDialog();
    });

    // prediction button
    QObject::connect(this->ui_.predictButton, &Button::leftClicked, [this] {
        this->openPredictionDialog();
    });

    // These must come AFTER emoteButton is created — pajlada calls the
    // callback immediately on connect, so emoteButton must already exist.
    getSettings()->hideEmojiButton.connect(
        [this](const bool, auto) { this->updateEmoteButton(); },
        this->managedConnections_);
    getSettings()->showCommandSuggestions.connect(
        [this](const bool, auto) { this->updateCommandSuggestions(); },
        this->managedConnections_);
    getSettings()->showOutgoingTranslationButton.connect(
        [this](const bool, auto) { this->updateTranslateButton(); },
        this->managedConnections_);
    getSettings()->enablePolls.connect(
        [this](const bool, auto) { this->updatePollPredictButtons(); },
        this->managedConnections_);
    getSettings()->enablePredictions.connect(
        [this](const bool, auto) { this->updatePollPredictButtons(); },
        this->managedConnections_);
    this->managedConnections_.managedConnect(
        this->split_->channelChanged,
        [this] { this->updatePollPredictButtons(); });

    // clear input and remove reply thread
    QObject::connect(this->ui_.cancelReplyButton, &Button::leftClicked, [this] {
        this->setReply(nullptr, {});
    });

    // Forward selection change signal
    QObject::connect(this->ui_.textEdit, &QTextEdit::copyAvailable,
                     [this](bool available) {
                         if (available)
                         {
                             this->selectionChanged.invoke();
                         }
                     });

    // textEditLength visibility
    getSettings()->showMessageLength.connect(
        [this](const bool &value, auto) {
            // this->ui_.textEditLength->setHidden(!value);
            this->editTextChanged();
        },
        this->managedConnections_);

    // sendWaitStatus visibility
    getSettings()->showSendWaitTimer.connect(
        [this](bool value, const auto &) {
            if (!this->ui_.sendWaitStatus->text().isEmpty())
            {
                this->ui_.sendWaitStatus->setHidden(!value);
            }
        },
        this->managedConnections_);
}

void SplitInput::triggerSelfMessageReceived()
{
    if (this->backgroundColorAnimation.state() != QPropertyAnimation::Stopped)
    {
        this->backgroundColorAnimation.stop();
    }
    this->backgroundColorAnimation.setDirection(QPropertyAnimation::Forward);
    this->backgroundColorAnimation.start();
}

void SplitInput::scaleChangedEvent(float scale)
{
    // update the icon size of the buttons
    this->updateEmoteButton();
    this->updateCancelReplyButton();

    // set maximum height
    if (!this->hidden)
    {
        this->setMaximumHeight(this->scaledMaxHeight());
        if (this->replyTarget_ != nullptr)
        {
            this->ui_.vbox->setSpacing(this->marginForTheme());
        }
    }
    this->updateFonts();
}

void SplitInput::themeChangedEvent()
{
    QPalette palette;

    palette.setColor(QPalette::WindowText, this->theme->splits.input.text);

    this->ui_.textEditLength->setPalette(palette);
    this->ui_.sendWaitStatus->setPalette(palette);

    // Theme changed, reset current background color
    this->setBackgroundColor(this->theme->splits.input.background);
    this->backgroundColorAnimation.setStartValue(
        this->theme->splits.input.backgroundPulse);
    this->backgroundColorAnimation.setEndValue(
        this->theme->splits.input.background);
    this->backgroundColorAnimation.stop();
    this->updateTextEditPalette();

    if (this->theme->isLightTheme())
    {
        this->ui_.replyLabel->setStyleSheet("color: #333");
    }
    else
    {
        this->ui_.replyLabel->setStyleSheet("color: #ccc");
    }

    // update vbox
    this->applyOuterMargin();
    if (this->replyTarget_ != nullptr)
    {
        this->ui_.vbox->setSpacing(this->marginForTheme());
    }
}

void SplitInput::updateEmoteButton()
{
    auto scale = this->scale();

    this->ui_.emoteButton->setFixedHeight(int(18 * scale));
    // Make button slightly wider so it's easier to click
    this->ui_.emoteButton->setFixedWidth(int(24 * scale));
    this->ui_.emoteButton->setVisible(!getSettings()->hideEmojiButton);
}

void SplitInput::updateCommandSuggestions()
{
    if (!this->ui_.commandSuggestionWidget ||
        !getSettings()->showCommandSuggestions)
    {
        if (this->ui_.commandSuggestionWidget)
        {
            this->ui_.commandSuggestionWidget->setVisible(false);
        }
        return;
    }

    const auto text = this->ui_.textEdit->toPlainText();
    const auto cursorPos = this->ui_.textEdit->textCursor().position();
    if (cursorPos <= 0 || !text.startsWith('/'))
    {
        this->ui_.commandSuggestionWidget->setVisible(false);
        return;
    }

    // The current "word" being typed (up to cursor)
    const auto partial = text.left(cursorPos).toLower();

    // Gather matching commands from the built-in list
    const auto allCommands =
        getApp()->getCommands()->getDefaultChatterinoCommandList();

    QStringList matches;
    for (const auto &cmd : allCommands)
    {
        if (cmd.startsWith(partial, Qt::CaseInsensitive) && matches.size() < 5)
        {
            matches.append(cmd);
        }
    }

    const auto labels = this->ui_.commandSuggestionLayout;
    for (int i = 0; i < labels->count() - 1; ++i)  // -1 for stretch
    {
        auto *item = labels->itemAt(i);
        if (!item)
        {
            continue;
        }
        auto *label = qobject_cast<QLabel *>(item->widget());
        if (!label)
        {
            continue;
        }

        if (i < matches.size())
        {
            label->setText(matches.at(i));
            label->setVisible(true);

            // Reconnect click to insert this command
            const auto cmd = matches.at(i);
            disconnect(label, nullptr, nullptr, nullptr);
            label->installEventFilter(this);
            label->setProperty("cmdSuggestion", cmd);
        }
        else
        {
            label->setVisible(false);
        }
    }

    this->ui_.commandSuggestionWidget->setVisible(!matches.isEmpty());
}

void SplitInput::updateCancelReplyButton()
{
    float scale = this->scale();

    this->ui_.cancelReplyButton->setFixedHeight(int(12 * scale));
    this->ui_.cancelReplyButton->setFixedWidth(int(20 * scale));
}

void SplitInput::openEmotePopup()
{
    if (!this->emotePopup_)
    {
        this->emotePopup_ = new EmotePopup(this);
        this->emotePopup_->setAttribute(Qt::WA_DeleteOnClose);

        // The EmotePopup is closed & destroyed when this is destroyed, meaning it's safe to ignore this connection
        std::ignore =
            this->emotePopup_->linkClicked.connect([this](const Link &link) {
                if (link.type == Link::InsertText)
                {
                    QTextCursor cursor = this->ui_.textEdit->textCursor();
                    QString textToInsert(link.value + " ");

                    // If symbol before cursor isn't space or empty
                    // Then insert space before emote.
                    if (cursor.position() > 0 &&
                        !this->getInputText()[cursor.position() - 1].isSpace())
                    {
                        textToInsert = " " + textToInsert;
                    }
                    this->insertText(textToInsert);
                    this->ui_.textEdit->activateWindow();
                }
            });
    }

    this->emotePopup_->loadChannel(this->split_->getSelectedChannel());
    this->emotePopup_->show();
    this->emotePopup_->raise();
    this->emotePopup_->activateWindow();
}

QString SplitInput::handleSendMessage(const std::vector<QString> &arguments)
{
    ChannelPtr c;
    if (this->replyTarget_)
    {
        c = this->replyChannel_.lock();
    }
    if (!c)
    {
        c = this->split_->getSelectedChannel();
    }
    if (c == nullptr)
    {
        return "";
    }

    if (!c->isTwitchOrKickChannel() || this->replyTarget_ == nullptr)
    {
        // standard message send behavior
        QString message = this->ui_.textEdit->toPlainText();

        message = message.replace('\n', ' ');
        QString sendMessage =
            getApp()->getCommands()->execCommand(message, c, false);

        c->sendMessage(sendMessage);

        this->postMessageSend(message, arguments);
        return "";
    }

    // Reply to message
    auto *tc = dynamic_cast<TwitchChannel *>(c.get());
    auto *kc = dynamic_cast<KickChannel *>(c.get());
    if (!tc && !kc)
    {
        // this should not fail
        return "";
    }

    QString message = this->ui_.textEdit->toPlainText();

    if (this->enableInlineReplying_)
    {
        // Remove @username prefix that is inserted when doing inline replies
        message.remove(0, this->replyTarget_->displayName.length() +
                              1);  // remove "@username"

        if (!message.isEmpty() && message.at(0) == ' ')
        {
            message.remove(0, 1);  // remove possible space
        }
    }

    message = message.replace('\n', ' ');
    QString sendMessage =
        getApp()->getCommands()->execCommand(message, c, false);

    // Reply within TwitchChannel
    if (tc)
    {
        tc->sendReply(sendMessage, this->replyTarget_->id);
    }
    else if (kc)
    {
        kc->sendReply(sendMessage, this->replyTarget_->id);
    }

    this->postMessageSend(message, arguments);
    return "";
}

void SplitInput::postMessageSend(const QString &message,
                                 const std::vector<QString> &arguments)
{
    // don't add duplicate messages and empty message to message history
    if ((this->prevMsg_.isEmpty() || !this->prevMsg_.endsWith(message)) &&
        !message.trimmed().isEmpty())
    {
        this->prevMsg_.append(message);
    }

    if (arguments.empty() || arguments.at(0) != "keepInput")
    {
        this->clearInput();
    }
    this->prevIndex_ = this->prevMsg_.size();
}

int SplitInput::scaledMaxHeight() const
{
    if (this->replyTarget_ != nullptr)
    {
        // give more space for showing the message being replied to
        return int(250 * this->scale());
    }
    else
    {
        return int(150 * this->scale());
    }
}

void SplitInput::addShortcuts()
{
    HotkeyController::HotkeyMap actions{
        {"cursorToStart",
         [this](const std::vector<QString> &arguments) -> QString {
             if (arguments.size() != 1)
             {
                 qCWarning(chatterinoHotkeys)
                     << "Invalid cursorToStart arguments. Argument 0: select "
                        "(\"withSelection\" or \"withoutSelection\")";
                 return "Invalid cursorToStart arguments. Argument 0: select "
                        "(\"withSelection\" or \"withoutSelection\")";
             }
             QTextCursor cursor = this->ui_.textEdit->textCursor();
             auto place = QTextCursor::Start;
             const auto &stringTakeSelection = arguments.at(0);
             bool select{};
             if (stringTakeSelection == "withSelection")
             {
                 select = true;
             }
             else if (stringTakeSelection == "withoutSelection")
             {
                 select = false;
             }
             else
             {
                 qCWarning(chatterinoHotkeys)
                     << "Invalid cursorToStart select argument (0)!";
                 return "Invalid cursorToStart select argument (0)!";
             }

             cursor.movePosition(place,
                                 select ? QTextCursor::MoveMode::KeepAnchor
                                        : QTextCursor::MoveMode::MoveAnchor);
             this->ui_.textEdit->setTextCursor(cursor);
             return "";
         }},
        {"cursorToEnd",
         [this](const std::vector<QString> &arguments) -> QString {
             if (arguments.size() != 1)
             {
                 qCWarning(chatterinoHotkeys)
                     << "Invalid cursorToEnd arguments. Argument 0: select "
                        "(\"withSelection\" or \"withoutSelection\")";
                 return "Invalid cursorToEnd arguments. Argument 0: select "
                        "(\"withSelection\" or \"withoutSelection\")";
             }
             QTextCursor cursor = this->ui_.textEdit->textCursor();
             auto place = QTextCursor::End;
             const auto &stringTakeSelection = arguments.at(0);
             bool select{};
             if (stringTakeSelection == "withSelection")
             {
                 select = true;
             }
             else if (stringTakeSelection == "withoutSelection")
             {
                 select = false;
             }
             else
             {
                 qCWarning(chatterinoHotkeys)
                     << "Invalid cursorToEnd select argument (0)!";
                 return "Invalid cursorToEnd select argument (0)!";
             }

             cursor.movePosition(place,
                                 select ? QTextCursor::MoveMode::KeepAnchor
                                        : QTextCursor::MoveMode::MoveAnchor);
             this->ui_.textEdit->setTextCursor(cursor);
             return "";
         }},
        {"openEmotesPopup",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             this->openEmotePopup();
             return "";
         }},
        {"sendMessage",
         [this](const std::vector<QString> &arguments) -> QString {
             return this->handleSendMessage(arguments);
         }},
        {"previousMessage",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             if (this->prevMsg_.isEmpty() || this->prevIndex_ == 0)
             {
                 return "";
             }

             if (this->prevIndex_ == (this->prevMsg_.size()))
             {
                 this->currMsg_ = this->ui_.textEdit->toPlainText();
             }

             this->prevIndex_--;
             this->ui_.textEdit->setPlainText(
                 this->prevMsg_.at(this->prevIndex_));
             this->ui_.textEdit->resetCompletion();

             QTextCursor cursor = this->ui_.textEdit->textCursor();
             cursor.movePosition(QTextCursor::End);
             this->ui_.textEdit->setTextCursor(cursor);

             return "";
         }},
        {"nextMessage",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             // If user did not write anything before then just do nothing.
             if (this->prevMsg_.isEmpty())
             {
                 return "";
             }
             bool cursorToEnd = true;
             QString message = this->ui_.textEdit->toPlainText();

             if (this->prevIndex_ != (this->prevMsg_.size() - 1) &&
                 this->prevIndex_ != this->prevMsg_.size())
             {
                 this->prevIndex_++;
                 this->ui_.textEdit->setPlainText(
                     this->prevMsg_.at(this->prevIndex_));
                 this->ui_.textEdit->resetCompletion();
             }
             else
             {
                 this->prevIndex_ = this->prevMsg_.size();
                 if (message == this->prevMsg_.at(this->prevIndex_ - 1))
                 {
                     // If user has just come from a message history
                     // Then simply get currMsg_.
                     this->ui_.textEdit->setPlainText(this->currMsg_);
                     this->ui_.textEdit->resetCompletion();
                 }
                 else if (message != this->currMsg_)
                 {
                     // If user are already in current message
                     // And type something new
                     // Then replace currMsg_ with new one.
                     this->currMsg_ = message;
                 }
                 // If user is already in current message
                 // Then don't touch cursos.
                 cursorToEnd =
                     (message == this->prevMsg_.at(this->prevIndex_ - 1));
             }

             if (cursorToEnd)
             {
                 QTextCursor cursor = this->ui_.textEdit->textCursor();
                 cursor.movePosition(QTextCursor::End);
                 this->ui_.textEdit->setTextCursor(cursor);
             }
             return "";
         }},
        {"undo",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             this->ui_.textEdit->undo();
             return "";
         }},
        {"redo",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             this->ui_.textEdit->redo();
             return "";
         }},
        {"copy",
         [this](const std::vector<QString> &arguments) -> QString {
             // XXX: this action is unused at the moment, a qt standard shortcut is used instead
             if (arguments.empty())
             {
                 return "copy action takes only one argument: the source "
                        "of the copy \"split\", \"input\" or "
                        "\"auto\". If the source is \"split\", only text "
                        "from the chat will be copied. If it is "
                        "\"splitInput\", text from the input box will be "
                        "copied. Automatic will pick whichever has a "
                        "selection";
             }

             bool copyFromSplit = false;
             const auto &mode = arguments.at(0);
             if (mode == "split")
             {
                 copyFromSplit = true;
             }
             else if (mode == "splitInput")
             {
                 copyFromSplit = false;
             }
             else if (mode == "auto")
             {
                 const auto &cursor = this->ui_.textEdit->textCursor();
                 copyFromSplit = !cursor.hasSelection();
             }

             if (copyFromSplit)
             {
                 this->channelView_->copySelectedText();
             }
             else
             {
                 this->ui_.textEdit->copy();
             }
             return "";
         }},
        {"paste",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             this->ui_.textEdit->paste();
             return "";
         }},
        {"clear",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             this->clearInput();
             return "";
         }},
        {"selectAll",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             this->ui_.textEdit->selectAll();
             return "";
         }},
        {"selectWord",
         [this](const std::vector<QString> &arguments) -> QString {
             (void)arguments;

             auto cursor = this->ui_.textEdit->textCursor();
             cursor.select(QTextCursor::WordUnderCursor);
             this->ui_.textEdit->setTextCursor(cursor);
             return "";
         }},
    };

    this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
        HotkeyCategory::SplitInput, actions, this->parentWidget());
}

bool SplitInput::eventFilter(QObject *obj, QEvent *event)
{
    // Handle click on command suggestion labels
    if (event->type() == QEvent::MouseButtonRelease)
    {
        if (auto *label = qobject_cast<QLabel *>(obj))
        {
            const auto cmd = label->property("cmdSuggestion").toString();
            if (!cmd.isEmpty())
            {
                this->ui_.textEdit->setPlainText(cmd + u" "_s);
                auto cursor = this->ui_.textEdit->textCursor();
                cursor.movePosition(QTextCursor::End);
                this->ui_.textEdit->setTextCursor(cursor);
                this->ui_.textEdit->setFocus();
                return true;
            }
        }
    }

    if (event->type() == QEvent::ShortcutOverride ||
        event->type() == QEvent::Shortcut)
    {
        if (auto *popup = this->inputCompletionPopup_.data())
        {
            if (popup->isVisible())
            {
                // Stop shortcut from triggering by saying we will handle it ourselves
                event->accept();

                // Return false means the underlying event isn't stopped, it will continue to propagate
                return false;
            }
        }
    }

    return BaseWidget::eventFilter(obj, event);
}

void SplitInput::installTextEditEvents()
{
    // We can safely ignore this signal's connection because SplitInput owns
    // the textEdit object, so it will always be deleted before SplitInput
    std::ignore =
        this->ui_.textEdit->keyPressed.connect([this](QKeyEvent *event) {
            if (auto *popup = this->inputCompletionPopup_.data())
            {
                if (popup->isVisible())
                {
                    if (popup->eventFilter(nullptr, event))
                    {
                        event->accept();
                        return;
                    }
                }
            }

            // One of the last remaining of it's kind, the copy shortcut.
            // For some bizarre reason Qt doesn't want this key be rebound.
            // TODO(Mm2PL): Revisit in Qt6, maybe something changed?
            if ((event->key() == Qt::Key_C || event->key() == Qt::Key_Insert) &&
                event->modifiers() == Qt::ControlModifier)
            {
                if (this->channelView_->hasSelection())
                {
                    this->channelView_->copySelectedText();
                    event->accept();
                }
            }
        });

    std::ignore = this->ui_.textEdit->contextMenuRequested.connect(
        [this](QMenu *menu, QPoint pos) {
            auto channel = this->split_->getChannel();
            if (auto *mc = dynamic_cast<MultiChannel *>(channel.get()))
            {
                auto channels = mc->channels();
                auto currentIdx = mc->activeChannelIndex();
                if (!channels.empty())
                {
                    auto *submenu = menu->addMenu("Set Context");
                    auto *group = new QActionGroup(submenu);

                    for (size_t i = 0; i < channels.size(); i++)
                    {
                        QString name = channels[i].channel->getName() % u" (";
                        name +=
                            qmagicenum::enumNameString(channels[i].platform);
                        name += ')';
                        auto *action = new QAction(name, submenu);
                        action->setActionGroup(group);
                        action->setCheckable(true);
                        action->setChecked(i == currentIdx);
                        QObject::connect(
                            action, &QAction::toggled, this,
                            [this, i](bool checked) {
                                if (!checked)
                                {
                                    return;
                                }
                                auto *mc = dynamic_cast<MultiChannel *>(
                                    this->split_->getChannel().get());
                                mc->setActiveChannelIndex(i);
                                getApp()
                                    ->getWindows()
                                    ->forceLayoutChannelViews();
                            });
                        submenu->addAction(action);
                    }
                }
            }

#ifdef CHATTERINO_WITH_SPELLCHECK
            menu->addSeparator();
            auto *spellcheckAction = new QAction("Check spelling", menu);
            spellcheckAction->setCheckable(true);
            spellcheckAction->setChecked(this->shouldCheckSpelling());
            QObject::connect(spellcheckAction, &QAction::toggled, this,
                             [this](bool enabled) {
                                 this->checkSpellingOverride_ = enabled;
                                 this->checkSpellingChanged();
                             });
            menu->addAction(spellcheckAction);

            int nSuggestions = getSettings()->nSpellCheckingSuggestions;
            if (nSuggestions < 0)
            {
                nSuggestions = std::numeric_limits<int>::max();
            }

            if (!this->inputHighlighter || nSuggestions == 0)
            {
                return;
            }

            auto cursorAtPos = this->ui_.textEdit->cursorForPosition(pos);
            QString text = this->ui_.textEdit->toPlainText();
            QStringView word =
                this->inputHighlighter->getWordAt(text, cursorAtPos.position());
            if (!word.isEmpty())
            {
                auto cursor = this->ui_.textEdit->textCursor();
                // Select `word`. `word` is a view into `text`, so we can use
                // the offsets of `word` from the start of `text`.
                cursor.setPosition(
                    static_cast<int>(word.begin() - text.begin()));
                cursor.setPosition(static_cast<int>(word.end() - text.begin()),
                                   QTextCursor::KeepAnchor);

                auto suggestions =
                    getApp()->getSpellChecker()->suggestions(word.toString());
                for (const auto &sugg :
                     suggestions | std::views::take(nSuggestions))
                {
                    auto qSugg = QString::fromStdString(sugg);
                    menu->addAction(qSugg, [this, qSugg, cursor]() mutable {
                        cursor.insertText(qSugg);
                        this->ui_.textEdit->setTextCursor(cursor);
                    });
                }
            }
#else
            (void)menu;
            (void)pos;
            (void)this;
#endif
        });
}

void SplitInput::mousePressEvent(QMouseEvent *event)
{
    this->giveFocus(Qt::MouseFocusReason);

    if (this->hidden)
    {
        BaseWidget::mousePressEvent(event);
    }
    // else, don't call QWidget::mousePressEvent,
    // which will call event->ignore()
}

void SplitInput::onTextChanged()
{
    this->updateCompletionPopup();
    this->updateCommandSuggestions();
}

void SplitInput::onCursorPositionChanged()
{
    this->updateCompletionPopup();
    this->updateCommandSuggestions();
}

void SplitInput::updateCompletionPopup()
{
    auto *channel = this->split_->getSelectedChannel().get();
    auto *tc = dynamic_cast<TwitchChannel *>(channel);
    bool showEmoteCompletion = getSettings()->emoteCompletionWithColon;
    bool showUsernameCompletion =
        tc != nullptr && getSettings()->showUsernameCompletionMenu;
    if (!showEmoteCompletion && !showUsernameCompletion)
    {
        this->hideCompletionPopup();
        return;
    }

    // check if in completion prefix
    auto &edit = *this->ui_.textEdit;

    auto text = edit.toPlainText();
    auto position = edit.textCursor().position() - 1;

    if (text.length() == 0 || position == -1)
    {
        this->hideCompletionPopup();
        return;
    }

    for (int i = std::clamp(position, 0, (int)text.length() - 1); i >= 0; i--)
    {
        if (text[i] == ' ')
        {
            this->hideCompletionPopup();
            return;
        }

        if (text[i] == ':' && showEmoteCompletion)
        {
            if (i == 0 || text[i - 1].isSpace())
            {
                this->showCompletionPopup(text.mid(i, position - i + 1),
                                          CompletionKind::Emote);
            }
            else
            {
                this->hideCompletionPopup();
            }
            return;
        }

        if (text[i] == '@' && showUsernameCompletion)
        {
            if (i == 0 || text[i - 1].isSpace())
            {
                this->showCompletionPopup(text.mid(i, position - i + 1),
                                          CompletionKind::User);
            }
            else
            {
                this->hideCompletionPopup();
            }
            return;
        }
    }

    this->hideCompletionPopup();
}

void SplitInput::showCompletionPopup(const QString &text, CompletionKind kind)
{
    if (this->inputCompletionPopup_.isNull())
    {
        this->inputCompletionPopup_ = new InputCompletionPopup(this);
        this->inputCompletionPopup_->setInputAction(
            [that = QPointer(this)](const QString &text) mutable {
                if (auto *this2 = that.data())
                {
                    this2->insertCompletionText(text);
                    this2->hideCompletionPopup();
                }
            });
    }

    auto *popup = this->inputCompletionPopup_.data();
    assert(popup);

    popup->updateCompletion(text, kind, this->split_->getSelectedChannel());

    auto pos = this->mapToGlobal(QPoint{0, 0}) - QPoint(0, popup->height()) +
               QPoint((this->width() - popup->width()) / 2, 0);

    popup->move(pos);
    popup->show();
}

void SplitInput::hideCompletionPopup()
{
    if (auto *popup = this->inputCompletionPopup_.data())
    {
        popup->hide();
    }
}

void SplitInput::insertCompletionText(const QString &input_) const
{
    auto &edit = *this->ui_.textEdit;
    auto input = input_ + ' ';

    auto text = edit.toPlainText();
    auto position = edit.textCursor().position() - 1;

    for (int i = std::clamp(position, 0, (int)text.length() - 1); i >= 0; i--)
    {
        bool done = false;
        if (text[i] == ':')
        {
            done = true;
        }
        else if (text[i] == '@')
        {
            const auto userMention =
                formatUserMention(input_, edit.isFirstWord(),
                                  getSettings()->mentionUsersWithComma);
            input = "@" + userMention + " ";
            done = true;
        }

        if (done)
        {
            auto cursor = edit.textCursor();
            edit.setPlainText(
                text.remove(i, position - i + 1).insert(i, input));

            cursor.setPosition(i + input.size());
            edit.setTextCursor(cursor);
            break;
        }
    }
}

bool SplitInput::hasSelection() const
{
    return this->ui_.textEdit->textCursor().hasSelection();
}

void SplitInput::clearSelection() const
{
    auto cursor = this->ui_.textEdit->textCursor();
    cursor.clearSelection();
    this->ui_.textEdit->setTextCursor(cursor);
}

bool SplitInput::isEditFirstWord() const
{
    return this->ui_.textEdit->isFirstWord();
}

QString SplitInput::getInputText() const
{
    return this->ui_.textEdit->toPlainText();
}

void SplitInput::insertText(const QString &text)
{
    this->ui_.textEdit->insertPlainText(text);
}

void SplitInput::hide()
{
    if (this->isHidden())
    {
        return;
    }

    this->hidden = true;
    this->setMaximumHeight(0);
    this->updateGeometry();
}

void SplitInput::show()
{
    if (!this->isHidden())
    {
        return;
    }

    this->hidden = false;
    this->setMaximumHeight(this->scaledMaxHeight());
    this->updateGeometry();
}

bool SplitInput::isHidden() const
{
    return this->hidden;
}

void SplitInput::setInputText(const QString &newInputText)
{
    this->ui_.textEdit->setPlainText(newInputText);
}

void SplitInput::editTextChanged()
{
    auto *app = getApp();

    // set textLengthLabel value
    QString text = this->ui_.textEdit->toPlainText();

    if (this->shouldPreventInput(text))
    {
        this->ui_.textEdit->setPlainText(text.left(TWITCH_MESSAGE_LIMIT));
        this->ui_.textEdit->moveCursor(QTextCursor::EndOfBlock);
        return;
    }

    if (text.startsWith("/r ", Qt::CaseInsensitive) &&
        this->split_->getSelectedChannel()->isTwitchChannel())
    {
        auto lastUser = app->getTwitch()->getLastUserThatWhisperedMe();
        if (!lastUser.isEmpty())
        {
            this->ui_.textEdit->setPlainText("/w " + lastUser + text.mid(2));
            this->ui_.textEdit->moveCursor(QTextCursor::EndOfBlock);
        }
    }
    else
    {
        this->textChanged.invoke(text);

        text = text.trimmed();
        text = app->getCommands()->execCommand(text, this->split_->getChannel(),
                                               true);
    }

    if (text.length() > 0 &&
        getSettings()->messageOverflow.getValue() == MessageOverflow::Highlight)
    {
        QTextCursor cursor = this->ui_.textEdit->textCursor();
        QTextCharFormat format;
        QList<QTextEdit::ExtraSelection> selections;

        cursor.setPosition(qMin(text.length(), TWITCH_MESSAGE_LIMIT),
                           QTextCursor::MoveAnchor);
        cursor.movePosition(QTextCursor::Start, QTextCursor::KeepAnchor);
        selections.append({cursor, format});

        if (text.length() > TWITCH_MESSAGE_LIMIT)
        {
            cursor.setPosition(TWITCH_MESSAGE_LIMIT, QTextCursor::MoveAnchor);
            cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
            format.setForeground(Qt::red);
            selections.append({cursor, format});
        }
        // block reemit of QTextEdit::textChanged()
        {
            const QSignalBlocker b(this->ui_.textEdit);
            this->ui_.textEdit->setExtraSelections(selections);
        }
    }

    QString labelText;

    if (text.length() > 0 && getSettings()->showMessageLength)
    {
        labelText = QString::number(text.length());
        if (text.length() > TWITCH_MESSAGE_LIMIT)
        {
            this->ui_.textEditLength->setStyleSheet("color: red");
        }
        else
        {
            this->ui_.textEditLength->setStyleSheet("");
        }
    }
    else
    {
        labelText = "";
    }

    this->ui_.textEditLength->setText(labelText);

    bool hasReply = false;
    if (this->enableInlineReplying_)
    {
        if (this->replyTarget_ != nullptr)
        {
            // Check if the input still starts with @username. If not, don't reply.
            //
            // We need to verify that
            // 1. the @username prefix exists and
            // 2. if a character exists after the @username, it is a space
            QString replyPrefix = "@" + this->replyTarget_->displayName;
            if (!text.startsWith(replyPrefix) ||
                (text.length() > replyPrefix.length() &&
                 text.at(replyPrefix.length()) != ' '))
            {
                this->clearReplyTarget();
            }
        }

        // Show/hide reply label if inline replies are possible
        hasReply = this->replyTarget_ != nullptr;
    }

    this->ui_.replyWrapper->setVisible(hasReply);
    this->ui_.replyLabel->setVisible(hasReply);
    this->ui_.cancelReplyButton->setVisible(hasReply);
}

void SplitInput::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);

    QColor borderColor =
        this->theme->isLightTheme() ? QColor("#ccc") : QColor("#333");

    QRect baseRect = this->rect();
    baseRect.setWidth(baseRect.width() - 1);

    auto *inputWrap = this->ui_.inputWrapper;
    auto inputBoxRect = inputWrap->geometry();
    inputBoxRect.setSize(inputBoxRect.size() - QSize{1, 1});

    painter.setBrush({this->theme->splits.input.background});
    painter.setPen(borderColor);
    painter.drawRect(inputBoxRect);

    if (this->enableInlineReplying_ && this->replyTarget_ != nullptr)
    {
        auto replyRect = this->ui_.replyWrapper->geometry();
        replyRect.setSize(replyRect.size() - QSize{1, 1});

        painter.setBrush(this->theme->splits.input.background);
        painter.setPen(borderColor);
        painter.drawRect(replyRect);

        QPoint replyLabelBorderStart(
            replyRect.x(),
            replyRect.y() + this->ui_.replyHbox->geometry().height());
        QPoint replyLabelBorderEnd(replyRect.right(),
                                   replyLabelBorderStart.y());
        painter.drawLine(replyLabelBorderStart, replyLabelBorderEnd);
    }
}

void SplitInput::resizeEvent(QResizeEvent *event)
{
    (void)event;

    if (this->height() == this->maximumHeight())
    {
        this->ui_.textEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    }
    else
    {
        this->ui_.textEdit->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    }

    this->ui_.replyMessage->setWidth(this->replyMessageWidth());
}

void SplitInput::giveFocus(Qt::FocusReason reason)
{
    this->ui_.textEdit->setFocus(reason);
}

void SplitInput::setReply(MessagePtr target, std::weak_ptr<Channel> channel)
{
    auto oldParent = this->replyTarget_;
    if (this->enableInlineReplying_ && oldParent)
    {
        // Remove old reply prefix
        auto replyPrefix = "@" + oldParent->displayName;
        auto plainText = this->ui_.textEdit->toPlainText().trimmed();
        if (plainText.startsWith(replyPrefix))
        {
            plainText.remove(0, replyPrefix.length());
        }
        this->ui_.textEdit->setPlainText(plainText.trimmed());
        this->ui_.textEdit->moveCursor(QTextCursor::EndOfBlock);
        this->ui_.textEdit->resetCompletion();
    }

    if (target != nullptr)
    {
        this->replyTarget_ = std::move(target);
        this->replyChannel_ = std::move(channel);

        if (this->enableInlineReplying_)
        {
            this->ui_.replyMessage->setWidth(this->replyMessageWidth());
            this->ui_.replyMessage->setMessage(this->replyTarget_);

            // add spacing between reply box and input box
            this->ui_.vbox->setSpacing(this->marginForTheme());
            if (!this->isHidden())
            {
                // update maximum height to give space for message
                this->setMaximumHeight(this->scaledMaxHeight());
            }

            // Only enable reply label if inline replying
            auto replyPrefix = "@" + this->replyTarget_->displayName;
            auto plainText = this->ui_.textEdit->toPlainText().trimmed();

            // This makes it so if plainText contains "@StreamerFan" and
            // we are replying to "@Streamer" we don't just leave "Fan"
            // in the text box
            if (plainText.startsWith(replyPrefix))
            {
                if (plainText.length() > replyPrefix.length())
                {
                    if (plainText.at(replyPrefix.length()) == ',' ||
                        plainText.at(replyPrefix.length()) == ' ')
                    {
                        plainText.remove(0, replyPrefix.length() + 1);
                    }
                }
                else
                {
                    plainText.remove(0, replyPrefix.length());
                }
            }
            if (!plainText.isEmpty() && !plainText.startsWith(' '))
            {
                replyPrefix.append(' ');
            }
            this->ui_.textEdit->setPlainText(replyPrefix + plainText + " ");
            this->ui_.textEdit->moveCursor(QTextCursor::EndOfBlock);
            this->ui_.textEdit->resetCompletion();
            this->ui_.replyLabel->setText("Replying to @" +
                                          this->replyTarget_->displayName);
        }
    }
    else
    {
        this->replyTarget_.reset();
        this->replyChannel_.reset();

        if (this->enableInlineReplying_)
        {
            this->clearReplyTarget();
        }
    }
}

void SplitInput::setPlaceholderText(const QString &text)
{
    this->ui_.textEdit->setPlaceholderText(text);
}

void SplitInput::clearInput()
{
    this->currMsg_ = "";
    this->ui_.textEdit->setText("");
    this->ui_.textEdit->moveCursor(QTextCursor::Start);
    if (this->enableInlineReplying_)
    {
        this->clearReplyTarget();
    }
}

void SplitInput::clearReplyTarget()
{
    this->replyTarget_.reset();
    this->ui_.replyMessage->clearMessage();
    this->ui_.vbox->setSpacing(0);
    if (!this->isHidden())
    {
        this->setMaximumHeight(this->scaledMaxHeight());
    }
}

bool SplitInput::shouldPreventInput(const QString &text) const
{
    if (getSettings()->messageOverflow.getValue() != MessageOverflow::Prevent)
    {
        return false;
    }

    auto channel = this->split_->getSelectedChannel();

    if (channel == nullptr)
    {
        return false;
    }

    if (!channel->isTwitchChannel())
    {
        // Don't respect this setting for IRC channels as the limits might be server-specific
        return false;
    }

    return text.length() > TWITCH_MESSAGE_LIMIT;
}

int SplitInput::marginForTheme() const
{
    if (this->theme->isLightTheme())
    {
        return int(3 * this->scale());
    }
    else
    {
        return int(1 * this->scale());
    }
}

void SplitInput::applyOuterMargin()
{
    auto margin = std::max(this->marginForTheme() - 1, 0);
    this->ui_.vbox->setContentsMargins(margin, margin, margin, margin);
}

int SplitInput::replyMessageWidth() const
{
    return this->ui_.inputWrapper->width() - 1 - 10;
}

void SplitInput::updateTextEditPalette()
{
    QPalette p;

    // Placeholder text color
    p.setColor(QPalette::PlaceholderText,
               this->theme->messages.textColors.chatPlaceholder);

    // Text color
    p.setColor(QPalette::Text, this->theme->messages.textColors.regular);

    // Selection background color
    p.setBrush(QPalette::Highlight,
               this->theme->isLightTheme()
                   ? QColor(u"#68B1FF"_s)
                   : this->theme->tabs.selected.backgrounds.regular);

    // Background color
    p.setBrush(QPalette::Base, this->backgroundColor());

    this->ui_.textEdit->setPalette(p);
}

QColor SplitInput::backgroundColor() const
{
    return this->backgroundColor_;
}

void SplitInput::setBackgroundColor(QColor newColor)
{
    this->backgroundColor_ = newColor;

    this->updateTextEditPalette();
}

std::optional<bool> SplitInput::checkSpellingOverride() const
{
    return this->checkSpellingOverride_;
}

void SplitInput::setCheckSpellingOverride(std::optional<bool> override)
{
    this->checkSpellingOverride_ = override;
    this->checkSpellingChanged();
}

bool SplitInput::shouldCheckSpelling() const
{
    if (this->checkSpellingOverride_)
    {
        return *this->checkSpellingOverride_;
    }
    return getSettings()->enableSpellChecking;
}

void SplitInput::checkSpellingChanged()
{
    QTextDocument *target = nullptr;
    if (this->shouldCheckSpelling())
    {
        target = this->ui_.textEdit->document();
    }

    if (this->inputHighlighter->document() != target)
    {
        this->inputHighlighter->setDocument(target);
    }
}

void SplitInput::updateFonts()
{
    auto *app = getApp();
    this->ui_.textEdit->setFont(
        app->getFonts()->getFont(FontStyle::ChatMedium, this->scale()));

    // NOTE: We're using TimestampMedium here to get a font that uses the tnum font feature,
    // meaning numbers get equal width & don't bounce around while the user is typing.
    auto tsMedium =
        app->getFonts()->getFont(FontStyle::TimestampMedium, this->scale());
    this->ui_.textEditLength->setFont(tsMedium);
    this->ui_.sendWaitStatus->setFont(tsMedium);
    this->ui_.replyLabel->setFont(
        app->getFonts()->getFont(FontStyle::ChatMediumBold, this->scale()));
}

void SplitInput::setSendWaitStatus(const QString &text) const
{
    this->ui_.sendWaitStatus->setText(text);
    if (text.isEmpty())
    {
        this->ui_.sendWaitStatus->setHidden(true);
    }
    else
    {
        this->ui_.sendWaitStatus->setHidden(!getSettings()->showSendWaitTimer);
    }
}

void SplitInput::updateChannel()
{
    this->channelConnections_.clear();

    auto channel = this->split_->getChannel();
    if (auto *multiChannel = dynamic_cast<MultiChannel *>(channel.get()))
    {
        this->channelConnections_.managedConnect(
            multiChannel->activeChannelChanged, [this] {
                auto selected = this->split_->getSelectedChannel();
                this->ui_.textEdit->setCompleter(
                    new QCompleter(selected->completionModel));
                this->inputHighlighter->setChannel(selected);
            });
    }

    auto selected = this->split_->getSelectedChannel();
    this->ui_.textEdit->setCompleter(new QCompleter(selected->completionModel));
    this->inputHighlighter->setChannel(selected);
}

void SplitInput::updateTranslateButton()
{
    if (this->ui_.translateButton)
    {
        this->ui_.translateButton->setVisible(
            getSettings()->showOutgoingTranslationButton);
    }
}

void SplitInput::updatePollPredictButtons()
{
    auto *tc = dynamic_cast<TwitchChannel *>(
        this->split_->getChannel().get());

    // Mods can manage (end/lock/resolve) but only broadcaster can create
    const bool hasMod = tc != nullptr && tc->hasModRights();
    const bool isBroadcaster = tc != nullptr && tc->isBroadcaster();

    if (this->ui_.pollButton)
    {
        const bool hasPoll = tc && [tc] {
            auto p = tc->accessPoll();
            return p->has_value();
        }();
        // Show if: broadcaster (can create or end) OR mod with active poll (can end)
        this->ui_.pollButton->setVisible(
            getSettings()->enablePolls &&
            (isBroadcaster || (hasMod && hasPoll)));
        this->ui_.pollButton->setText(hasPoll ? "End Poll" : "Poll");
    }

    if (this->ui_.predictButton)
    {
        const bool hasPred = tc && [tc] {
            auto p = tc->accessPrediction();
            return p->has_value();
        }();
        // Show if: broadcaster (can create or manage) OR mod with active prediction (can manage)
        this->ui_.predictButton->setVisible(
            getSettings()->enablePredictions &&
            (isBroadcaster || (hasMod && hasPred)));
        this->ui_.predictButton->setText(hasPred ? "Prediction ▾" : "Predict");
    }
}

void SplitInput::openPollDialog()
{
    auto shared = std::dynamic_pointer_cast<TwitchChannel>(
        this->split_->getChannel());
    auto *tc = shared.get();
    if (!tc)
    {
        return;
    }
    const auto weak = tc->weakFromThis();

    // If a poll is active, offer to end it
    {
        auto poll = tc->accessPoll();
        if (poll->has_value())
        {
            auto *menu = new QMenu(this);
            menu->setAttribute(Qt::WA_DeleteOnClose);
            menu->addAction("End poll (archive)", [weak] {
                if (auto s = std::dynamic_pointer_cast<TwitchChannel>(
                        weak.lock()))
                {
                    getHelix()->endPoll(
                        s->roomId(), s->accessPoll()->value().id, true,
                        [](const HelixPoll &) {},
                        [](const QString &) {});
                }
            });
            menu->addAction("End poll (show results)", [weak] {
                if (auto s = std::dynamic_pointer_cast<TwitchChannel>(
                        weak.lock()))
                {
                    getHelix()->endPoll(
                        s->roomId(), s->accessPoll()->value().id, false,
                        [](const HelixPoll &) {},
                        [](const QString &) {});
                }
            });
            menu->popup(QCursor::pos());
            return;
        }
    }

    // Create a new poll
    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle("Create Poll");
    dlg->setMinimumWidth(380);

    auto *form = new QFormLayout;

    auto *titleEdit = new QLineEdit(dlg);
    titleEdit->setPlaceholderText("Poll question");
    form->addRow("Title:", titleEdit);

    auto *durationSpin = new QSpinBox(dlg);
    durationSpin->setRange(15, 1800);
    durationSpin->setValue(60);
    durationSpin->setSuffix(" seconds");
    form->addRow("Duration:", durationSpin);

    QVector<QLineEdit *> choiceEdits;
    for (int i = 0; i < 5; ++i)
    {
        auto *edit = new QLineEdit(dlg);
        edit->setPlaceholderText(i < 2 ? "Required" : "Optional");
        form->addRow(QString("Choice %1:").arg(i + 1), edit);
        choiceEdits.push_back(edit);
    }

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dlg);

    auto *vbox = new QVBoxLayout(dlg);
    vbox->addLayout(form);
    vbox->addWidget(buttons);

    QObject::connect(buttons, &QDialogButtonBox::accepted, dlg,
                     [dlg, weak, titleEdit, durationSpin, choiceEdits] {
                         auto s = std::dynamic_pointer_cast<TwitchChannel>(
                             weak.lock());
                         if (!s)
                         {
                             dlg->reject();
                             return;
                         }
                         const auto title = titleEdit->text().trimmed();
                         if (title.isEmpty())
                         {
                             return;
                         }
                         QStringList choices;
                         for (auto *e : choiceEdits)
                         {
                             const auto t = e->text().trimmed();
                             if (!t.isEmpty())
                             {
                                 choices.append(t);
                             }
                         }
                         if (choices.size() < 2)
                         {
                             QMessageBox::warning(
                                 dlg, "Create Poll",
                                 "At least 2 choices are required.");
                             return;
                         }
                         const auto chanWeak = s->weakFromThis();
                         dlg->accept();  // close before async call
                         getHelix()->createPoll(
                             s->roomId(), title, choices,
                             std::chrono::seconds(durationSpin->value()),
                             0,
                             [] {},
                             [chanWeak](const QString &err) {
                                 if (auto ch =
                                         std::dynamic_pointer_cast<TwitchChannel>(
                                             chanWeak.lock()))
                                 {
                                     ch->addSystemMessage(
                                         QStringLiteral("Failed to create poll: %1")
                                             .arg(err));
                                 }
                             });
                     });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dlg,
                     &QDialog::reject);

    dlg->show();
}

void SplitInput::openPredictionDialog()
{
    auto shared = std::dynamic_pointer_cast<TwitchChannel>(
        this->split_->getChannel());
    auto *tc = shared.get();
    if (!tc)
    {
        return;
    }
    const auto weak = tc->weakFromThis();

    // If a prediction is active, show management options
    {
        auto pred = tc->accessPrediction();
        if (pred->has_value())
        {
            const auto predId = pred->value().id;
            const auto outcomes = pred->value().outcomes;
            auto *menu = new QMenu(this);
            menu->setAttribute(Qt::WA_DeleteOnClose);
            menu->addAction("Lock prediction", [weak, predId] {
                if (auto s = std::dynamic_pointer_cast<TwitchChannel>(
                        weak.lock()))
                {
                    getHelix()->endPrediction(
                        s->roomId(), predId, false, QString{},
                        [](const HelixPrediction &) {},
                        [](const QString &) {});
                }
            });
            menu->addSeparator();
            for (const auto &o : outcomes)
            {
                const auto oid = o.id;
                const auto otitle = o.title;
                menu->addAction("Resolve: " + otitle,
                                [weak, predId, oid] {
                                    if (auto s =
                                            std::dynamic_pointer_cast<
                                                TwitchChannel>(weak.lock()))
                                    {
                                        getHelix()->endPrediction(
                                            s->roomId(), predId, false,
                                            oid,
                                            [](const HelixPrediction &) {},
                                            [](const QString &) {});
                                    }
                                });
            }
            menu->addSeparator();
            menu->addAction("Cancel & refund", [weak, predId] {
                if (auto s = std::dynamic_pointer_cast<TwitchChannel>(
                        weak.lock()))
                {
                    getHelix()->endPrediction(
                        s->roomId(), predId, true, QString{},
                        [](const HelixPrediction &) {},
                        [](const QString &) {});
                }
            });
            menu->popup(QCursor::pos());
            return;
        }
    }

    // Create a new prediction
    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle("Create Prediction");
    dlg->setMinimumWidth(380);

    auto *form = new QFormLayout;

    auto *titleEdit = new QLineEdit(dlg);
    titleEdit->setPlaceholderText("Prediction title");
    form->addRow("Title:", titleEdit);

    auto *durationSpin = new QSpinBox(dlg);
    durationSpin->setRange(30, 1800);
    durationSpin->setValue(300);
    durationSpin->setSuffix(" seconds");
    form->addRow("Duration:", durationSpin);

    auto *outcome1Edit = new QLineEdit(dlg);
    outcome1Edit->setText("Yes");
    form->addRow("Outcome 1 (Blue):", outcome1Edit);

    auto *outcome2Edit = new QLineEdit(dlg);
    outcome2Edit->setText("No");
    form->addRow("Outcome 2 (Pink):", outcome2Edit);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dlg);

    auto *vbox = new QVBoxLayout(dlg);
    vbox->addLayout(form);
    vbox->addWidget(buttons);

    QObject::connect(
        buttons, &QDialogButtonBox::accepted, dlg,
        [dlg, weak, titleEdit, durationSpin, outcome1Edit, outcome2Edit] {
            auto s = std::dynamic_pointer_cast<TwitchChannel>(weak.lock());
            if (!s)
            {
                dlg->reject();
                return;
            }
            const auto title = titleEdit->text().trimmed();
            const auto o1 = outcome1Edit->text().trimmed();
            const auto o2 = outcome2Edit->text().trimmed();
            if (title.isEmpty() || o1.isEmpty() || o2.isEmpty())
            {
                return;
            }
            const auto chanWeak = s->weakFromThis();
            dlg->accept();  // close before async call
            getHelix()->createPrediction(
                s->roomId(), title, QStringList{o1, o2},
                std::chrono::seconds(durationSpin->value()),
                [] {},
                [chanWeak](const QString &err) {
                    if (auto ch = std::dynamic_pointer_cast<TwitchChannel>(
                            chanWeak.lock()))
                    {
                        ch->addSystemMessage(
                            QStringLiteral("Failed to create prediction: %1")
                                .arg(err));
                    }
                });
        });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dlg,
                     &QDialog::reject);

    dlg->show();
}

void SplitInput::translateInput()
{
    const auto text = this->ui_.textEdit->toPlainText().trimmed();
    if (text.isEmpty())
    {
        return;
    }

    const auto targetLanguage = normalizedTranslationTargetLanguage(
        getSettings()->messageTranslationTargetLanguage.getValue());

    this->ui_.translateButton->setEnabled(false);

    requestTextTranslation(
        text, targetLanguage, this,
        [this](const TranslationResult &result) {
            auto translated = result.translatedText.trimmed();
            translated.replace('\n', ' ');
            if (!translated.isEmpty())
            {
                this->ui_.textEdit->setPlainText(translated);
                auto cursor = this->ui_.textEdit->textCursor();
                cursor.movePosition(QTextCursor::End);
                this->ui_.textEdit->setTextCursor(cursor);
            }
            if (this->ui_.translateButton)
            {
                this->ui_.translateButton->setEnabled(true);
            }
        },
        [this](const QString &error) {
            Q_UNUSED(error)
            if (this->ui_.translateButton)
            {
                this->ui_.translateButton->setEnabled(true);
            }
        });
}

}  // namespace chatterino
