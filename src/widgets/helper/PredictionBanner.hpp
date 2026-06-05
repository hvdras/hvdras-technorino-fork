#pragma once

#include "providers/twitch/TwitchChannel.hpp"
#include "widgets/BaseWidget.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QLabel>
#include <QVBoxLayout>

namespace chatterino {

class PredictionBanner : public BaseWidget
{
    Q_OBJECT

public:
    explicit PredictionBanner(QWidget *parent = nullptr);

    void setPrediction(const std::optional<TwitchChannel::PredictionEvent> &pred);
    bool hasPrediction() const;

protected:
    void themeChangedEvent() override;

private:
    void rebuild();

    QVBoxLayout *layout_{};
    QLabel *titleLabel_{};
    QLabel *outcomesLabel_{};
    QLabel *statusLabel_{};

    std::optional<TwitchChannel::PredictionEvent> current_;
};

}  // namespace chatterino
