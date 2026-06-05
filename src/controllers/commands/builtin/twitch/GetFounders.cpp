// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/commands/builtin/twitch/GetFounders.hpp"

#include "controllers/commands/CommandContext.hpp"
#include "providers/IvrApi.hpp"
#include "providers/twitch/TwitchChannel.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

namespace {

using namespace chatterino;

QString targetChannelLogin(const CommandContext &ctx)
{
    if (ctx.words.size() > 1)
    {
        auto login = ctx.words.at(1).trimmed();
        if (login.startsWith('#'))
        {
            login.remove(0, 1);
        }
        return login.toLower();
    }

    if (ctx.twitchChannel != nullptr)
    {
        return ctx.twitchChannel->getName().toLower();
    }

    return {};
}

}  // namespace

namespace chatterino::commands {

QString getFounders(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    const auto channelLogin = targetChannelLogin(ctx);
    if (channelLogin.isEmpty())
    {
        ctx.channel->addSystemMessage(
            "The /founders command only works in Twitch channels.");
        return "";
    }

    getIvr()->getFounders(
        channelLogin,
        [channel{ctx.channel}](const QJsonArray &founders) {
            if (founders.isEmpty())
            {
                channel->addSystemMessage(
                    "This channel does not have any founders.");
                return;
            }

            QStringList names;
            for (const auto &val : founders)
            {
                const auto obj = val.toObject();
                auto name = obj.value("displayName").toString();
                if (name.isEmpty())
                {
                    name = obj.value("login").toString();
                }
                if (!name.isEmpty())
                {
                    names.append(name);
                }
            }

            if (names.isEmpty())
            {
                channel->addSystemMessage(
                    "This channel does not have any founders.");
                return;
            }

            channel->addSystemMessage(
                QStringLiteral("The founders of this channel are: %1")
                    .arg(names.join(QStringLiteral(", "))));
        },
        [channel{ctx.channel}] {
            channel->addSystemMessage("Could not get founders list!");
        });

    return "";
}

}  // namespace chatterino::commands
