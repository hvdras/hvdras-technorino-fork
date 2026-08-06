// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/youtube/YouTubeChatServer.hpp"

#include "providers/youtube/YouTubeChannel.hpp"

namespace chatterino {

std::shared_ptr<YouTubeChannel> YouTubeChatServer::find(
    const QString &videoId) const
{
    auto it = this->channelsByVideoId_.find(videoId);
    if (it != this->channelsByVideoId_.end())
    {
        return it->second.lock();
    }
    return nullptr;
}

std::shared_ptr<Channel> YouTubeChatServer::getOrCreate(
    const QString &videoId)
{
    if (auto existing = this->find(videoId))
    {
        return existing;
    }

    auto chan = std::make_shared<YouTubeChannel>(videoId);
    // initialize() must be called after shared_ptr construction so that
    // weak_from_this() is valid inside the network request callbacks.
    chan->initialize();
    this->channelsByVideoId_[videoId] = chan;
    return chan;
}

}  // namespace chatterino
