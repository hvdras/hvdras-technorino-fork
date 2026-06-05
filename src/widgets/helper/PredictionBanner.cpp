// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/helper/PredictionBanner.hpp"

#include "Application.hpp"
#include "singletons/Theme.hpp"

#include <QStringBuilder>

namespace chatterino {

PredictionBanner::PredictionBanner(QWidget *parent)
    : BaseWidget(parent)
{
    this->layout_ = new QVBoxLayout(this);
    this->layout_->setContentsMargins(8, 4, 8, 4);
    this->layout_->setSpacing(2);

    this->titleLabel_ = new QLabel(this);
    this->titleLabel_->setWordWrap(true);
    this->titleLabel_->setStyleSheet("font-weight: bold;");
    this->layout_->addWidget(this->titleLabel_);

    this->outcomesLabel_ = new QLabel(this);
    this->outcomesLabel_->setWordWrap(true);
    this->layout_->addWidget(this->outcomesLabel_);

    this->statusLabel_ = new QLabel(this);
    this->statusLabel_->setAlignment(Qt::AlignRight);
    this->layout_->addWidget(this->statusLabel_);

    this->setVisible(false);
}

void PredictionBanner::setPrediction(
    const std::optional<TwitchChannel::PredictionEvent> &pred)
{
    this->current_ = pred;
    this->rebuild();
}

bool PredictionBanner::hasPrediction() const
{
    return this->current_.has_value();
}

void PredictionBanner::rebuild()
{
    if (!this->current_.has_value())
    {
        this->setVisible(false);
        return;
    }

    const auto &pred = *this->current_;

    // Title
    this->titleLabel_->setText(
        QStringLiteral("Prediction: ") + pred.title);

    // Outcomes
    QStringList parts;
    qlonglong totalPoints = 0;
    for (const auto &o : pred.outcomes)
    {
        totalPoints += o.channelPoints;
    }
    for (const auto &o : pred.outcomes)
    {
        const int pct = totalPoints > 0
                            ? int(o.channelPoints * 100 / totalPoints)
                            : 0;
        const bool isWinner =
            !pred.winningOutcomeId.isEmpty() && o.id == pred.winningOutcomeId;
        QString entry = o.title % QStringLiteral(" — ") %
                        QString::number(o.channelPoints) %
                        QStringLiteral(" pts (") % QString::number(pct) %
                        QStringLiteral("%)");
        if (isWinner)
        {
            entry = QStringLiteral("✓ ") + entry;
        }
        parts.append(entry);
    }
    this->outcomesLabel_->setText(parts.join(QStringLiteral("   ·   ")));

    // Status
    QString statusText = pred.status;
    if (pred.status == QStringLiteral("ACTIVE"))
    {
        statusText = QStringLiteral("Active");
    }
    else if (pred.status == QStringLiteral("LOCKED"))
    {
        statusText = QStringLiteral("Locked — awaiting resolution");
    }
    else if (pred.status == QStringLiteral("RESOLVED"))
    {
        statusText = QStringLiteral("Resolved");
    }
    this->statusLabel_->setText(statusText);

    this->setVisible(true);
}

void PredictionBanner::themeChangedEvent()
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
