// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"

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

    bool canSendMessage() const override;
    bool isLive() const override;

private:
    void fetchChannelLivePage(const QString &handle);
    void fetchWatchPage();
    void fetchLiveChat(const QString &continuation);
    void scheduleNextPoll(const QString &continuation, int timeoutMs);

    QString videoId_;
    QString apiKey_;
    bool live_ = false;

    pajlada::Signals::SignalHolder signalHolder_;
};

}  // namespace chatterino
