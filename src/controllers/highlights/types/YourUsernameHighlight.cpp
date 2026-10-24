// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/highlights/types/YourUsernameHighlight.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/highlights/HighlightCheck.hpp"
#include "controllers/highlights/HighlightResult.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/twitch/TwitchAccount.hpp"  // IWYU pragma: keep
#include "providers/youtube/YouTubeAccount.hpp"

#include <QStringList>

namespace chatterino::highlights {

HighlightCheck YourUsernameHighlight::buildCheck() const
{
    using H = std::remove_pointer_t<decltype(this)>;
    using Params = HighlightCheckParams;

    auto twitchRe = [&] -> std::optional<QRegularExpression> {
        auto user = getApp()->getAccounts()->twitch.getCurrent();
        auto name = user->getUserName();
        if (user->isAnon() || name.isEmpty())
        {
            return std::nullopt;
        }

        return QRegularExpression{
            REGEX_START_BOUNDARY % QRegularExpression::escape(name) %
                REGEX_END_BOUNDARY,
            QRegularExpression::UseUnicodePropertiesOption |
                QRegularExpression::CaseInsensitiveOption,
        };
    }();
    auto kickRe = [&] -> std::optional<QRegularExpression> {
        auto user = getApp()->getAccounts()->kick.current();
        auto name = user->username();
        if (user->isAnonymous() || name.isEmpty())
        {
            return std::nullopt;
        }

        return QRegularExpression{
            REGEX_START_BOUNDARY % QRegularExpression::escape(name) %
                REGEX_END_BOUNDARY,
            QRegularExpression::UseUnicodePropertiesOption |
                QRegularExpression::CaseInsensitiveOption,
        };
    }();

    // People @-mention a YouTube channel by its handle (e.g. "hvdras"), which
    // can differ from its display name (e.g. "Hydra") - match either.
    auto youtubeRe = [&] -> std::optional<QRegularExpression> {
        auto user = getApp()->getAccounts()->youtube.current();
        if (user->isAnonymous())
        {
            return std::nullopt;
        }

        QStringList names;
        for (const auto &name : {user->handle(), user->channelName()})
        {
            if (!name.isEmpty())
            {
                names << QRegularExpression::escape(name);
            }
        }
        if (names.isEmpty())
        {
            return std::nullopt;
        }

        QString alternation = "(?:" + names.join('|') + ")";
        return QRegularExpression{
            REGEX_START_BOUNDARY % alternation % REGEX_END_BOUNDARY,
            QRegularExpression::UseUnicodePropertiesOption |
                QRegularExpression::CaseInsensitiveOption,
        };
    }();

    if (!twitchRe && !kickRe && !youtubeRe)
    {
        return {};
    }

    return {
        [highlight = *this, twitchRe, kickRe,
         youtubeRe](const Params &p) -> std::optional<HighlightResult> {
            if (p.self)
            {
                return std::nullopt;
            }

            switch (p.platform)
            {
                case MessagePlatform::AnyOrTwitch: {
                    if (!twitchRe ||
                        !twitchRe->match(p.originalMessage).hasMatch())
                    {
                        return std::nullopt;
                    }
                }
                break;
                case MessagePlatform::Kick: {
                    if (!kickRe || !kickRe->match(p.originalMessage).hasMatch())
                    {
                        return std::nullopt;
                    }
                }
                break;
                case MessagePlatform::YouTube: {
                    if (!youtubeRe ||
                        !youtubeRe->match(p.originalMessage).hasMatch())
                    {
                        return std::nullopt;
                    }
                }
                break;
            }

            return highlight.outcome.makeSimpleResult<H>();
        },
    };
}

QDebug operator<<(QDebug dbg, const YourUsernameHighlight &v)
{
    dbg.nospace() << "YourUsernameHighlight("        //
                  << "name:" << v.name               //
                  << ',' << "enabled:" << v.enabled  //
                  << ',' << "outcome:" << v.outcome  //
                  << ')';

    return dbg;
}

}  // namespace chatterino::highlights
