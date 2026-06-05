// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/commands/builtin/twitch/BlockedTerms.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "util/PostToThread.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrlQuery>

namespace chatterino::commands {

namespace {

QString phraseFromContext(const CommandContext &ctx)
{
    if (ctx.words.size() < 2)
    {
        return {};
    }
    return ctx.words.mid(1).join(QLatin1Char(' ')).trimmed();
}

}  // namespace

QString blockTerm(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "/blockterm only works in Twitch channels.");
        return "";
    }

    const auto phrase = phraseFromContext(ctx);
    if (phrase.isEmpty())
    {
        ctx.channel->addSystemMessage(
            "Usage: /blockterm <phrase or word>");
        return "";
    }

    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!account || account->isAnon())
    {
        ctx.channel->addSystemMessage(
            "You must be logged in to manage blocked terms.");
        return "";
    }

    const auto broadcasterID = ctx.twitchChannel->roomId();
    const auto moderatorID = account->getUserId();
    const auto token = account->getOAuthToken();
    const auto clientID = account->getOAuthClient();

    QJsonObject body;
    body["text"] = phrase;

    QUrl url("https://api.twitch.tv/helix/moderation/blocked_terms");
    QUrlQuery query;
    query.addQueryItem("broadcaster_id", broadcasterID);
    query.addQueryItem("moderator_id", moderatorID);
    url.setQuery(query);

    NetworkRequest(url, NetworkRequestType::Post)
        .timeout(10000)
        .header("Client-ID", clientID)
        .header("Authorization", "Bearer " + token)
        .header("Content-Type", "application/json")
        .payload(QJsonDocument(body).toJson(QJsonDocument::Compact))
        .onSuccess([channel{ctx.channel}, phrase](const NetworkResult &result) {
            runInGuiThread([channel, phrase] {
                if (channel)
                {
                    channel->addSystemMessage(
                        QStringLiteral("Blocked term added: \"%1\"")
                            .arg(phrase));
                }
            });
        })
        .onError([channel{ctx.channel}](const NetworkResult &result) {
            runInGuiThread([channel, error = result.formatError()] {
                if (channel)
                {
                    channel->addSystemMessage(
                        QStringLiteral("Failed to add blocked term: %1")
                            .arg(error));
                }
            });
        })
        .execute();

    return "";
}

QString unblockTerm(const CommandContext &ctx)
{
    if (ctx.channel == nullptr)
    {
        return "";
    }

    if (ctx.twitchChannel == nullptr)
    {
        ctx.channel->addSystemMessage(
            "/unblockterm only works in Twitch channels.");
        return "";
    }

    const auto phrase = phraseFromContext(ctx);
    if (phrase.isEmpty())
    {
        ctx.channel->addSystemMessage(
            "Usage: /unblockterm <phrase or word>");
        return "";
    }

    auto account = getApp()->getAccounts()->twitch.getCurrent();
    if (!account || account->isAnon())
    {
        ctx.channel->addSystemMessage(
            "You must be logged in to manage blocked terms.");
        return "";
    }

    const auto broadcasterID = ctx.twitchChannel->roomId();
    const auto moderatorID = account->getUserId();
    const auto token = account->getOAuthToken();
    const auto clientID = account->getOAuthClient();

    // First fetch all blocked terms to find the ID matching phrase
    QUrl listUrl("https://api.twitch.tv/helix/moderation/blocked_terms");
    QUrlQuery listQuery;
    listQuery.addQueryItem("broadcaster_id", broadcasterID);
    listQuery.addQueryItem("moderator_id", moderatorID);
    listQuery.addQueryItem("first", "100");
    listUrl.setQuery(listQuery);

    NetworkRequest(listUrl, NetworkRequestType::Get)
        .timeout(10000)
        .header("Client-ID", clientID)
        .header("Authorization", "Bearer " + token)
        .onSuccess([channel{ctx.channel}, phrase, broadcasterID, moderatorID,
                    token, clientID](const NetworkResult &result) {
            const auto root = result.parseJson();
            const auto data = root.value("data").toArray();

            QString termID;
            const auto needle = phrase.trimmed().toLower();
            for (const auto &val : data)
            {
                const auto obj = val.toObject();
                if (obj.value("text").toString().trimmed().toLower() == needle)
                {
                    termID = obj.value("id").toString();
                    break;
                }
            }

            if (termID.isEmpty())
            {
                runInGuiThread([channel, phrase] {
                    if (channel)
                    {
                        channel->addSystemMessage(
                            QStringLiteral(
                                "No blocked term found matching \"%1\"")
                                .arg(phrase));
                    }
                });
                return;
            }

            QUrl deleteUrl(
                "https://api.twitch.tv/helix/moderation/blocked_terms");
            QUrlQuery deleteQuery;
            deleteQuery.addQueryItem("broadcaster_id", broadcasterID);
            deleteQuery.addQueryItem("moderator_id", moderatorID);
            deleteQuery.addQueryItem("id", termID);
            deleteUrl.setQuery(deleteQuery);

            NetworkRequest(deleteUrl, NetworkRequestType::Delete)
                .timeout(10000)
                .header("Client-ID", clientID)
                .header("Authorization", "Bearer " + token)
                .onSuccess([channel, phrase](const NetworkResult &) {
                    runInGuiThread([channel, phrase] {
                        if (channel)
                        {
                            channel->addSystemMessage(
                                QStringLiteral("Blocked term removed: \"%1\"")
                                    .arg(phrase));
                        }
                    });
                })
                .onError([channel](const NetworkResult &result) {
                    runInGuiThread([channel, error = result.formatError()] {
                        if (channel)
                        {
                            channel->addSystemMessage(
                                QStringLiteral(
                                    "Failed to remove blocked term: %1")
                                    .arg(error));
                        }
                    });
                })
                .execute();
        })
        .onError([channel{ctx.channel}](const NetworkResult &result) {
            runInGuiThread([channel, error = result.formatError()] {
                if (channel)
                {
                    channel->addSystemMessage(
                        QStringLiteral("Failed to fetch blocked terms: %1")
                            .arg(error));
                }
            });
        })
        .execute();

    return "";
}

}  // namespace chatterino::commands
