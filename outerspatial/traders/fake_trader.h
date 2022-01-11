//
// Created by henry on 14/12/2021.
//

#ifndef CPPBAZAARBOT_FAKE_TRADER_H
#define CPPBAZAARBOT_FAKE_TRADER_H

#include <utility>

#include "../common/messages.h"

#include "../auction/auction_house.h"
#include "../metrics/logger.h"


struct OngoingShortage {
    OngoingShortage(std::string commodity, double severity, int start_tick, int duration)
        : commodity(std::move(commodity))
        , severity(severity)
        , start_tick(start_tick)
        , duration(duration) {};

    std::string commodity;
    double base_price;
    double severity;    // [0:-1]
    int start_tick;
    int duration;
};

struct OngoingSurplus {
    OngoingSurplus(std::string commodity, double severity, int start_tick, int duration)
        : commodity(std::move(commodity))
        , severity(severity)
        , start_tick(start_tick)
        , duration(duration) {};

    std::string commodity;
    double base_price;
    double severity;    // [0:inf]
    int start_tick;
    int duration;
};
//This class is used to simulate shortage/surplus events by placing loads of fake bids on command
class FakeTrader : public Trader {
private:
    std::weak_ptr<AuctionHouse> auction_house;
    int auction_house_id = -1;
    int lookback = 20;
    double LOW_PRICE = 0.2;
    double HIGH_PRICE = 10;

    std::vector<OngoingShortage> shortages = {};
    std::vector<OngoingSurplus> surpluses = {};
public:
    FakeTrader(int id, std::weak_ptr<AuctionHouse> auction_house_ptr)
        : Trader(id, "fake")
        , auction_house(std::move(auction_house_ptr)) {
        auction_house_id = auction_house.lock()->id;
    }

    bool HasMoney(double quantity) override;
    bool HasCommodity(const std::string& commodity, int quantity) override;

    void FlushOutbox();
    void FlushInbox();

    void RegisterShortage(const std::string& commodity, double severity, int start, int duration);
    void RegisterSurplus(const std::string& commodity, double severity, int start, int duration);

    //places 50 absurdly high bids for a good this tick
    void TriggerShortage(OngoingShortage& shortage);
    //places 50 absurdly low asks for a good this tick
    void TriggerSurplus(OngoingSurplus& surplus);

    void Tick();
private:
    friend AuctionHouse;
    double TryTakeMoney(double quantity, bool atomic) override;
    void AddMoney(double quantity) override;
    int TryAddCommodity(const std::string& commodity, int quantity, std::optional<double> unit_price, bool atomic) override;
    int TryTakeCommodity(const std::string& commodity, int quantity, std::optional<double> unit_price, bool atomic) override;
};

//Fake all these functions to pass AuctionHouse's trade checks (eg checking you have enough money for a trade)
bool FakeTrader::HasMoney(double quantity) {return true;}
bool FakeTrader::HasCommodity(const std::string& commodity, int quantity){return true;}

double FakeTrader::TryTakeMoney(double quantity, bool atomic){return quantity;}
void FakeTrader::AddMoney(double quantity) {}
int FakeTrader::TryAddCommodity(const std::string& commodity, int quantity, std::optional<double> unit_price, bool atomic) {return quantity;}
int FakeTrader::TryTakeCommodity(const std::string& commodity, int quantity, std::optional<double> unit_price, bool atomic) {return quantity;}

void FakeTrader::Tick() {
    FlushInbox();
    for (auto& surplus : surpluses) {
        TriggerSurplus(surplus);
    }
    for (auto& shortage : shortages) {
        TriggerShortage(shortage);
    }
    FlushOutbox();
    ticks++;
}

void FakeTrader::FlushOutbox() {
    auto outgoing = outbox.pop();
    while (outgoing) {
        // Trader can currently only talk to auction houses (not other traders)
        if (outgoing->first != auction_house_id) {
            continue;
        }
        auction_house.lock()->ReceiveMessage(std::move(outgoing->second));
        outgoing = outbox.pop();
    }
}

void FakeTrader::FlushInbox() {
    auto incoming_message = inbox.pop();
    while (incoming_message) {
        incoming_message = inbox.pop();
    }
}

void FakeTrader::RegisterShortage(const std::string& commodity, double severity, int start, int duration) {
    shortages.emplace_back(commodity, severity, start, duration);
}
void FakeTrader::RegisterSurplus(const std::string& commodity, double severity, int start, int duration) {
    surpluses.emplace_back(commodity, severity, start, duration);
}


void FakeTrader::TriggerShortage(OngoingShortage& shortage) {
    if (shortage.start_tick > ticks || shortage.start_tick + shortage.duration < ticks) {
        return;
    }

    if (shortage.start_tick == ticks) {
        shortage.base_price = auction_house.lock()->AverageHistoricalPrice(shortage.commodity, lookback);
    }
    // % through event
    double progress = double (ticks - shortage.start_tick)/shortage.duration;
    double price_distortion = 1 + shortage.severity*std::exp(-(4*progress - 2)*(4*progress - 2));

    double bid_price = shortage.base_price * price_distortion;
    bid_price = std::min(bid_price, HIGH_PRICE);
    auto offer = BidOffer(id, shortage.commodity, 50, bid_price);
    SendMessage(*Message(id).AddBidOffer(offer), auction_house_id);
}

void FakeTrader::TriggerSurplus(OngoingSurplus& surplus) {
    if (surplus.start_tick > ticks || surplus.start_tick + surplus.duration < ticks) {
        return;
    }

    if (surplus.start_tick == ticks) {
        surplus.base_price = auction_house.lock()->AverageHistoricalPrice(surplus.commodity, lookback);
    }
    // % through event
    double progress = double (ticks - surplus.start_tick)/surplus.duration;
    double price_distortion = 1 + surplus.severity*std::exp(-(4*progress - 2)*(4*progress - 2));

    double ask_price = surplus.base_price * price_distortion;
    ask_price = std::max(ask_price, LOW_PRICE);
    auto offer = AskOffer(id, surplus.commodity, 50, ask_price);
    SendMessage(*Message(id).AddAskOffer(offer), auction_house_id);
}
#endif//CPPBAZAARBOT_FAKE_TRADER_H
