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

namespace {
  enum TraderStatus {
    UNINITIALISED = 0,
    ACTIVE = 1,
    PENDING_DESTRUCTION = 2,
    DESTROYED = 3
};
}

class AITrader : public Trader {
private:
    std::atomic<bool> queue_active = true;

    std::string unique_name;
    
    int TICK_TIME_MS;

    int MAX_PROCESSED_MESSAGES_PER_FLUSH = 100;

    std::mt19937 rng_gen = std::mt19937(std::random_device()());
    double tracked_costs = 0;
    double MIN_COST = 10;
    double MIN_PRICE = 0.10;
    bool ready = false;

    messages::AIRole role = messages::AIRole::NONE;
    CommodityBeliefs commodity_beliefs;
    int auction_house_id = -1;

    std::map<std::string, std::vector<double>> observed_trading_range;

    int  external_lookback = 50*TICK_TIME_MS; //history range (num ticks)
    int internal_lookback = 50; //history range (num trades)

    double IDLE_TAX = 20;
    std::unique_ptr<Logger> logger;

    double money;

public:
    std::atomic<TraderStatus> status = TraderStatus::UNINITIALISED;

    AITrader(worker::Connection& connection, worker::View& view, int auction_house_id, messages::AIRole role, int tick_time_ms, Log::LogLevel verbosity = Log::WARN)
    : Trader(-1, "unregistered",  connection, view) //id is -1 until set by the SpatialOS Registration procedure
    , unique_name(RoleToString(role))
    , TICK_TIME_MS(tick_time_ms)
    , role(role)
    , auction_house_id(auction_house_id)
    , money(0){
        //construct inv_inventory = Inventory(inv_capacity, starting_inv);
      MakeCallbacks();
      logger = std::make_unique<Logger>(verbosity, unique_name);
    }

    ~AITrader() {
        logger->Log(Log::DEBUG, "Destroying AI trader");
    }

private:
    void MakeCallbacks() override;

    // MESSAGE PROCESSING
    void UpdatePriceModelFromProduction(worker::Map<std::basic_string<char>, int>& useful_production,
                                        worker::Map<std::basic_string<char>, int>& overproduction,
                                        worker::Map<std::basic_string<char>, int>& consumption);

    // INTERNAL LOGIC
    void GenerateOffers(const std::string& commodity);
    BidOffer CreateBid(const std::string& commodity, int min_limit, int max_limit, double desperation = 0);
    AskOffer CreateAsk(const std::string& commodity, int min_limit);
    void SendOffer(AskOffer& offer);
    void SendOffer(BidOffer& offer);
    int DetermineBuyQuantity(const std::string& commodity, double bid_price);
    int DetermineSaleQuantity(const std::string& commodity);

    std::pair<double, double> ObserveTradingRange(const std::string& commodity, int window);

public:
    void RequestShutdown();
    void Tick();
    void TickOnce();

    int GetIdeal(const std::string& name);
    int Query(const std::string& name);
    double QueryCost(const std::string& name);

    double GetIdleTax() { return IDLE_TAX;};
    double QueryMoney() { return money;};
protected:

    // EXTERNAL SETTERS (i.e. for auction house & role only)
    double TryTakeMoney(double quantity, bool atomic) override;
    void ForceTakeMoney(double quantity) override;
    void AddMoney(double quantity) override;
    double QuerySpace();
    int QueryShortage(const std::string& commodity);
    int QuerySurplus(const std::string& commodity);
    double QueryUnitSize(const std::string& commodity);
};

void AITrader::MakeCallbacks() {
  using RegisterTraderCommand = market::RegisterCommandComponent::Commands::RegisterCommand;
  using ReportBidResultCommand = trader::ReportOfferResultComponent::Commands::ReportBidOffer;
  using ReportAskResultCommand = trader::ReportOfferResultComponent::Commands::ReportAskOffer;
  using RequestShutdownCommand = market::RequestShutdownComponent::Commands::RequestShutdown;
  using RequestProductionCommand = market::RequestProductionComponent::Commands::RequestProduction;
  view.OnCommandResponse<RegisterTraderCommand>(
      [&](const worker::CommandResponseOp<RegisterTraderCommand>& op) {
        if (op.StatusCode != worker::StatusCode::kSuccess || !op.Response->accepted()) {
          status = TraderStatus::PENDING_DESTRUCTION;
          RequestShutdown();
          return;
        }
        id = op.Response->entity_id();
        status = ACTIVE;
      });
  view.OnCommandRequest<ReportBidResultCommand>(
      [&](const worker::CommandRequestOp<ReportBidResultCommand>& op) {
        connection.SendCommandResponse<ReportBidResultCommand>(op.RequestId, {true});
        auto commodity = op.Request.good();
        auto bought_price = op.Request.avg_price();
        auto quantity_traded= op.Request.quantity_bought();
        // TODO: Mutex lock this?
        for (int i = 0; i < quantity_traded; i++) {
          observed_trading_range[commodity].push_back(bought_price);
        }
        while ((int) observed_trading_range[commodity].size() > internal_lookback) {
          observed_trading_range[commodity].erase(observed_trading_range[commodity].begin());
        }
    });
  view.OnCommandRequest<ReportAskResultCommand>(
      [&](const worker::CommandRequestOp<ReportAskResultCommand>& op) {
        connection.SendCommandResponse<ReportAskResultCommand>(op.RequestId, {true});
        auto commodity = op.Request.good();
        auto sold_price = op.Request.avg_price();
        auto quantity_traded= op.Request.quantity_sold();
        for (int i = 0; i < quantity_traded; i++) {
          observed_trading_range[commodity].push_back(sold_price);
        }

        while ((int) observed_trading_range[commodity].size() > internal_lookback) {
          observed_trading_range[commodity].erase(observed_trading_range[commodity].begin());
        }
      });
  view.OnCommandResponse<RequestProductionCommand>(
      [&](const worker::CommandResponseOp<RequestProductionCommand>& op) {
        if (op.StatusCode == worker::StatusCode::kSuccess) {
          if (op.Response->bankrupt()) {
            messages::ShutdownRequest request = {id, role, 0, ticks};
            connection.SendCommandRequest<RequestShutdownCommand>(auction_house_id, request, {});
            status = PENDING_DESTRUCTION;
            return;
          }
          auto useful_production = op.Response->useful_production_result();
          auto wasted_production = op.Response->overproduction_result();
          auto consumption = op.Response->consumption_result();
          UpdatePriceModelFromProduction(useful_production, wasted_production, consumption);
        }
      });
  view.OnCommandResponse<RequestShutdownCommand>(
      [&](const worker::CommandResponseOp<RequestShutdownCommand>& op) {
        if (op.StatusCode == worker::StatusCode::kSuccess && op.Response->ack()) {
          status = DESTROYED;
          return;
        }
      });
}
void AITrader::UpdatePriceModelFromProduction(worker::Map<std::basic_string<char>, int>& useful_production,
                                              worker::Map<std::basic_string<char>, int>& overproduction,
                                              worker::Map<std::basic_string<char>, int>& consumption) {
    //For everything consumed, track_costs incremented by personal value
    for (auto& item : consumption) {
      tracked_costs += item.second*QueryCost(item.first);
    }
    //For everything produced, split the tracked costs across and set to zero
    int quantity = 0;
    for (auto& item : useful_production) {
      quantity += item.second;
    }
    tracked_costs = std::max(QueryMoney() / 50, tracked_costs);
    tracked_costs = std::max(MIN_COST, tracked_costs); //the richer you are, the greedier you get (the higher your minimum cost becomes)
    double unit_price = tracked_costs /  quantity;
    for (auto& item : useful_production) {
      commodity_beliefs.UpdateCostFromProduction(item.first, item.second, unit_price);
    }
    //For OVERPRODUCED items, drop the perceived value of the good (encourage selling it off)
    for (auto& item : overproduction) {
      commodity_beliefs.commodity_beliefs[item.first].cost *= std::pow(1.3, -1*item.second);
    }
};
double AITrader::TryTakeMoney(double quantity, bool atomic) {
    double amount_transferred;
    if (!atomic) {
        // Take what you can
        amount_transferred = std::min(money, quantity);
    } else {
        if (money < quantity) {
            logger->Log(Log::DEBUG, "Failed to take $"+std::to_string(quantity));
            amount_transferred = 0;
        } else {
            amount_transferred = quantity;
        }
    }
    money -= amount_transferred;
    return amount_transferred;
}
void AITrader::ForceTakeMoney(double quantity) {
    logger->Log(Log::DEBUG, "Lost money: $" + std::to_string(quantity));
    money -= quantity;
}
void AITrader::AddMoney(double quantity) {
    logger->Log(Log::DEBUG, "Gained money: $" + std::to_string(quantity));
    money += quantity;
}

int AITrader::GetIdeal(const std::string& name) {
    return commodity_beliefs.GetIdeal(name);
}
int AITrader::Query(const std::string& name) {
  auto inv = view.Entities[id].Get<trader::Inventory>()->inv();
  if (inv.count(name) != 1) {
    return 0; // no entry found
  }
  return inv[name].quantity();
}


double AITrader::QueryCost(const std::string& name) {
  return commodity_beliefs.GetCost(name);
}
int AITrader::QuerySurplus(const std::string& commodity) {
  return std::max(0, Query(commodity) - commodity_beliefs.GetIdeal(commodity));
}
int AITrader::QueryShortage(const std::string& commodity) {
  return std::max(0, commodity_beliefs.GetIdeal(commodity) - Query(commodity));
}
double AITrader::QuerySpace() {
  auto inv = view.Entities[id].Get<trader::Inventory>();
  double used_space = 0;
  for (auto& item : inv->inv()) {
    used_space += item.second.size()*item.second.quantity();
  }
  return inv->capacity() - used_space;
}
double AITrader::QueryUnitSize(const std::string& commodity) {
  auto inv = view.Entities[id].Get<trader::Inventory>()->inv();
  if (inv.count(commodity) != 1) {
    return 0; // no entry found
  }
  return inv[commodity].size();
}

void AITrader::SendOffer(AskOffer& offer) {
  messages::AskOffer msg = {id,
                            offer.commodity,
                            offer.expiry_ms,
                            offer.quantity,
                            offer.unit_price};
  using MakeAskOffer = market::MakeOfferCommandComponent::Commands::MakeAskOffer;
  logger->Log(Log::INFO, "Making bid offer: " + ToString(msg));
  connection.SendCommandRequest<MakeAskOffer>(auction_house_id, msg, {});
}
void AITrader::SendOffer(BidOffer& offer) {
  messages::BidOffer msg = {id,
                            offer.commodity,
                            offer.expiry_ms,
                            offer.quantity,
                            offer.unit_price};
  using MakeBidOffer = market::MakeOfferCommandComponent::Commands::MakeBidOffer;
  logger->Log(Log::INFO, "Making bid offer: " + ToString(msg));
  connection.SendCommandRequest<MakeBidOffer>(auction_house_id, msg, {});
}
void AITrader::GenerateOffers(const std::string& commodity) {
    int surplus = QuerySurplus(commodity);
    if (surplus >= 1) {
        auto offer = CreateAsk(commodity, 1);
        if (offer.quantity > 0) {
            SendOffer(offer);
        }
    }

    int shortage = QueryShortage(commodity);
    double space = QuerySpace();
    double unit_size = QueryUnitSize(commodity);


    double fulfillment;
    if (class_name == "refiner" || class_name == "blacksmith") {
        fulfillment = Query(commodity) / (0.001 + GetIdeal(commodity));
        fulfillment = std::max(0.5, fulfillment);
    } else {
        fulfillment = Query(commodity) / (0.001 + GetIdeal(commodity));
    }

    if (fulfillment < 1 && space >= unit_size) {
        int max_limit = (shortage*unit_size <= space) ? shortage : (int) space/shortage;
        if (max_limit > 0)
        {
            int min_limit = ( Query(commodity) == 0) ? 1 : 0;
//            logger->Log(Log::DEBUG, "Considering bid for "+commodity + std::string(" - Current shortage = ") + std::to_string(shortage));

            double desperation = 1;
            double days_savings = money / IDLE_TAX;
            desperation *= ( 5 /(days_savings*days_savings)) + 1;
            desperation *= 1 - (0.4*(fulfillment - 0.5))/(1 + 0.4*std::abs(fulfillment-0.5));
            auto offer = CreateBid(commodity, min_limit, max_limit, desperation);
            if (offer.quantity > 0) {
                SendOffer(offer);
            }
        }
    }
}
BidOffer AITrader::CreateBid(const std::string& commodity, int min_limit, int max_limit, double desperation) {
    double fair_bid_price;
    auto price_info = ToPriceInfo(view, auction_house_id, commodity);
    if (!price_info) {
      RequestShutdown();
        // quantity 0 BidOffers are never sent
        // (Yes this is hacky)
        return BidOffer(id, commodity, 0, -1, 0);
    }
    fair_bid_price = price_info->recent_price();
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
    auto price_info = ToPriceInfo(view, auction_house_id, commodity);
    if (!price_info) {
      RequestShutdown();
      // quantity 0 AskOffers are never sent
      // (Yes this is hacky)
      return AskOffer(id, commodity, 0, -1, 0);
    }
    market_price = price_info->recent_price();
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
        logger->Log(Log::WARN, "Tried to make bid with unitialised trading range");
        return 0;
    }
    double favorability = PositionInRange(avg_price, range.first, range.second);
    favorability = 1 - favorability; //do 1 - favorability to see how close we are to the low end
    double amount_to_buy = favorability * QueryShortage(commodity);//double

    return std::ceil(amount_to_buy);
}
int AITrader::DetermineSaleQuantity(const std::string& commodity) {
    return QuerySurplus(commodity); //Sell all surplus
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
void AITrader::RequestShutdown() {
    using RequestShutdownCommand = market::RequestShutdownComponent::Commands::RequestShutdown;
    connection.SendCommandRequest<RequestShutdownCommand>(auction_house_id, {id, role, 0, ticks}, {});
    status = PENDING_DESTRUCTION;
    logger->Log(Log::INFO, class_name+std::to_string(id)+std::string(" destroyed."));
}

void AITrader::Tick() {
    using ProductionCommand = market::RequestProductionComponent::Commands::RequestProduction;
    using std::chrono::milliseconds;
    using std::chrono::duration;
    using std::chrono::duration_cast;
    //Stagger starts
    std::this_thread::sleep_for(std::chrono::milliseconds{std::uniform_int_distribution<>(0, TICK_TIME_MS)(rng_gen)});
    logger->Log(Log::INFO, "Beginning tickloop");
    while (status > UNINITIALISED) {
        auto t1 = std::chrono::high_resolution_clock::now();
        if (status == ACTIVE) {
            connection.SendCommandRequest<ProductionCommand>(auction_house_id, {true}, {});
            for (const auto &commodity : commodity_beliefs.commodity_beliefs) {
                GenerateOffers(commodity.first);
            }
        }
        if (money <= 0) {
          RequestShutdown();
        }
        if (status == ACTIVE) {
            ticks++;
        }
        std::chrono::duration<double, std::milli> elapsed_ms = std::chrono::high_resolution_clock::now() - t1;
        int elapsed = elapsed_ms.count();
        if (elapsed < TICK_TIME_MS) {
            std::this_thread::sleep_for(std::chrono::milliseconds{TICK_TIME_MS - elapsed});
        } else {
            logger->Log(Log::WARN, "Trader thread overran on tick "+ std::to_string(ticks) + ": took " + std::to_string(elapsed) +"/" + std::to_string(TICK_TIME_MS) + "ms )");
        }
    }
}

void AITrader::TickOnce() {
    if (status != ACTIVE) {
        logger->Log(Log::DEBUG, "Not yet active, aborting tick");
        return;
    }
    using ProductionCommand = market::RequestProductionComponent::Commands::RequestProduction;
    if (status == ACTIVE) {
      connection.SendCommandRequest<ProductionCommand>(auction_house_id, {true}, {});
      for (const auto& commodity : commodity_beliefs.commodity_beliefs) {
        GenerateOffers(commodity.first);
      }
      ticks++;
    }
}

#endif//CPPBAZAARBOT_AI_TRADER_H
