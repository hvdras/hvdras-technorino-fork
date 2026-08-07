// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/youtube/YouTubeApi.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"

#include <QUrl>

#include <memory>

namespace chatterino {

using namespace Qt::Literals;

YouTubeApi *YouTubeApi::instance()
{
    static std::unique_ptr<YouTubeApi> api{new YouTubeApi};
    return api.get();
}

void YouTubeApi::deleteMessage(const QString &messageId, Callback cb)
{
    QString url =
        u"https://www.googleapis.com/youtube/v3/liveChatMessages?id="_s %
        QString::fromUtf8(QUrl::toPercentEncoding(messageId));

    NetworkRequest(url, NetworkRequestType::Delete)
        .header("Authorization"_ba, "Bearer "_ba + this->authToken_)
        .onError([cb](const NetworkResult &res) {
            auto message = res.parseJson()["error"_L1]
                              .toObject()["message"_L1]
                              .toString();
            if (!message.isEmpty())
            {
                cb(makeUnexpected(message));
            }
            else
            {
                cb(makeUnexpected(res.formatError()));
            }
        })
        .onSuccess([cb](const NetworkResult & /*res*/) {
            cb(ExpectedStr<void>{});
        })
        .execute();
}

void YouTubeApi::setAuth(const QString &authToken)
{
    this->authToken_ = authToken.toUtf8();
}

YouTubeApi *getYouTubeApi()
{
    return YouTubeApi::instance();
}

}  // namespace chatterino
