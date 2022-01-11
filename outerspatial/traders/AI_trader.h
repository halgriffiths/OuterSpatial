//
// Created by henry on 06/12/2021.
//

#ifndef CPPBAZAARBOT_AI_TRADER_H
#define CPPBAZAARBOT_AI_TRADER_H

#include <utility>

#include "inventory.h"
#include "../common/messages.h"

#include "../auction/auction_house.h"
#include "../metrics/logger.h"

class AITrader;

namespace {
    double PositionInRange(double value, double min, double max) {
        value -= min;
        max -= min;
        min = 0;
        value = (value / (max - min));

        if (value < 0) { value = 0; }
        if (value > 1) { value = 1; }

        return value;
    }
}

class Role {
private:
    // rng_gen
    std::mersenne_twister_engine<uint_fast32_t, 32, 624, 397, 31, 0x9908b0dfUL, 11, 0xffffffffUL, 7, 0x9d2c5680UL, 15, 0xefc60000UL, 18, 1812433253UL> rng_gen = std::mt19937(std::random_device()());

public:
    std::string required_good;
    Role(std::string required = "none", double min_cost = 1) : required_good(required), min_cost(min_cost){};
    bool Random(double chance);
    virtual void TickRole(AITrader & trader) = 0;
    void Produce(AITrader & trader, const std::string& commodity, int amount, double chance = 1);
    void Consume(AITrader & trader, const std::string& commodity, int amount, double chance = 1);
    void LoseMoney(AITrader & trader, double amount);
    double track_costs = 0;
    double min_cost; //minimum fair price for a single produced good
};


class AITrader : public Trader {
private:
    std::atomic<bool> queue_active = true;
    std::thread message_thread;

    std::string unique_name;
    
    int TICK_TIME_MS;

    int MAX_PROCESSED_MESSAGES_PER_FLUSH = 100;
    friend Role;
    std::mt19937 rng_gen = std::mt19937(std::random_device()());
    double MIN_PRICE = 0.10;
    bool ready = false;

    std::optional<std::shared_ptr<Role>> logic;
    Inventory _inventory;

    std::weak_ptr<AuctionHouse> auction_house;
    int auction_house_id = -1;

    std::map<std::string, std::vector<double>> observed_trading_range;

    int  external_lookback = 50*TICK_TIME_MS; //history range (num ticks)
    int internal_lookback = 50; //history range (num trades)

    double IDLE_TAX = 20;
    FileLogger logger;

    double money;

public:
    std::atomic<bool> destroyed = false;

    AITrader(int id, std::weak_ptr<AuctionHouse> auction_house_ptr, std::optional<std::shared_ptr<Role>> AI_logic, const std::string& class_name, double starting_money, double inv_capacity, const std::vector<InventoryItem> &starting_inv, int tick_time_ms, Log::LogLevel verbosity = Log::WARN)
    : Trader(id, class_name)
    , auction_house(std::move(auction_house_ptr))
    , logic(std::move(AI_logic))
    , money(starting_money)
    , unique_name(class_name + std::to_string(id))
    , logger(FileLogger(verbosity, unique_name))
    , TICK_TIME_MS(tick_time_ms) {
        //construct inv
        auction_house_id = auction_house.lock()->id;
        _inventory = Inventory(inv_capacity, starting_inv);
        for (const auto &item : starting_inv) {
            double base_price = auction_house.lock()->t_AverageHistoricalPrice(item.name, external_lookback);
            observed_trading_range[item.name] = {base_price*0.5, base_price*2};
            _inventory.SetCost(item.name, base_price);
        }
            message_thread = std::thread([this] { MessageLoop(); });
    }

    ~AITrader() {
        logger.Log(Log::DEBUG, "Destroying AI trader");
        ShutdownMessageThread();
        _inventory.inventory.clear();
        auction_house.reset();
        logic.reset();
    }

private:
    // MESSAGE PROCESSING
    void FlushOutbox();
    void FlushInbox();

    void ProcessBidResult(Message& message);
    void ProcessAskResult(Message& message);
    void ProcessRegistrationResponse(Message& message);

    void UpdatePriceModelFromBid(BidResult& result);
    void UpdatePriceModelFromAsk(const AskResult& result);

    // INTERNAL LOGIC
    void GenerateOffers(const std::string& commodity);
    BidOffer CreateBid(const std::string& commodity, int min_limit, int max_limit, double desperation = 0);
    AskOffer CreateAsk(const std::string& commodity, int min_limit);

    int DetermineBuyQuantity(const std::string& commodity, double bid_price);
    int DetermineSaleQuantity(const std::string& commodity);

    std::pair<double, double> ObserveTradingRange(const std::string& commodity, int window);

    void ShutdownMessageThread();
public:
    void Shutdown();
    void Tick();
    void TickOnce();
    void MessageLoop();



    int GetIdeal(const std::string& name);
    int Query(const std::string& name);
    double QueryCost(const std::string& name);

    double GetIdleTax() { return IDLE_TAX;};
    double QueryMoney() { return money;};
protected:
    // EXTERNAL QUERIES
    bool HasMoney(double quantity) override;
    bool HasCommodity(const std::string& commodity, int quantity) override;

    // EXTERNAL SETTERS (i.e. for auction house & role only)
    double TryTakeMoney(double quantity, bool atomic) override;
    void ForceTakeMoney(double quantity) override;
    void AddMoney(double quantity) override;

    int TryTakeCommodity(const std::string& commodity, int quantity, std::optional<double> unit_price, bool atomic) override;
    int TryAddCommodity(const std::string& commodity, int quantity, std::optional<double> unit_price, bool atomic) override;
};

void AITrader::FlushOutbox() {
        logger.Log(Log::DEBUG, "Flushing outbox");
        auto outgoing = outbox.pop();
        int num_processed = 0;
        while (outgoing && num_processed < MAX_PROCESSED_MESSAGES_PER_FLUSH) {
            // Trader can currently only talk to auction houses (not other traders)
            if (outgoing->first != auction_house_id) {
                logger.Log(Log::ERROR, "Failed to send message, unknown recipient " + std::to_string(outgoing->first));
            } else {
                logger.LogSent(outgoing->first, Log::DEBUG, outgoing->second.ToString());
                auto res = auction_house.lock();
                if (res) {
                    res->ReceiveMessage(std::move(outgoing->second));
                } else {
                    queue_active = false;
                    destroyed = true;
                    return;
                }
            }
            num_processed++;
            outgoing = outbox.pop();
        }
    if (num_processed == MAX_PROCESSED_MESSAGES_PER_FLUSH) {
        logger.Log(Log::WARN, "Outbox not fully flushed");
    }
    logger.Log(Log::DEBUG, "Flush finished");
}
void AITrader::FlushInbox() {
    logger.Log(Log::DEBUG, "Flushing inbox");
    auto incoming_message = inbox.pop();
    int num_processed = 0;
    while (incoming_message && num_processed < MAX_PROCESSED_MESSAGES_PER_FLUSH) {
        logger.LogReceived(incoming_message->sender_id, Log::INFO, incoming_message->ToString());
        if (incoming_message->GetType() == Msg::EMPTY) {
            //no-op
        } else if (incoming_message->GetType() == Msg::BID_RESULT) {
            ProcessBidResult(*incoming_message);
        } else if (incoming_message->GetType() == Msg::ASK_RESULT) {
            ProcessAskResult(*incoming_message);
        } else if (incoming_message->GetType() == Msg::REGISTER_RESPONSE) {
            ProcessRegistrationResponse(*incoming_message);
        } else if (incoming_message->GetType() == Msg::SHUTDOWN_COMMAND) {
            destroyed = true;
            queue_active = false;
        } else {
            logger.Log(Log::ERROR, "Unknown/unsupported message type");
        }
        num_processed++;
        incoming_message = inbox.pop();
    }
    if (num_processed == MAX_PROCESSED_MESSAGES_PER_FLUSH) {
        logger.Log(Log::WARN, "Inbox not fully flushed");
    }
    logger.Log(Log::DEBUG, "Flush finished");
}
void AITrader::ProcessAskResult(Message& message) {
    UpdatePriceModelFromAsk(*message.ask_result);
}
void AITrader::ProcessBidResult(Message& message) {
    UpdatePriceModelFromBid(*message.bid_result);
}
void AITrader::ProcessRegistrationResponse(Message& message) {
    if (message.register_response->accepted) {
        ready = true;
        logger.Log(Log::INFO, "Successfully registered with auction house");
    } else {
        logger.Log(Log::ERROR, "Failed to register with auction house");
        Shutdown();
    }
}

bool AITrader::HasMoney(double quantity) {
    return (money >= quantity);
}
double AITrader::TryTakeMoney(double quantity, bool atomic) {
    double amount_transferred;
    if (!atomic) {
        // Take what you can
        amount_transferred = std::min(money, quantity);
    } else {
        if (money < quantity) {
            logger.Log(Log::DEBUG, "Failed to take $"+std::to_string(quantity));
            amount_transferred = 0;
        } else {
            amount_transferred = quantity;
        }
    }
    money -= amount_transferred;
    return amount_transferred;
}
void AITrader::ForceTakeMoney(double quantity) {
    logger.Log(Log::DEBUG, "Lost money: $" + std::to_string(quantity));
    money -= quantity;
}
void AITrader::AddMoney(double quantity) {
    logger.Log(Log::DEBUG, "Gained money: $" + std::to_string(quantity));
    money += quantity;
}

bool AITrader::HasCommodity(const std::string& commodity, int quantity) {
    auto stored = _inventory.Query(commodity);
    return (stored >= quantity);
}
int AITrader::TryTakeCommodity(const std::string& commodity, int quantity, std::optional<double> unit_price, bool atomic) {
    auto comm = _inventory.GetItem(commodity);
    if (!comm) {
        //item unknown, fail
        logger.Log(Log::ERROR, "Tried to take unknown item "+commodity);
        return 0;
    }
    int actual_transferred ;
    auto stored = _inventory.Query(commodity);
    if ( stored>= quantity) {
        actual_transferred = quantity;
    } else {
        if (atomic) {
            actual_transferred = 0;
            logger.Log(Log::DEBUG, "Failed to take "+commodity+std::string(" x") + std::to_string(quantity));
        } else {
            actual_transferred = stored;
        }
    }
    _inventory.TakeItem(commodity, actual_transferred, unit_price);
    return actual_transferred;
}
int AITrader::TryAddCommodity(const std::string& commodity, int quantity, std::optional<double> unit_price, bool atomic) {
    auto comm = _inventory.GetItem(commodity);
    if (!comm) {
        //item unknown, fail
        logger.Log(Log::ERROR, "Tried to add unknown item "+commodity);
        return 0;
    }
    int actual_transferred;
    if (_inventory.GetEmptySpace() >= quantity*comm->size) {
        actual_transferred = quantity;
    } else {
        if (atomic) {
            actual_transferred = 0;
            logger.Log(Log::DEBUG, "Failed to add "+commodity+std::string(" x") + std::to_string(quantity));
        } else {
            actual_transferred = std::floor(_inventory.GetEmptySpace()/comm->size);
            //overproduced! Drop value of goods accordingly
            int overproduction = quantity - actual_transferred;
            _inventory.inventory[commodity].original_cost *= std::pow(1.3, -1*overproduction);
        }
    }
    _inventory.AddItem(commodity, actual_transferred, unit_price);
    return actual_transferred;
}
int AITrader::GetIdeal(const std::string& name) {
    auto res = _inventory.GetItem(name);
    if (!res) {
        return 0;
    }
    return res->ideal_quantity;
}
int AITrader::Query(const std::string& name) { return _inventory.Query(name); }
double AITrader::QueryCost(const std::string& name) { return _inventory.QueryCost(name); }

// Trading functions
void AITrader::UpdatePriceModelFromBid(BidResult& result) {
    for (int i = 0; i < result.quantity_traded; i++) {
        observed_trading_range[result.commodity].push_back(result.bought_price);
    }

    while (observed_trading_range[result.commodity].size() > internal_lookback) {
        observed_trading_range[result.commodity].erase(observed_trading_range[result.commodity].begin());
    }
}
void AITrader::UpdatePriceModelFromAsk(const AskResult& result) {

    for (int i = 0; i < result.quantity_traded; i++) {
        observed_trading_range[result.commodity].push_back(result.avg_price);
    }

    while (observed_trading_range[result.commodity].size() > internal_lookback) {
        observed_trading_range[result.commodity].erase(observed_trading_range[result.commodity].begin());
    }
}

void AITrader::GenerateOffers(const std::string& commodity) {
    int surplus = _inventory.Surplus(commodity);
    if (surplus >= 1) {
//        logger.Log(Log::DEBUG, "Considering ask for "+commodity + std::string(" - Current surplus = ") + std::to_string(surplus));
        auto offer = CreateAsk(commodity, 1);
        if (offer.quantity > 0) {
            SendMessage(*Message(id).AddAskOffer(offer), auction_house_id);
        }
    }

    int shortage = _inventory.Shortage(commodity);
    double space = _inventory.GetEmptySpace();
    double unit_size = _inventory.GetSize(commodity);


    double fulfillment;
    if (class_name == "refiner" || class_name == "blacksmith") {
        fulfillment = _inventory.Query(commodity) / (0.001 + _inventory.GetItem(commodity)->ideal_quantity);
        fulfillment = std::max(0.5, fulfillment);
    } else {
        fulfillment = _inventory.Query(commodity) / (0.001 + _inventory.GetItem(commodity)->ideal_quantity);
    }

    if (fulfillment < 1 && space >= unit_size) {
        int max_limit = (shortage*unit_size <= space) ? shortage : (int) space/shortage;
        if (max_limit > 0)
        {
            int min_limit = (_inventory.Query(commodity) == 0) ? 1 : 0;
//            logger.Log(Log::DEBUG, "Considering bid for "+commodity + std::string(" - Current shortage = ") + std::to_string(shortage));

            double desperation = 1;
            double days_savings = money / IDLE_TAX;
            desperation *= ( 5 /(days_savings*days_savings)) + 1;
            desperation *= 1 - (0.4*(fulfillment - 0.5))/(1 + 0.4*std::abs(fulfillment-0.5));
            auto offer = CreateBid(commodity, min_limit, max_limit, desperation);
            if (offer.quantity > 0) {
                SendMessage(*Message(id).AddBidOffer(offer), auction_house_id);
            }
        }
    }
}
BidOffer AITrader::CreateBid(const std::string& commodity, int min_limit, int max_limit, double desperation) {
    double fair_bid_price;
    auto res = auction_house.lock();
    if (res) {
        fair_bid_price = res->t_AverageHistoricalPrice(commodity, external_lookback);
    } else {
        destroyed = true;
        // quantity 0 BidOffers are never sent
        // (Yes this is hacky)
        return BidOffer(id, commodity, 0, -1, 0);
    }
    //scale between price based on need
    double max_price = money;
    double min_price = MIN_PRICE;
    double bid_price = fair_bid_price *desperation;
    bid_price = std::max(std::min(max_price, bid_price), min_price);

    int ideal = DetermineBuyQuantity(commodity, bid_price);
    int quantity = std::max(std::min(ideal, max_limit), min_limit);

    //set to expire just before next tick
    std::uint64_t expiry_ms = to_unix_timestamp_ms(std::chrono::system_clock::now()) + TICK_TIME_MS;
    return BidOffer(id, commodity, quantity, bid_price, expiry_ms);
}
AskOffer AITrader::CreateAsk(const std::string& commodity, int min_limit) {
    //AI agents offer a fair ask price - costs + 15% profit
    double market_price;
    double ask_price;
    auto res = auction_house.lock();
    if (res) {
        market_price = res->t_AverageHistoricalBuyPrice(commodity, external_lookback);
    } else {
        destroyed = true;
        // quantity 0 AskOffers are never sent
        // (Yes this is hacky)
        return AskOffer(id, commodity, 0, -1, 0);
    }
    double fair_price = QueryCost(commodity) * 1.15;

    std::uniform_real_distribution<> random_price(fair_price, market_price);
    ask_price = random_price(rng_gen);
    ask_price = std::max(MIN_PRICE, ask_price);
    int quantity = DetermineSaleQuantity(commodity);
    //can't sell less than limit
    quantity = quantity < min_limit ? min_limit : quantity;

    //set to expire just before next tick
    std::uint64_t expiry_ms = to_unix_timestamp_ms(std::chrono::system_clock::now()) + TICK_TIME_MS;
    return AskOffer(id, commodity, quantity, ask_price, expiry_ms);
}

int AITrader::DetermineBuyQuantity(const std::string& commodity, double avg_price) {
    std::pair<double, double> range = ObserveTradingRange(commodity, internal_lookback);
    if (range.first == 0 && range.second == 0) {
        //uninitialised range
        logger.Log(Log::WARN, "Tried to make bid with unitialised trading range");
        return 0;
    }
    double favorability = PositionInRange(avg_price, range.first, range.second);
    favorability = 1 - favorability; //do 1 - favorability to see how close we are to the low end
    double amount_to_buy = favorability * _inventory.Shortage(commodity);//double

    return std::ceil(amount_to_buy);
}
int AITrader::DetermineSaleQuantity(const std::string& commodity) {
    return _inventory.Surplus(commodity); //Sell all surplus
}

std::pair<double, double> AITrader::ObserveTradingRange(const std::string& commodity, int window) {
    if (observed_trading_range.count(commodity) < 1 || observed_trading_range[commodity].empty()) {
        return {0,0};
    }
    double min_observed = observed_trading_range[commodity][0];
    double max_observed = observed_trading_range[commodity][0];
    window = std::min(window, (int) observed_trading_range[commodity].size());

    for (int i = 0; i < window; i++) {
        min_observed = std::min(min_observed, observed_trading_range[commodity][i]);
        max_observed = std::max(max_observed, observed_trading_range[commodity][i]);
    }
    return {min_observed, max_observed};
}

// Misc
void AITrader::ShutdownMessageThread() {
    logger.Log(Log::INFO, "Shutting down message thread...");
    queue_active = false;
    if (message_thread.joinable()) {
        message_thread.join();
    }
    logger.Log(Log::INFO, "Message thread shutdown");
}

void AITrader::Shutdown() {
    auto res = auction_house.lock();
    if (res) {
        res->ReceiveMessage(*Message(id).AddShutdownNotify({id, class_name, ticks}));
    }
    destroyed = true;
    logger.Log(Log::INFO, class_name+std::to_string(id)+std::string(" destroyed."));
}

void AITrader::Tick() {
    using std::chrono::milliseconds;
    using std::chrono::duration;
    using std::chrono::duration_cast;
    //Stagger starts
    std::this_thread::sleep_for(std::chrono::milliseconds{std::uniform_int_distribution<>(0, TICK_TIME_MS)(rng_gen)});
    logger.Log(Log::INFO, "Beginning tickloop");
    while (!destroyed) {
        auto t1 = std::chrono::high_resolution_clock::now();
        if (ready) {
            if (logic) {
                logger.Log(Log::DEBUG, "Ticking internal logic");
                (*logic)->TickRole(*this);
            }
            for (const auto &commodity : _inventory.inventory) {
                GenerateOffers(commodity.first);
            }
        }
        if (money <= 0) {
            Shutdown();
        }
        if (ready) {
            ticks++;
        }
        std::chrono::duration<double, std::milli> elapsed_ms = std::chrono::high_resolution_clock::now() - t1;
        int elapsed = elapsed_ms.count();
        if (elapsed < TICK_TIME_MS) {
            std::this_thread::sleep_for(std::chrono::milliseconds{TICK_TIME_MS - elapsed});
        } else {
            logger.Log(Log::WARN, "Trader thread overran on tick "+ std::to_string(ticks) + ": took " + std::to_string(elapsed) +"/" + std::to_string(TICK_TIME_MS) + "ms )");
        }
    }
}

void AITrader::TickOnce() {
    if (destroyed) {
        return;
    }
    if (ready) {
        if (logic) {
            logger.Log(Log::DEBUG, "Ticking internal logic");
            (*logic)->TickRole(*this);
        }
        for (const auto& commodity : _inventory.inventory) {
            GenerateOffers(commodity.first);
        }
    }
    if (money <= 0) {
        destroyed = true;
        return;
    }
    if (ready){
        ticks++;
    }
}

void AITrader::MessageLoop() {
    while (true) {
        if (!queue_active) {
            return;
        }
        FlushInbox();
        FlushOutbox();
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
}

bool Role::Random(double chance) {
    if (chance >= 1) return true;
    return (rng_gen() < chance*rng_gen.max());
}
void Role::Produce(AITrader& trader, const std::string& commodity, int amount, double chance) {
    if (amount > 0 && Random(chance)) {
        trader.logger.Log(Log::DEBUG, "Produced " + std::string(commodity) + std::string(" x") + std::to_string(amount));

        //the richer you are, the greedier you get (the higher your minimum cost becomes)
        track_costs = std::max(trader.QueryMoney() / 50, track_costs);
        track_costs = std::max(min_cost, track_costs);
        trader.TryAddCommodity(commodity, amount, track_costs /  amount, false);
        track_costs = 0;
    }
}
void Role::Consume(AITrader& trader, const std::string& commodity, int amount, double chance) {
    if (Random(chance)) {
        trader.logger.Log(Log::DEBUG, "Consumed " + std::string(commodity) + std::string(" x") + std::to_string(amount));
        int actual_quantity = trader.TryTakeCommodity(commodity, amount, 0, false);
        if (actual_quantity > 0) {
            track_costs += actual_quantity*trader.QueryCost(commodity);
        }
    }
}
void Role::LoseMoney(AITrader& trader, double amount) {
    trader.ForceTakeMoney(amount);
    //track_costs += amount;
}

#endif//CPPBAZAARBOT_AI_TRADER_H
