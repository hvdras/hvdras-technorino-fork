// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/youtube/YouTubeChannel.hpp"

#include "common/enums/MessageContext.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"

#include <QColor>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

using namespace Qt::Literals;
using namespace std::chrono_literals;

namespace chatterino {

namespace {

Q_LOGGING_CATEGORY(chatterinoYoutube, "chatterino.youtube")

// YouTube red for author names
const QColor YOUTUBE_RED{0xFF, 0x00, 0x00};

// Minimum/maximum poll intervals (ms)
constexpr int MIN_POLL_MS = 3000;
constexpr int MAX_POLL_MS = 15000;
constexpr int DEFAULT_POLL_MS = 5000;
constexpr int ERROR_RETRY_MS = 10000;

// Headers that make requests look like a real browser
const char *USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/124.0.0.0 Safari/537.36";

/// Recursively search obj for the first string value at key.
QString findKey(const QJsonObject &obj, const QString &key)
{
    for (auto it = obj.begin(); it != obj.end(); ++it)
    {
        if (it.key() == key && it.value().isString())
        {
            return it.value().toString();
        }
        if (it.value().isObject())
        {
            auto found = findKey(it.value().toObject(), key);
            if (!found.isEmpty())
            {
                return found;
            }
        }
        if (it.value().isArray())
        {
            for (const auto &elem : it.value().toArray())
            {
                if (elem.isObject())
                {
                    auto found = findKey(elem.toObject(), key);
                    if (!found.isEmpty())
                    {
                        return found;
                    }
                }
            }
        }
    }
    return {};
}

/// Extract the live chat continuation token from ytInitialData.
/// Looks for reloadContinuationData.continuation under liveChatRenderer.
QString extractInitialContinuation(const QJsonObject &root)
{
    // Known path: contents.twoColumnWatchNextResults.conversationBar
    //             .liveChatRenderer.continuations[0].reloadContinuationData.continuation
    auto contents = root["contents"].toObject();
    auto two = contents["twoColumnWatchNextResults"].toObject();
    auto bar = two["conversationBar"].toObject();
    auto lcr = bar["liveChatRenderer"].toObject();
    if (lcr.isEmpty())
    {
        // Fallback: search recursively for reloadContinuationData
        return findKey(root, "reloadContinuationData");
    }

    auto continuations = lcr["continuations"].toArray();
    if (continuations.isEmpty())
    {
        return {};
    }

    // Try reloadContinuationData first, then timedContinuationData
    auto first = continuations[0].toObject();
    {
        auto rcd = first["reloadContinuationData"].toObject();
        if (!rcd.isEmpty())
        {
            return rcd["continuation"].toString();
        }
    }
    {
        auto tcd = first["timedContinuationData"].toObject();
        if (!tcd.isEmpty())
        {
            return tcd["continuation"].toString();
        }
    }
    return {};
}

/// Extract the Innertube API key embedded in the page HTML.
QString extractApiKey(const QByteArray &body)
{
    static const QByteArray MARKER = "\"INNERTUBE_API_KEY\":\"";
    auto idx = body.indexOf(MARKER);
    if (idx == -1)
    {
        return {};
    }
    idx += static_cast<int>(MARKER.size());
    const auto end = body.indexOf('"', idx);
    if (end == -1)
    {
        return {};
    }
    return QString::fromUtf8(body.mid(idx, end - idx));
}

/// Parse ytInitialData JSON from page HTML.
/// Returns the JSON document, or null if not found / parse failed.
QJsonDocument extractYtInitialData(const QByteArray &body)
{
    static const QByteArray MARKER1 = "var ytInitialData = ";
    static const QByteArray MARKER2 = "ytInitialData = ";

    int idx = body.indexOf(MARKER1);
    if (idx != -1)
    {
        idx += static_cast<int>(MARKER1.size());
    }
    else
    {
        idx = body.indexOf(MARKER2);
        if (idx == -1)
        {
            return {};
        }
        idx += static_cast<int>(MARKER2.size());
    }

    auto endIdx = body.indexOf(";</script>", idx);
    if (endIdx == -1)
    {
        endIdx = body.indexOf(';', idx);
    }
    if (endIdx == -1)
    {
        return {};
    }

    return QJsonDocument::fromJson(body.mid(idx, endIdx - idx));
}

/// Extract text from a YouTube "runs" array (list of text/emoji run objects).
QString runsToText(const QJsonArray &runs)
{
    QString result;
    for (const auto &run : runs)
    {
        auto obj = run.toObject();
        if (obj.contains("text"_L1))
        {
            result += obj["text"].toString();
        }
        else if (obj.contains("emoji"_L1))
        {
            // Replace emoji with its shortcode if available
            auto emoji = obj["emoji"].toObject();
            auto shortcuts = emoji["shortcuts"].toArray();
            if (!shortcuts.isEmpty())
            {
                result += shortcuts[0].toString();
            }
            else
            {
                // Use the emojiId as fallback
                result += emoji["emojiId"].toString();
            }
        }
    }
    return result;
}

}  // namespace

namespace {

// A YouTube video ID is exactly 11 characters from [A-Za-z0-9_-].
bool looksLikeVideoId(const QString &s)
{
    if (s.size() != 11)
    {
        return false;
    }
    for (const QChar ch : s)
    {
        if (!ch.isLetterOrNumber() && ch != u'_' && ch != u'-')
        {
            return false;
        }
    }
    return true;
}

}  // namespace

YouTubeChannel::YouTubeChannel(const QString &nameOrHandle)
    : Channel(nameOrHandle, Type::YouTube)
    , videoId_(nameOrHandle)
{
    if (looksLikeVideoId(nameOrHandle))
    {
        this->addSystemMessage(
            u"YouTube: Connecting to live chat for video %1..."_s.arg(
                nameOrHandle));
        this->fetchWatchPage();
    }
    else
    {
        // Treat as channel handle — ensure it has the @ prefix
        const auto handle = nameOrHandle.startsWith(u'@')
                                ? nameOrHandle
                                : u'@' + nameOrHandle;
        this->addSystemMessage(
            u"YouTube: Looking up live stream for %1..."_s.arg(handle));
        this->fetchChannelLivePage(handle);
    }
}

YouTubeChannel::~YouTubeChannel() = default;

const QString &YouTubeChannel::videoId() const
{
    return this->videoId_;
}

bool YouTubeChannel::canSendMessage() const
{
    return false;
}

bool YouTubeChannel::isLive() const
{
    return this->live_;
}

void YouTubeChannel::fetchChannelLivePage(const QString &handle)
{
    auto weak = this->weak_from_this();

    NetworkRequest(u"https://www.youtube.com/%1/live"_s.arg(handle))
        .header("User-Agent", USER_AGENT)
        .header("Accept-Language", "en-US,en;q=0.9")
        .followRedirects(true)
        .onSuccess([weak](const NetworkResult &result) {
            auto self =
                std::static_pointer_cast<YouTubeChannel>(weak.lock());
            if (!self)
            {
                return;
            }

            const auto &body = result.getData();

            // Extract video ID from the live page
            static const QByteArray VIDEO_ID_MARKER = "\"videoId\":\"";
            auto vidIdx = body.indexOf(VIDEO_ID_MARKER);
            if (vidIdx != -1)
            {
                vidIdx += static_cast<int>(VIDEO_ID_MARKER.size());
                auto vidEnd = body.indexOf('"', vidIdx);
                if (vidEnd != -1 && vidEnd - vidIdx == 11)
                {
                    self->videoId_ = QString::fromUtf8(body.mid(vidIdx, 11));
                }
            }

            self->apiKey_ = extractApiKey(body);

            const auto doc = extractYtInitialData(body);
            if (doc.isNull())
            {
                self->addSystemMessage(
                    u"YouTube: Channel is not live."_s);
                return;
            }

            const auto continuation =
                extractInitialContinuation(doc.object());
            if (continuation.isEmpty())
            {
                self->addSystemMessage(
                    u"YouTube: Channel is not currently live."_s);
                return;
            }

            self->live_ = true;
            self->fetchLiveChat(continuation);
        })
        .onError([weak](const NetworkResult &result) {
            auto self =
                std::static_pointer_cast<YouTubeChannel>(weak.lock());
            if (!self)
            {
                return;
            }
            qCWarning(chatterinoYoutube)
                << "Failed to fetch channel live page:"
                << result.formatError();
            self->addSystemMessage(
                u"YouTube: Failed to load channel (%1)."_s.arg(
                    result.formatError()));
        })
        .execute();
}

void YouTubeChannel::fetchWatchPage()
{
    auto weak = this->weak_from_this();

    NetworkRequest(u"https://www.youtube.com/watch?v=%1"_s.arg(this->videoId_))
        .header("User-Agent", USER_AGENT)
        .header("Accept-Language", "en-US,en;q=0.9")
        .followRedirects(true)
        .onSuccess([weak](const NetworkResult &result) {
            auto self =
                std::static_pointer_cast<YouTubeChannel>(weak.lock());
            if (!self)
            {
                return;
            }

            const auto &body = result.getData();

            self->apiKey_ = extractApiKey(body);

            const auto doc = extractYtInitialData(body);
            if (doc.isNull())
            {
                self->addSystemMessage(
                    u"YouTube: No live chat data found. Is this an active live stream?"_s);
                return;
            }

            const auto continuation =
                extractInitialContinuation(doc.object());
            if (continuation.isEmpty())
            {
                self->addSystemMessage(
                    u"YouTube: No live chat available. The stream may not be live."_s);
                return;
            }

            self->live_ = true;
            self->fetchLiveChat(continuation);
        })
        .onError([weak](const NetworkResult &result) {
            auto self =
                std::static_pointer_cast<YouTubeChannel>(weak.lock());
            if (!self)
            {
                return;
            }
            qCWarning(chatterinoYoutube)
                << "Failed to fetch YouTube watch page:"
                << result.formatError();
            self->addSystemMessage(
                u"YouTube: Failed to load page (%1). Retrying..."_s.arg(
                    result.formatError()));
            QTimer::singleShot(ERROR_RETRY_MS, [weak] {
                auto self =
                    std::static_pointer_cast<YouTubeChannel>(weak.lock());
                if (self)
                {
                    self->fetchWatchPage();
                }
            });
        })
        .execute();
}

void YouTubeChannel::fetchLiveChat(const QString &continuation)
{
    auto weak = this->weak_from_this();

    const QJsonObject requestBody{
        {"context",
         QJsonObject{{"client",
                      QJsonObject{
                          {"clientName", "WEB"},
                          {"clientVersion", "2.20240101.00.00"},
                          {"hl", "en"},
                          {"gl", "US"},
                      }}}},
        {"continuation", continuation},
    };

    // YouTube requires the API key as a query parameter to return JSON.
    // The key is extracted from the watch page; fall back to the known
    // public key if extraction failed.
    const auto key = this->apiKey_.isEmpty()
                         ? u"AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8"_s
                         : this->apiKey_;

    const auto url =
        u"https://www.youtube.com/youtubei/v1/live_chat/get_live_chat?key=%1"_s
            .arg(key);
    const auto referer =
        u"https://www.youtube.com/watch?v=%1"_s.arg(this->videoId_);

    NetworkRequest(url, NetworkRequestType::Post)
        .json(requestBody)
        .header("User-Agent", USER_AGENT)
        .header("Accept-Language", "en-US,en;q=0.9")
        .header("Origin", "https://www.youtube.com")
        .header("Referer", referer)
        .header("X-YouTube-Client-Name", "1")
        .header("X-YouTube-Client-Version", "2.20240101.00.00")
        .onSuccess([weak](const NetworkResult &result) {
            auto self =
                std::static_pointer_cast<YouTubeChannel>(weak.lock());
            if (!self)
            {
                return;
            }

            const auto root = result.parseJson();
            if (root.isEmpty())
            {
                qCWarning(chatterinoYoutube)
                    << "Empty/invalid JSON from live chat API (status"
                    << result.status().value_or(0) << ")";
                self->addSystemMessage(
                    u"YouTube: Chat API returned an invalid response. Retrying..."_s);
                QTimer::singleShot(ERROR_RETRY_MS, [weak] {
                    auto self =
                        std::static_pointer_cast<YouTubeChannel>(weak.lock());
                    if (self)
                    {
                        self->fetchWatchPage();
                    }
                });
                return;
            }

            auto cc =
                root["continuationContents"].toObject()["liveChatContinuation"]
                    .toObject();
            if (cc.isEmpty())
            {
                qCWarning(chatterinoYoutube)
                    << "No liveChatContinuation in response";
                // Stream may have ended
                self->live_ = false;
                self->addSystemMessage(
                    u"YouTube: Live chat ended."_s);
                return;
            }

            // Parse next continuation & timeout
            QString nextContinuation;
            int timeoutMs = DEFAULT_POLL_MS;

            const auto continuations = cc["continuations"].toArray();
            if (!continuations.isEmpty())
            {
                auto contObj = continuations[0].toObject();
                // Try invalidationContinuationData → continuation
                for (const auto &key :
                     {"invalidationContinuationData",
                      "timedContinuationData", "liveChatReplayContinuationData",
                      "reloadContinuationData"})
                {
                    auto inner = contObj[key].toObject();
                    if (!inner.isEmpty())
                    {
                        nextContinuation = inner["continuation"].toString();
                        if (inner.contains("timeoutMs"_L1))
                        {
                            timeoutMs =
                                std::clamp(inner["timeoutMs"].toInt(),
                                           MIN_POLL_MS, MAX_POLL_MS);
                        }
                        break;
                    }
                }
            }

            // Parse chat messages
            const auto actions = cc["actions"].toArray();
            for (const auto &actionVal : actions)
            {
                auto action = actionVal.toObject();
                auto addItem =
                    action["addChatItemAction"].toObject()["item"].toObject();

                // Only handle regular text messages
                auto renderer =
                    addItem["liveChatTextMessageRenderer"].toObject();
                if (renderer.isEmpty())
                {
                    continue;
                }

                const QString authorName =
                    renderer["authorName"].toObject()["simpleText"].toString();
                const QString messageText =
                    runsToText(renderer["message"].toObject()["runs"].toArray());
                const qint64 timestampUsec =
                    renderer["timestampUsec"].toString().toLongLong();

                if (authorName.isEmpty() || messageText.isEmpty())
                {
                    continue;
                }

                MessageBuilder builder;
                builder->channelName = self->getName();

                if (timestampUsec > 0)
                {
                    builder->serverReceivedTime =
                        QDateTime::fromMSecsSinceEpoch(timestampUsec / 1000);
                }

                builder.emplace<TimestampElement>(
                    builder->serverReceivedTime.isValid()
                        ? builder->serverReceivedTime.toLocalTime().time()
                        : QTime::currentTime());

                builder.emplace<TextElement>(
                    authorName + ':',
                    MessageElementFlags{MessageElementFlag::Misc},
                    MessageColor{YOUTUBE_RED},
                    FontStyle::ChatMediumBold);

                builder.emplace<TextElement>(
                    messageText, MessageElementFlags{MessageElementFlag::Text},
                    MessageColor::Text);

                self->addMessage(builder.release(), MessageContext::Original);
            }

            if (nextContinuation.isEmpty())
            {
                self->live_ = false;
                self->addSystemMessage(u"YouTube: Live chat ended."_s);
                return;
            }

            self->scheduleNextPoll(nextContinuation, timeoutMs);
        })
        .onError([weak](const NetworkResult &result) {
            auto self =
                std::static_pointer_cast<YouTubeChannel>(weak.lock());
            if (!self)
            {
                return;
            }
            qCWarning(chatterinoYoutube)
                << "Live chat request failed:" << result.formatError();
            // On error, re-fetch the watch page to get a fresh token
            QTimer::singleShot(ERROR_RETRY_MS, [weak] {
                auto self =
                    std::static_pointer_cast<YouTubeChannel>(weak.lock());
                if (self)
                {
                    self->fetchWatchPage();
                }
            });
        })
        .execute();
}

void YouTubeChannel::scheduleNextPoll(const QString &continuation,
                                      int timeoutMs)
{
    auto weak = this->weak_from_this();
    QTimer::singleShot(timeoutMs, [weak, continuation] {
        auto self = std::static_pointer_cast<YouTubeChannel>(weak.lock());
        if (self)
        {
            self->fetchLiveChat(continuation);
        }
    });
}

}  // namespace chatterino
