// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/PollBanner.hpp"

#include "Application.hpp"
#include "singletons/Theme.hpp"

#include <QStringBuilder>

namespace chatterino {

PollBanner::PollBanner(QWidget *parent)
    : BaseWidget(parent)
{
    this->layout_ = new QVBoxLayout(this);
    this->layout_->setContentsMargins(8, 4, 8, 4);
    this->layout_->setSpacing(2);

    this->titleLabel_ = new QLabel(this);
    this->titleLabel_->setWordWrap(true);
    this->titleLabel_->setStyleSheet("font-weight: bold;");
    this->layout_->addWidget(this->titleLabel_);

    this->choicesLabel_ = new QLabel(this);
    this->choicesLabel_->setWordWrap(true);
    this->layout_->addWidget(this->choicesLabel_);

    this->statusLabel_ = new QLabel(this);
    this->statusLabel_->setAlignment(Qt::AlignRight);
    this->layout_->addWidget(this->statusLabel_);

    this->setVisible(false);
}

void PollBanner::setPoll(const std::optional<TwitchChannel::PollEvent> &poll)
{
    this->current_ = poll;
    this->rebuild();
}

bool PollBanner::hasPoll() const
{
    return this->current_.has_value();
}

void PollBanner::rebuild()
{
    if (!this->current_.has_value())
    {
        this->setVisible(false);
        return;
    }

    const auto &poll = *this->current_;

    this->titleLabel_->setText(QStringLiteral("Poll: ") + poll.title);

    int totalVotes = 0;
    for (const auto &c : poll.choices)
    {
        totalVotes += c.votes;
    }

    QStringList parts;
    for (const auto &c : poll.choices)
    {
        const int pct =
            totalVotes > 0 ? int(c.votes * 100 / totalVotes) : 0;
        parts.append(c.title % QStringLiteral(" — ") %
                     QString::number(c.votes) % QStringLiteral(" votes (") %
                     QString::number(pct) % QStringLiteral("%)"));
    }
    this->choicesLabel_->setText(parts.join(QStringLiteral("   ·   ")));

    QString statusText = QStringLiteral("Active");
    if (poll.status != QStringLiteral("ACTIVE"))
    {
        statusText = poll.status;
    }
    this->statusLabel_->setText(statusText);

    this->setVisible(true);
}

void PollBanner::themeChangedEvent()
{
    auto *theme = getApp()->getThemes();
    const auto bg = theme->tabs.regular.backgrounds.regular;
    this->setStyleSheet(
        QStringLiteral("background: rgba(%1,%2,%3,0.15);")
            .arg(bg.red())
            .arg(bg.green())
            .arg(bg.blue()));
}

}  // namespace chatterino
