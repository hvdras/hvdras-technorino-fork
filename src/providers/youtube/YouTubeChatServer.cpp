// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/youtube/YouTubeChatServer.hpp"

#include "providers/youtube/YouTubeChannel.hpp"

namespace chatterino {

std::shared_ptr<Channel> YouTubeChatServer::getOrCreate(
    const QString &videoId)
{
    auto it = this->channelsByVideoId_.find(videoId);
    if (it != this->channelsByVideoId_.end())
    {
        if (auto existing = it->second.lock())
        {
            return existing;
        }
    }

    auto chan = std::make_shared<YouTubeChannel>(videoId);
    this->channelsByVideoId_[videoId] = chan;
    return chan;
}

}  // namespace chatterino
