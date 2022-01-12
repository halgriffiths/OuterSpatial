//
// Created by henry on 10/01/2022.
//
#ifndef CPPBAZAARBOT_HUMAN_TRADER_H
#define CPPBAZAARBOT_HUMAN_TRADER_H
#include "../common/agent.h"
#include "../common/history.h"
#include "../metrics/metrics.h"
#include "../metrics/display.h"

#include <optional>
#include <atomic>

class PlayerTrader : public Trader {
private:
    int MAX_PROCESSED_MESSAGES_PER_FLUSH = 100;

    std::atomic_bool ready = false;
    std::atomic_bool destroyed = false;

    std::string unique_name;

    std::thread message_thread{};
    std::thread UI_thread{};
    std::thread production_thread{};
    std::thread monitor_thread{};

    std::vector<std::string> tracked_goods{};
    std::vector<std::string> tracked_roles{};

    std::weak_ptr<AuctionHouse> auction_house{};
    int auction_house_id = -1;

    Inventory _inventory;
    LocalMetrics local_metrics;
    FileLogger logger;

    double money;
public:
    PlayerTrader(std::uint64_t start_time, int id, worker::Connection& connection, worker::View& view, std::weak_ptr<AuctionHouse> auction_house_ptr, double starting_money, double inv_capacity, const std::vector<InventoryItem> &starting_inv, std::vector<std::string> tracked_goods, std::vector<std::string> tracked_roles, Log::LogLevel verbosity)
            : Trader(id, "player", connection, view)
            , unique_name(class_name + std::to_string(id))
            , tracked_goods(tracked_goods)
            , tracked_roles(tracked_roles)
            , auction_house(std::move(auction_house_ptr))
            , local_metrics(start_time, tracked_goods,tracked_roles)
            , logger(FileLogger(verbosity, unique_name))
            , money(starting_money) {
        //construct inv
        auction_house_id = auction_house.lock()->id;
        _inventory = Inventory(inv_capacity, starting_inv);

        UI_thread = std::thread([this] { UILoop(); });
        production_thread = std::thread([this] { ProductionLoop(); });
        monitor_thread = std::thread([this] { MonitorLoop(); });
        message_thread = std::thread([this] { MessageLoop(); });
    }

    ~PlayerTrader() {
        logger.Log(Log::DEBUG, "Destroying Player trader");
        Shutdown();
        _inventory.inventory.clear();
        auction_house.reset();
    }
private:
    // THREAD LOOPS:
    // Handles incoming/outgoing messages to Auction House
    void MessageLoop();
    // Handles user-side UI and display functions. Mostly const but can interface with state (eg: user makes trade)
    void UILoop();
    // (not implemented) Intended to handle logic for a simple idle minigame
    void ProductionLoop();
    // Polls AH regularly to build a local understanding of price history, statistics etc to be used by UILoop
    void MonitorLoop();

    // MESSAGE PROCESSING
    void FlushOutbox();
    void FlushInbox();

    void ProcessBidResult(Message& message);
    void ProcessAskResult(Message& message);
    void ProcessRegistrationResponse(Message& message);

public:
    void Shutdown();

    // EXTERNAL QUERIES
    bool HasMoney(double quantity) override;
    bool HasCommodity(const std::string& commodity, int quantity) override;

    // EXTERNAL SETTERS (i.e. for auction house & production thread only)
    double TryTakeMoney(double quantity, bool atomic) override;
    void ForceTakeMoney(double quantity) override;
    void AddMoney(double quantity) override;

    int TryTakeCommodity(const std::string& commodity, int quantity, std::optional<double> unit_price, bool atomic) override;
    int TryAddCommodity(const std::string& commodity, int quantity, std::optional<double> unit_price, bool atomic) override;
};
void PlayerTrader::MessageLoop() {
    while (!destroyed) {
        FlushInbox();
        FlushOutbox();
    }
}
void PlayerTrader::ProductionLoop() {
    while (!ready) {}
    while (!destroyed) {
        //Idle game logic goes here...
    }
}
void PlayerTrader::UILoop() {
    while (!ready) {}
    while (!destroyed) {
        //UI logic goes here...

        for (auto& good : tracked_goods) {
            std::cout << "\t\t\t" << good;
        }
        std::cout << std::endl;
        for (auto& good : tracked_goods) {
            double curr_price = local_metrics.local_history.prices.most_recent[good];

            std::cout << "\t\t$" << curr_price;
            double pc_change = local_metrics.local_history.prices.t_percentage_change(good, 1000);
            if (pc_change < 0) {
                //▼
                std::cout << "\033[1;31m(▼" << pc_change << "%)\033[0m";
            } else if (pc_change > 0) {
                //▲
                std::cout << "\033[1;32m(▲" << pc_change << "%)\033[0m";
            } else {
                std::cout << "(" << pc_change << "%)";
            }
        }
        std::cout << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds{1000});
    }
}
void PlayerTrader::MonitorLoop() {
    while (!ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    while (!destroyed) {
        //Polling logic goes here...
        auto res = auction_house.lock();
        if (res) {
            local_metrics.CollectAuctionHouseMetrics(res);
        }
    }
}


void PlayerTrader::FlushOutbox() {
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
void PlayerTrader::FlushInbox() {
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
void PlayerTrader::ProcessBidResult(Message &message) {
}
void PlayerTrader::ProcessAskResult(Message& message) {
}

void PlayerTrader::ProcessRegistrationResponse(Message& message) {
    if (message.register_response->accepted) {
        ready = true;
        logger.Log(Log::INFO, "Successfully registered with auction house");
    } else {
        logger.Log(Log::ERROR, "Failed to register with auction house");
        Shutdown();
    }
}

bool PlayerTrader::HasMoney(double quantity) {
    return (money >= quantity);
}
double PlayerTrader::TryTakeMoney(double quantity, bool atomic) {
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
void PlayerTrader::ForceTakeMoney(double quantity) {
    logger.Log(Log::DEBUG, "Lost money: $" + std::to_string(quantity));
    money -= quantity;
}
void PlayerTrader::AddMoney(double quantity) {
    logger.Log(Log::DEBUG, "Gained money: $" + std::to_string(quantity));
    money += quantity;
}

bool PlayerTrader::HasCommodity(const std::string& commodity, int quantity) {
    auto stored = _inventory.Query(commodity);
    return (stored >= quantity);
}
int PlayerTrader::TryTakeCommodity(const std::string& commodity, int quantity, std::optional<double> unit_price, bool atomic) {
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
int PlayerTrader::TryAddCommodity(const std::string& commodity, int quantity, std::optional<double> unit_price, bool atomic) {
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
void PlayerTrader::Shutdown() {
    auto res = auction_house.lock();
    if (res) {
        res->ReceiveMessage(*Message(id).AddShutdownNotify({id, class_name, ticks}));
    }
    destroyed = true;
    message_thread.join();
    monitor_thread.join();
    production_thread.join();
    UI_thread.join();

    logger.Log(Log::INFO, unique_name+std::string(" destroyed."));
}

#endif//CPPBAZAARBOT_HUMAN_TRADER_H
