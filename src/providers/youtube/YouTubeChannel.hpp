// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"
#include "util/Expected.hpp"

#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QDateTime>
#include <QHash>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

namespace chatterino {

/// Read-only YouTube live chat channel.
/// Polls the YouTube internal live chat API without requiring an API key.
/// Open via channel type "YouTube" with channelName = video ID.
class YouTubeChannel final : public Channel
{
public:
    explicit YouTubeChannel(const QString &videoId);
    ~YouTubeChannel() override;

    // Must be called once, immediately after the shared_ptr is created.
    void initialize();

    std::shared_ptr<YouTubeChannel> sharedFromThis();
    std::weak_ptr<YouTubeChannel> weakFromThis();

    const QString &videoId() const;

    /// The stream's title, if known. Populated once when the watch/live page
    /// is first fetched; not kept up to date afterwards.
    const QString &title() const;
    /// URL of the stream's thumbnail image, if known. Same freshness caveat
    /// as title().
    const QString &thumbnailUrl() const;

    bool canSendMessage() const override;
    bool isLive() const override;
    bool canReconnect() const override;
    void reconnect() override;
    bool hasModRights() const override;

    /// Deletes a message via the official YouTube Data API v3, using the
    /// currently logged-in YouTubeAccount's OAuth token. Requires that
    /// account to actually be a moderator/owner of this chat - otherwise
    /// the request fails and a system message is posted with the error.
    ///
    /// `messageId` is the message's own ID, as seen from the unofficial live
    /// chat feed this channel reads from - it isn't a valid ID for the
    /// official API, so the message is looked up there by
    /// author/timestamp/text first (see YouTubeApi::findMessageId).
    /// `messageId` is still used to hide the message locally immediately
    /// after a successful delete, rather than waiting for the next live
    /// chat poll to notice YouTube's own removal event.
    void deleteMessage(const QString &messageId, const QString &authorChannelId,
                       const QDateTime &timestamp, const QString &messageText);

    /// Resolves (and caches) the active live chat ID for the current
    /// broadcast. Used by every moderation action (delete/ban/timeout),
    /// which all need it but shouldn't each re-resolve it individually.
    void resolveLiveChatId(std::function<void(ExpectedStr<QString>)> cb);

    /// Remembers the ban resource ID YouTube returned for a given target
    /// channel, so a later unban can reference it - the official API can
    /// only unban by ban resource ID, not by channel ID, and has no way to
    /// look one up after the fact.
    void recordBanId(const QString &targetChannelId, const QString &banId);
    /// Returns and forgets the most recent ban ID recorded for a channel in
    /// this session, if any.
    std::optional<QString> takeBanId(const QString &targetChannelId);

    /// Fired whenever isLive() changes, so the tab's live indicator updates.
    pajlada::Signals::NoArgSignal liveStatusChanged;

private:
    void fetchChannelLivePage(const QString &handle);
    void fetchWatchPage();
    void fetchLiveChat(const QString &continuation);
    void scheduleNextPoll(const QString &continuation, int timeoutMs);
    void setLive(bool live);
    /// Called when the chat/stream we were watching ends (or a handle
    /// lookup finds nobody currently live). Schedules another attempt to
    /// find a live stream instead of giving up permanently.
    void scheduleRediscovery();

    QString videoId_;
    // The owning channel's path (e.g. "@somechannel" or "channel/UCxxxx").
    // Set immediately if this channel was opened via a handle; otherwise
    // learned from the video's watch page once it's fetched. Used to
    // re-search for a new live stream on the same channel after one ends,
    // instead of only ever re-checking a single dead video.
    QString handle_;
    QString apiKey_;
    QString title_;
    QString thumbnailUrl_;
    bool live_ = false;
    // True once the first fetchLiveChat() poll of a connection has been
    // displayed. That first poll is a catch-up batch of messages that
    // already happened, not new arrivals, so it's shown immediately rather
    // than staggered like later polls. Reset whenever a new connection to a
    // live chat starts.
    bool receivedFirstBatch_ = false;
    // Cached activeLiveChatId for the current videoId_, resolved lazily on
    // first moderation action. Cleared whenever videoId_ changes (i.e. a new
    // connection/broadcast is picked up).
    QString liveChatId_;
    // Ban resource IDs recorded from successful ban/timeout calls, keyed by
    // target channel ID, so unban can find them again. In-memory only and
    // scoped to the current liveChatId_ - cleared alongside it.
    QHash<QString, QString> banIdsByChannelId_;

    pajlada::Signals::SignalHolder signalHolder_;
};

}  // namespace chatterino
