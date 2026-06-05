#pragma once

#include "providers/twitch/TwitchChannel.hpp"
#include "widgets/BaseWidget.hpp"

#include <QLabel>
#include <QVBoxLayout>

namespace chatterino {

class PollBanner : public BaseWidget
{
    Q_OBJECT

public:
    explicit PollBanner(QWidget *parent = nullptr);

    void setPoll(const std::optional<TwitchChannel::PollEvent> &poll);
    bool hasPoll() const;

protected:
    void themeChangedEvent() override;

private:
    void rebuild();

    QVBoxLayout *layout_{};
    QLabel *titleLabel_{};
    QLabel *choicesLabel_{};
    QLabel *statusLabel_{};

    std::optional<TwitchChannel::PollEvent> current_;
};

}  // namespace chatterino
