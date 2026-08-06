// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"
#include "providers/youtube/YouTubeChannel.hpp"
#include "util/QStringHash.hpp"

#include <boost/unordered/unordered_flat_map.hpp>

#include <memory>

namespace chatterino {

/// Manages YouTubeChannel instances keyed by video ID.
class YouTubeChatServer
{
public:
    YouTubeChatServer() = default;
    ~YouTubeChatServer() = default;

    YouTubeChatServer(const YouTubeChatServer &) = delete;
    YouTubeChatServer(YouTubeChatServer &&) = delete;
    YouTubeChatServer &operator=(const YouTubeChatServer &) = delete;
    YouTubeChatServer &operator=(YouTubeChatServer &&) = delete;

    /// Returns an existing channel for videoId or creates a new one.
    std::shared_ptr<Channel> getOrCreate(const QString &videoId);

    /// Looks up an already-tracked channel by name, without creating one.
    /// Returns nullptr if not found. Used to resolve the real source
    /// channel for a message shown inside a MultiChannel view (whose own
    /// "channel" is the MultiChannel wrapper, not this one).
    std::shared_ptr<YouTubeChannel> find(const QString &videoId) const;

private:
    boost::unordered_flat_map<QString, std::weak_ptr<YouTubeChannel>>
        channelsByVideoId_;
};

}  // namespace chatterino
