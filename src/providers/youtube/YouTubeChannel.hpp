// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"

#include <pajlada/signals/signal.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QString>

#include <memory>

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
    // Non-empty if this channel was opened via a channel handle (e.g.
    // "@somechannel") rather than a fixed video ID. Used to re-search for a
    // new live stream after one ends.
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

    pajlada::Signals::SignalHolder signalHolder_;
};

}  // namespace chatterino
