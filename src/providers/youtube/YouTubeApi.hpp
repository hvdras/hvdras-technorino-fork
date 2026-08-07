// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/Expected.hpp"

#include <QByteArray>
#include <QString>

#include <functional>

namespace chatterino {

/// A minimal client for the write endpoints of the official YouTube Data
/// API v3 that this app needs for moderation. Unlike YouTubeChannel (which
/// reads live chat anonymously via the unofficial Innertube API), these
/// calls require an authenticated YouTubeAccount's OAuth token.
class YouTubeApi
{
public:
    using Callback = std::function<void(ExpectedStr<void>)>;

    static YouTubeApi *instance();

    void deleteMessage(const QString &messageId, Callback cb);

    void setAuth(const QString &authToken);

private:
    YouTubeApi() = default;

    QByteArray authToken_;
};

YouTubeApi *getYouTubeApi();

}  // namespace chatterino
