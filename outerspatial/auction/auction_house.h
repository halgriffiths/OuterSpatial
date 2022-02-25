//
// Created by henry on 06/12/2021.
//

#ifndef CPPBAZAARBOT_AUCTION_HOUSE_H
#define CPPBAZAARBOT_AUCTION_HOUSE_H

#include <random>
#include <algorithm>
#include <utility>
#include <memory>

#include "../common/history.h"

#include "../common/agent.h"
#include "../common/messages.h"
#include "../traders/inventory.h"
#include "../common/commodity.h"

#include "../metrics/logger.h"

#include <thread>


namespace {
  messages::AIRole RandomChoice(std::vector<std::pair<messages::AIRole, double>>& weights, std::mt19937& gen) {
    double sum_of_weight = 0;
    int num_weights = weights.size();
    for(int i=0; i<num_weights; i++) {
      sum_of_weight += weights[i].second;
    }
    std::uniform_real_distribution<> random(0, sum_of_weight);
    double rnd = random(gen);
    for(int i=0; i<num_weights; i++) {
      if(rnd < weights[i].second)
        return weights[i].first;
      rnd -= weights[i].second;
    }
    return messages::AIRole::NONE;
  }

  messages::AIRole GetProducer(const std::string& commodity) {
    if (commodity == "food") {
      return messages::AIRole::FARMER;
    } else if (commodity == "fertilizer") {
      return messages::AIRole::COMPOSTER;
    } else if (commodity == "wood") {
      return messages::AIRole::WOODCUTTER;
    } else if (commodity == "ore") {
      return messages::AIRole::MINER;
    } else if (commodity == "metal") {
      return messages::AIRole::REFINER;
    } else if (commodity == "tools") {
      return messages::AIRole::BLACKSMITH;
    } else {
      return messages::AIRole::NONE;
    }
  }
}
namespace ah {
  enum RegisterProgress {
    NONE,
    RESERVED_ID,
    CREATED_ENTITY,
    ASSIGNED_PARTITION
  };
}
class AuctionHouse : public Agent {
public:
    History history;
    std::atomic_bool destroyed = false;
    
    std::string unique_name;
private:
    // debug info
    int num_deaths;
    int total_age;

    int TICK_TIME_MS; //ms
    std::atomic<bool> queue_active = true;

    std::mutex bid_book_mutex;
    std::mutex ask_book_mutex;

    int MAX_PROCESSED_MESSAGES_PER_FLUSH = 800;
    double SALES_TAX = 0.08;
    double BROKER_FEE = 0.03;
    int ticks = 0;
    std::mt19937 rng_gen = std::mt19937(std::random_device()());
    std::map<std::string, Commodity> known_commodities;
    std::map<std::string, int> demographics = {};

    std::map<std::string, std::vector<std::pair<BidOffer, BidResult>>> bid_book = {};
    std::map<std::string, std::vector<std::pair<AskOffer, AskResult>>> ask_book = {};
    std::unique_ptr<Logger> logger;

public:
    double spread_profit = 0;
    AuctionHouse(worker::Connection& connection, worker::View& view, int auction_house_id, int tick_time_ms, Log::LogLevel verbosity)
        : Agent(auction_house_id, connection, view)
        , unique_name(std::string("AH")+std::to_string(id))
        , TICK_TIME_MS(tick_time_ms) {
        logger = std::make_unique<SpatialLogger>(verbosity, unique_name, connection);
        ConstructInitialAuctionHouseEntity(auction_house_id);
        MakeCallbacks();
    }

    ~AuctionHouse() override {
        logger->Log(Log::DEBUG, "Destroying auction house");
    }

    std::pair<double, std::map<std::string, int>> GetDemographics() const {
        return {(num_deaths > 0) ? total_age / num_deaths : 0, demographics};
    }

    void Shutdown(){
        destroyed = true;
    }

    void SendDirect(Message outgoing_message, std::shared_ptr<Agent>& recipient) {
        logger->Log(Log::WARN, "Using SendDirect method to reach unregistered trader");
        logger->LogSent(recipient->id, Log::DEBUG, outgoing_message.ToString());
        recipient->ReceiveMessage(std::move(outgoing_message));
    }

    // Message processing
    void ProcessBid(Message& message) {
        auto bid = message.bid_offer;
        if (!bid) {
            logger->Log(Log::ERROR, "Malformed bid_offer message");
            return; //drop
        }
        bid_book_mutex.lock();
        bid_book[bid->commodity].push_back({*bid, {id, bid->commodity, bid->unit_price} });
        bid_book_mutex.unlock();
    }
    void ProcessAsk(Message& message) {
        auto ask = message.ask_offer;
        if (!ask) {
            logger->Log(Log::ERROR, "Malformed ask_offer message");
            return; //drop
        }
        ask_book_mutex.lock();
        ask_book[ask->commodity].push_back({*ask, {id, ask->commodity} });
        ask_book_mutex.unlock();
    }

  messages::AIRole ChooseNewClassWeighted() {
    std::vector<std::pair<messages::AIRole, double>> weights;
    double gamma = -0.02;
    //auction house ticks at 10ms
    int lookback_time_ms = 100*TICK_TIME_MS;
    for (auto& commodity : known_commodities) {
      double supply = t_AverageHistoricalSupply(commodity.first, lookback_time_ms);
//        double supply = auction_house->AverageHistoricalAsks(commodity, 100) - auction_house->AverageHistoricalBids(commodity, 100);
      weights.emplace_back(GetProducer(commodity.first), std::exp(gamma*supply));
    }
    return RandomChoice(weights, rng_gen);
  }

    void IncrementDemographic(messages::AIRole role) {
      if (demographics.count(role) != 1) {
        demographics[role] = 1;
      } else {
        demographics[role] += 1;
      }
    }
    void DecrementDemographic(messages::AIRole role) {
      if (demographics.count(role) != 1) {
        demographics[role] = 0;
      } else {
        demographics[role] -= 1;
      }
    }
    void UpdateDemographicInfoComponent() {
      market::DemographicInfo::Update update_dems;
      update_dems.set_total_deaths(num_deaths);
      update_dems.set_role_counts(demographics);
      if (num_deaths > 0) {
        update_dems.set_average_age_ticks(total_age/num_deaths);
      } else {
        update_dems.set_average_age_ticks(0);
      }
      connection.SendComponentUpdate<market::DemographicInfo>(id, update_dems);
    }
  template <class Tmarket>
  void UpdatePriceInfoComponent(const std::string& commodity) {
      static_assert(std::is_base_of<::worker::detail::ComponentMetaclass, Tmarket>::value, "T must inherit from ComponentMetaclass");
      int recent = 50*TICK_TIME_MS; // arbritrary choice

      // Get data
      double curr_price = history.prices.t_average(commodity, TICK_TIME_MS);
      double recent_price = history.prices.t_average(commodity, recent);

      int curr_net_supply = history.net_supply.t_average(commodity, TICK_TIME_MS);
      int recent_net_supply = history.net_supply.t_average(commodity, recent);

      int recent_trade_volume = history.trades.t_total(commodity, recent);

      // Send update
      market::PriceInfo info{curr_price, recent_price, curr_net_supply, recent_net_supply, recent_trade_volume};
      info.set_curr_price(curr_price);


      market::MarketListing listing;
      listing.set_price_info(info);

      typename Tmarket::Update update_market;
      update_market.set_listing(listing);
      connection.SendComponentUpdate<Tmarket>(id, update_market);
    }

    double MostRecentBuyPrice(const std::string& commodity) const {
        return history.buy_prices.most_recent.at(commodity);
    }
    double MostRecentPrice(const std::string& commodity) const {
        return history.prices.most_recent.at(commodity);
    }
    double AverageHistoricalBuyPrice(const std::string& commodity, int window) const {
        if (window == 1) {
            return history.buy_prices.most_recent.at(commodity);
        }
        return history.buy_prices.average(commodity, window);
    }
    double t_AverageHistoricalBuyPrice(const std::string& commodity, int window) const {
        return history.buy_prices.t_average(commodity, window);
    }

    double AverageHistoricalPrice(const std::string& commodity, int window) const {
        if (window == 1) {
            return history.prices.most_recent.at(commodity);
        }
        return history.prices.average(commodity, window);
    }
    double t_AverageHistoricalPrice(const std::string& commodity, int window) const {
        return history.prices.t_average(commodity, window);
    }

    double AverageHistoricalTrades(const std::string& commodity, int window) const {
        if (window == 1) {
            return history.trades.most_recent.at(commodity);
        }
        return history.trades.average(commodity, window);
    }
    double AverageHistoricalAsks(const std::string& commodity, int window) const {
        if (window == 1) {
            return history.asks.most_recent.at(commodity);
        }
        return history.asks.average(commodity, window);
    }
    double AverageHistoricalBids(const std::string& commodity, int window) const {
        if (window == 1) {
            return history.bids.most_recent.at(commodity);
        }return history.bids.average(commodity, window);
    }

    double t_AverageHistoricalAsks(const std::string& commodity, int window) const {
        return history.asks.t_average(commodity, window);
    }
    double t_AverageHistoricalBids(const std::string& commodity, int window) const {
        return history.bids.t_average(commodity, window);
    }

    double AverageHistoricalSupply(const std::string& commodity, int window) const {
        return history.net_supply.average(commodity, window);
    }
    double t_AverageHistoricalSupply(const std::string& commodity, int window) const {
        return history.net_supply.t_average(commodity, window);
    }

    void RegisterCommodity(const Commodity& new_commodity) {
        if (known_commodities.find(new_commodity.name) != known_commodities.end()) {
            //already exists
            return;
        }
        history.initialise(new_commodity.name);
        known_commodities[new_commodity.name] = new_commodity;

        bid_book_mutex.lock();
        bid_book[new_commodity.name] = {};
        bid_book_mutex.unlock();

        ask_book_mutex.lock();
        ask_book[new_commodity.name] = {};
        ask_book_mutex.unlock();
    }

    void Tick(int duration) {
        std::int64_t expiry_ms = to_unix_timestamp_ms(std::chrono::system_clock::now()) + duration;
        while (!destroyed) {
            auto t1 = std::chrono::high_resolution_clock::now();
            TickOnce();
            ticks++;

            std::chrono::duration<double, std::milli> elapsed_ms = std::chrono::high_resolution_clock::now() - t1;
            int elapsed = elapsed_ms.count();
            if (elapsed < TICK_TIME_MS) {
                std::this_thread::sleep_for(std::chrono::milliseconds{TICK_TIME_MS - elapsed});
            } else {
                logger->Log(Log::WARN, "AH thread overran on tick "+ std::to_string(ticks) + ": took " + std::to_string(elapsed) +"/" + std::to_string(TICK_TIME_MS) + "ms )");
            }
            if (to_unix_timestamp_ms(std::chrono::system_clock::now()) > expiry_ms) {
              logger->Log(Log::ERROR, "Shutting down (expiry time reached)");
              Shutdown();
            }
        }
    }

    void TickOnce() {
      for (const auto& item : known_commodities) {
        ResolveOffers(item.first);
      }
      logger->Log(Log::INFO, "Net spread profit for tick" + std::to_string(ticks) + ": " + std::to_string(spread_profit));

      UpdatePriceInfoComponent<market::FoodMarket>("food");
      UpdatePriceInfoComponent<market::WoodMarket>("wood");
      UpdatePriceInfoComponent<market::FertilizerMarket>("fertilizer");
      UpdatePriceInfoComponent<market::OreMarket>("ore");
      UpdatePriceInfoComponent<market::MetalMarket>("metal");
      UpdatePriceInfoComponent<market::ToolsMarket>("tools");

    }
    double QuerySpace(trader::InventoryData& inv) {
      double used_space = 0;
      for (auto& item : inv.inv()) {
        used_space += item.second.size()*item.second.quantity();
      }
      return inv.capacity() - used_space;
    }
    int ConsumeItem(trader::InventoryItem& item, int quantity) {
      int actual_consumed = std::max(quantity, item.quantity());
      item.set_quantity(item.quantity() - actual_consumed);
      return actual_consumed;
    };
    int ProduceItem(trader::InventoryItem& item, int quantity, double capacity) {
      int actual_produced = std::max(quantity, (int) std::floor(capacity * item.size()));
      item.set_quantity(item.quantity() + actual_produced);
      return actual_produced;
    }
    bool CheckTraderHasItem(std::string& commodity, int quantity, trader::InventoryData& inv) {
      if (inv.inv().count(commodity) != 1) return false;
      if (inv.inv()[commodity].quantity() < quantity ) return false;
      return true;
    }
    bool CheckTraderHasMoney(double quantity, trader::InventoryData& inv) {
      return (inv.cash() >= quantity);
    }
    bool CheckBuildingRequirementsMet(worker::List<trader::Consumption>& all_requirements, trader::InventoryData& inv) {
      for (auto& requirement : all_requirements) {
        if (!CheckTraderHasItem(requirement.item().name(), requirement.quantity(), inv)) {
          return false;
        }
      }
      return true;
    }
    std::optional<messages::ProductionResponse> TickWorkerProduction(const worker::CommandRequestOp<market::RequestProductionComponent::Commands::RequestProduction>& op) {
        ::worker::Map< std::string, std::int32_t> production = {};
        ::worker::Map< std::string, std::int32_t> overproduction = {};
        ::worker::Map< std::string, std::int32_t> consumption = {};

        auto trader_buildings = view.Entities[op.Request.sender_id()].Get<trader::AIBuildings>();
        auto trader_inventory = view.Entities[op.Request.sender_id()].Get<trader::Inventory>();
        if (!trader_inventory) {
          return {};
        }
        if (!trader_buildings) {
          return {};
        }

        auto all_buildings = trader_buildings->buildings();
        std::vector<trader::Building*> building_ptrs;
        for (auto& item : all_buildings) {
          building_ptrs.push_back(&item);
        }
        // Sort production options by priority
        std::sort(building_ptrs.begin(), building_ptrs.end(),
                  [](trader::Building* i,trader::Building* j){
                    return i->priority() < j->priority();
                  });
        trader::Inventory::Update inv_update;
        for (auto& ptr : building_ptrs) {
          if (CheckBuildingRequirementsMet(ptr->requires(), *trader_inventory)) {
            std::uniform_real_distribution<> consumption_chance(0, 1);
            auto final_inventory = trader_inventory->inv();
            for (auto& requirement : ptr->requires()) {
              if (requirement.chance() >= 1 || consumption_chance(rng_gen) < requirement.chance()) {
                //consume
                int actual = ConsumeItem(final_inventory[requirement.item().name()], requirement.quantity());
                consumption[requirement.item().name()] = actual;
              }
            }
            for (auto& result : ptr->produces()) {
              if (result.chance() >= 1 || consumption_chance(rng_gen) < result.chance()) {
                int actual = ProduceItem(final_inventory[result.item().name()], result.quantity(), QuerySpace(*trader_inventory));
                production[result.item().name()] = actual;
                overproduction[result.item().name()] = result.quantity() - actual; // overflow
              }
            }
            inv_update.set_inv(final_inventory);
            connection.SendComponentUpdate<trader::Inventory>(op.Request.sender_id(), inv_update);
            return {{(trader_inventory->cash() < 0), production, overproduction, consumption}};
          }
        }
        inv_update.set_cash(trader_inventory->cash() - trader_buildings->idle_tax());
        connection.SendComponentUpdate<trader::Inventory>(op.Request.sender_id(), inv_update);
        return {{(trader_inventory->cash() < 0), production, overproduction, consumption}};
    };
private:
    // SPATIALOS CONCEPTS
    bool ConstructInitialAuctionHouseEntity(worker::EntityId AH_id) {
      if (!IsConnected()) {
        return false;
      }
      worker::Entity AH_entity;

      AH_entity.Add<improbable::Metadata>({{"AuctionHouseEntity"}});
      AH_entity.Add<improbable::Persistence>({});
      AH_entity.Add<improbable::Position>({{1, 0, static_cast<double>(AH_id)}});
      // TODO: Create partition entity instead of using hardcoded snapshot one (id = 3)
      AH_entity.Add<improbable::AuthorityDelegation>({{{3020, 3}}}); //3 is the auction house partition entity
      AH_entity.Add<market::RegisterCommandComponent>({});
      AH_entity.Add<market::MakeOfferCommandComponent>({});
      AH_entity.Add<market::RequestProductionComponent>({});
      AH_entity.Add<market::RequestShutdownComponent>({});
      AH_entity.Add<market::DemographicInfo>({{},
                                              0,
                                              0.0});
      AH_entity.Add<market::FoodMarket>({{{
                                              "food",
                                              0.5,
                                              3010
                                          },
                                             {
                                                 10.0,
                                                 10.0,
                                                 0,
                                                 0,
                                                 0
                                             }}});
      AH_entity.Add<market::WoodMarket>({{{
                                              "wood",
                                              1,
                                              3011
                                          },
                                             {
                                                 3.0,
                                                 3.0,
                                                 0,
                                                 0,
                                                0
                                             }}});
      AH_entity.Add<market::FertilizerMarket>({{{
                                                    "fertilizer",
                                                    0.1,
                                                    3012
                                                },
                                                   {
                                                       11.0,
                                                       11.0,
                                                       0,
                                                       0,
                                                       0
                                                   }}});
      AH_entity.Add<market::OreMarket>({{{
                                             "ore",
                                             1,
                                             3013
                                         },
                                            {
                                                1.0,
                                                1.0,
                                                0,
                                                0,
                                                0
                                            }}});
      AH_entity.Add<market::MetalMarket>({{{
                                               "metal",
                                               1,
                                               3014
                                           },
                                              {
                                                  2.0,
                                                  2.0,
                                                  0,
                                                  0,
                                                  0
                                              }}});
      AH_entity.Add<market::ToolsMarket>({{{
                                               "tools",
                                               1,
                                               3015
                                           },
                                              {
                                                  5.0,
                                                  5.0,
                                                  0,
                                                  0,
                                                  0
                                              }}});
      auto result = connection.SendCreateEntityRequest(AH_entity, AH_id, {});
      if (!result) {
        connection.SendLogMessage(worker::LogLevel::kError, "AHWorker",
                                  "Failed to create new auction house: "+result.GetErrorMessage());
        return false;
      }
      connection.SendLogMessage(worker::LogLevel::kInfo, "AHWorker",
                                "Created new auction house with ID #"+std::to_string(AH_id));
      return true;
    }
    void MakeCallbacks() override {
      using RegisterTraderCommand = market::RegisterCommandComponent::Commands::RegisterCommand;
      using MakeBidOfferCommand = market::MakeOfferCommandComponent::Commands::MakeBidOffer;
      using MakeAskOfferCommand = market::MakeOfferCommandComponent::Commands::MakeAskOffer;
      using RequestShutdownCommand = market::RequestShutdownComponent::Commands::RequestShutdown;
      using RequestProductionCommand = market::RequestProductionComponent::Commands::RequestProduction;
      view.OnCommandRequest<RequestProductionCommand>(
          [&](const worker::CommandRequestOp<RequestProductionCommand>& op) {
            auto res = TickWorkerProduction(op);
            if (!res) {
              connection.SendCommandFailure<RequestProductionCommand>(op.RequestId, "Failed to tick production");
            }
            connection.SendCommandResponse<RequestProductionCommand>(op.RequestId, *res);
          });
      view.OnCommandRequest<RequestShutdownCommand>(
          [&](const worker::CommandRequestOp<RequestShutdownCommand>& op) {
            worker::EntityId entity_id = op.Request.entity_id();
            std::string role = RoleToString(op.Request.role());
            int age_ticks = op.Request.age_ticks();
            demographics[role] -= 1;
            num_deaths += 1;
            total_age += age_ticks;

            logger->Log(Log::INFO, "Deregistered trader "+std::to_string(entity_id));

            connection.SendCommandResponse<RequestShutdownCommand>(op.RequestId, {true});

            connection.SendDeleteEntityRequest(entity_id, {});
          });
      view.OnCommandRequest<RegisterTraderCommand>(
          [&](const worker::CommandRequestOp<RegisterTraderCommand>& op) {
            std::string req_type = "unknown";
            if (op.Request.type() == messages::AgentType::MONITOR) {
              req_type = "Monitor";
            } else if (op.Request.type() == messages::AgentType::AI_TRADER) {
              req_type = "AITrader";
            }else if (op.Request.type() == messages::AgentType::HUMAN_TRADER) {
              req_type = "HumanTrader";
            }
            connection.SendLogMessage(worker::LogLevel::kInfo, "AuctionHouse",
                                      "Received register request.\nCallerWorkerEntityId: " + std::to_string(op.CallerWorkerEntityId)
                                      + " for new trader of type: " + req_type);

          bool result = RegisterNewAgent(op);
          if (!result) {
            connection.SendCommandFailure<RegisterTraderCommand>(op.RequestId, "Auction House failed to create trader entities");
            connection.SendLogMessage(worker::LogLevel::kWarn, "AuctionHouse",
                                      "Failed to register new" + req_type +"trader with ID #" + std::to_string(result));
          }});

      view.OnCommandRequest<MakeBidOfferCommand>(
          [&](const worker::CommandRequestOp<MakeBidOfferCommand>& op) {
            std::cout << "Bid Offer received from #" << op.Request.sender_id() <<  std::endl;
            std::cout << op.Request.good() << ": " << op.Request.quantity() << "@" << op.Request.unit_price() << std::endl;

            BidOffer bid = {op.RequestId,
                            static_cast<int>(op.Request.sender_id()),
                            op.Request.good(),
                            op.Request.quantity(),
                            op.Request.unit_price(),
                            op.Request.expiry_time()};
            BidResult result = {static_cast<int>(op.Request.sender_id()),
                                op.Request.good(),
                                op.Request.unit_price()};
            bid_book_mutex.lock();
            bid_book[op.Request.good()].push_back({bid, result});
            bid_book_mutex.unlock();

            connection.SendCommandResponse<MakeBidOfferCommand>(op.RequestId, {true});
          });
      view.OnCommandRequest<MakeAskOfferCommand>(
          [&](const worker::CommandRequestOp<MakeAskOfferCommand>& op) {
            std::cout << "Ask Offer received from #" << op.Request.sender_id() <<  std::endl;
            std::cout << op.Request.good() << ": " << op.Request.quantity() << "@" << op.Request.unit_price() << std::endl;
            // Basic check for validity (more checking is done at resolution-time)
            if (op.Request.quantity() <= 0) {
              connection.SendCommandFailure<MakeAskOfferCommand>(op.RequestId, "Quantity offered must be > 0");
            }
            else if (op.Request.unit_price() <= 0) {
              connection.SendCommandFailure<MakeAskOfferCommand>(op.RequestId, "Unit price must be > 0");
            }
            else if (known_commodities.count(op.Request.good()) != 1) {
              connection.SendCommandFailure<MakeAskOfferCommand>(op.RequestId, "Unknown commodity: " + op.Request.good());
            } else {
              AskOffer ask = {op.RequestId,
                              static_cast<int>(op.Request.sender_id()),
                              op.Request.good(),
                              op.Request.quantity(),
                              op.Request.unit_price(),
                              op.Request.expiry_time()};
              AskResult result = {static_cast<int>(op.Request.sender_id()),
                                  op.Request.good()};
              ask_book_mutex.lock();
              ask_book[op.Request.good()].push_back({ask, result});
              ask_book_mutex.unlock();

              connection.SendCommandResponse<MakeAskOfferCommand>(op.RequestId, {true});
            }
          });
    }
    // Transaction functions
    bool CheckBidStake(BidOffer& offer) {
        if (offer.quantity < 0 || offer.unit_price <= 0) {
            logger->Log(Log::WARN, "Rejected nonsensical bid: " + offer.ToString());
            return false;
        }

        auto inv = view.Entities[offer.sender_id].Get<trader::Inventory>();
        if (!inv) {
          logger->Log(Log::WARN, "Missing entity for Bid stake: " + offer.ToString());
          return false;
        }
        if (!CheckTraderHasMoney(offer.quantity*offer.unit_price, *inv)) {
            logger->Log(Log::DEBUG, "Failed to take Bid stake: " + offer.ToString());
            return false;
        }
        return true;
    }
    bool CheckAskStake(AskOffer& offer) {
        if (offer.quantity < 0 || offer.unit_price <= 0) {
            logger->Log(Log::WARN, "Rejected nonsensical ask: " + offer.ToString());
            return false;
        }
        auto inv = view.Entities[offer.sender_id].Get<trader::Inventory>();
        if (!inv) {
          logger->Log(Log::WARN, "Missing entity for Ask stake: " + offer.ToString());
          return false;
        }
        if (!CheckTraderHasItem(offer.commodity, offer.quantity, *inv)) {
            logger->Log(Log::DEBUG, "Failed to take Ask stake: " + offer.ToString());
            return false;
        }
        return true;
    }
    void CloseBid(const BidOffer& bid, BidResult bid_result) {
        if (bid.quantity > 0) {
            // partially unfilled
            bid_result.UpdateWithNoTrade(bid.quantity);
        }
        SendResult(bid_result);
    }
    void CloseAsk(const AskOffer& ask, AskResult ask_result) {
        if (ask.quantity > 0) {
            // partially unfilled
            ask_result.UpdateWithNoTrade(ask.quantity);
        }
        SendResult(ask_result);
    }
    int TryTakeCommodity(int trader_id, const std::string& commodity, int quantity, bool atomic) {
        if (quantity <= 0) return 0;
        auto inv = view.Entities[trader_id].Get<trader::Inventory>();
        if (!inv) {
          return 0;
        }
        auto initial_inventory = inv->inv();
        if (initial_inventory.count(commodity) != 1) return 0;
        int available = initial_inventory[commodity].quantity();
        if (available < quantity && atomic) return 0;

        int actual_taken = std::min(available, quantity);
        initial_inventory[commodity].set_quantity( available - actual_taken);
        trader::Inventory::Update inv_update;
        inv_update.set_inv(initial_inventory);
        connection.SendComponentUpdate<trader::Inventory>(trader_id, inv_update, {});
        return actual_taken;
    }
    double TryTakeMoney(int trader_id, double quantity, bool atomic) {
      if (quantity <= 0) return 0;
      auto inv = view.Entities[trader_id].Get<trader::Inventory>();
      if (!inv) {
        return 0;
      }

      double available = inv->cash();
      if (available < quantity && atomic) return 0;

      double actual_taken = std::min(available, quantity);
      trader::Inventory::Update inv_update;
      inv_update.set_cash(available - actual_taken);
      connection.SendComponentUpdate<trader::Inventory>(trader_id, inv_update, {});
      return actual_taken;
    }
    int TryAddCommodity(int trader_id, const std::string& commodity, int quantity, bool atomic) {
      if (quantity <= 0) return 0;
      auto inv = view.Entities[trader_id].Get<trader::Inventory>();
      if (!inv) {
        return 0;
      }

      int actual_added = std::min((int) std::floor(QuerySpace(*inv) / known_commodities[commodity].size), quantity);
      auto initial_inventory = inv->inv();
      initial_inventory[commodity].set_quantity( initial_inventory[commodity].quantity() + actual_added);
      trader::Inventory::Update inv_update;
      inv_update.set_inv(initial_inventory);
      connection.SendComponentUpdate<trader::Inventory>(trader_id, inv_update, {});
      return actual_added;
    }
    void AddMoney(int trader_id, double quantity) {
      if (quantity <= 0) return;
      auto inv = view.Entities[trader_id].Get<trader::Inventory>();
      if (!inv) {
        return;
      }
      trader::Inventory::Update inv_update;
      inv_update.set_cash(inv->cash() + quantity);
      connection.SendComponentUpdate<trader::Inventory>(trader_id, inv_update, {});
      return;
    }
    // 0 - success
    // 1 - seller failed
    // 2 - buyer failed
    int MakeTransaction(const std::string& commodity, int buyer, int seller, int quantity, double clearing_price) {
        // TODO: Instead of making many component updates, they could all be combined into just two net deltas
        // take from seller
        auto actual_quantity = TryTakeCommodity(seller, commodity, quantity, true);
        if (actual_quantity == 0) {
            // this may be unrecoverable, not sure
            logger->Log(Log::WARN, "Seller lacks good! Aborting trade");
            return 1;
        }
        auto actual_money = TryTakeMoney(buyer, actual_quantity*clearing_price, true);
        if (actual_money == 0) {
            // this may be unrecoverable, not sure
            logger->Log(Log::ERROR, "Buyer lacks money! Aborting trade");
            return 2;
        }
        TryAddCommodity(buyer, commodity, actual_quantity, false);
        //take sales tax from seller
        double profit = actual_quantity*clearing_price;
        AddMoney(seller, profit*(1-SALES_TAX));
        spread_profit += profit*SALES_TAX;

        auto info_msg = std::string("Made trade: ") + std::to_string(seller) + std::string(" >>> ") + std::to_string(buyer) + std::string(" : ") + commodity + std::string(" x") + std::to_string(quantity) + std::string(" @ $") + std::to_string(clearing_price);
        logger->Log(Log::INFO, info_msg);
        return 0;
    }

    void TakeBrokerFee(BidOffer& offer, BidResult& result) {
        double fee = offer.quantity*offer.unit_price*BROKER_FEE;
        auto res = TryTakeMoney(offer.sender_id, fee, true);
        if (res > 0) {
            spread_profit += fee;
            result.broker_fee_paid = true;
        } else {
            //failed to take broker fee
            return;
        }
    }
    void TakeBrokerFee(AskOffer& offer, AskResult& result) {
        double fee = offer.quantity*offer.unit_price*BROKER_FEE;
        auto res = TryTakeMoney(offer.sender_id, fee, true);
        if (res > 0) {
            spread_profit += fee;
            result.broker_fee_paid = true;
        } else {
            //failed to take broker fee
            return;
        }
    }

    bool ValidateBid(BidOffer& curr_bid, BidResult& bid_result, std::int64_t resolve_time) {
        if (curr_bid.expiry_ms == 0) {
            curr_bid.expiry_ms = 1;
            bid_result.broker_fee_paid = true; //dont need to pay broker fees for immediate offers
        } else if (curr_bid.expiry_ms < resolve_time) {
            return false; //expired bid
        }

        if (!bid_result.broker_fee_paid) {
            TakeBrokerFee(curr_bid, bid_result);
        }
        return (bid_result.broker_fee_paid && CheckBidStake(curr_bid));
    }

    bool ValidateAsk(AskOffer& curr_ask, AskResult& ask_result, std::int64_t resolve_time) {
        if (curr_ask.expiry_ms == 0) {
            curr_ask.expiry_ms = 1;
            ask_result.broker_fee_paid = true; //dont need to pay broker fees for immediate offers
        } else if (curr_ask.expiry_ms < resolve_time) {
            return false; //expired bid
        }

        if (!ask_result.broker_fee_paid) {
            TakeBrokerFee(curr_ask, ask_result);
        }
        return (ask_result.broker_fee_paid && CheckAskStake(curr_ask));
    }

    void ResolveOffers(const std::string& commodity) {
        bid_book_mutex.lock();
        ask_book_mutex.lock();

        std::vector<std::pair<BidOffer, BidResult>> retained_bids = {};
        std::vector<std::pair<AskOffer, AskResult>> retained_asks = {};

        auto resolve_time = to_unix_timestamp_ms(std::chrono::system_clock::now());

        auto& bids = bid_book[commodity];
        auto& asks = ask_book[commodity];
//
//        std::shuffle(bids.begin(), bids.end(), rng_gen);
//        std::shuffle(bids.begin(), bids.end(), rng_gen);

        std::sort(bids.rbegin(), bids.rend()); // NOTE: Reversed order
        std::sort(asks.rbegin(), asks.rend());   // lowest selling price first

        int num_bids = bids.size();
        int num_asks = asks.size();
        int num_trades_this_tick = 0;
        double money_traded_this_tick = 0;
        double units_traded_this_tick = 0;

        double avg_buy_price_this_tick = 0;
        double avg_price_this_tick = 0;

        double supply = 0;
        double demand = 0;
        {
            auto it = bids.begin();
            while (it != bids.end()) {
                if (!ValidateBid(it->first, it->second, resolve_time)) {
                    CloseBid(it->first, std::move(it->second));
                    bids.erase(it);
                }
                else  {
                    demand += it->first.quantity;
                    ++it;
                }
            }
        }
        {
            auto it = asks.begin();
            while (it != asks.end()) {
                if (!ValidateAsk(it->first, it->second, resolve_time)) {
                    CloseAsk(it->first, std::move(it->second));
                    asks.erase(it);
                }
                else  {
                    supply += it->first.quantity;
                    ++it;
                }
            }
        }
        while (!bids.empty() && !asks.empty()) {
            BidOffer& curr_bid = bids[0].first;
            AskOffer& curr_ask = asks[0].first;

            BidResult& bid_result = bids[0].second;
            AskResult& ask_result = asks[0].second;

            if (curr_ask.unit_price > curr_bid.unit_price) {
                break;
            }

            int quantity_traded = std::min(curr_bid.quantity, curr_ask.quantity);
            double clearing_price = curr_ask.unit_price;

            if (quantity_traded > 0) {
                // MAKE TRANSACTION
                int buyer = curr_bid.sender_id;
                int seller = curr_ask.sender_id;
                auto res = MakeTransaction(commodity, buyer, seller, quantity_traded, clearing_price);
                if (res == 1) {
                    //seller failed
                    CloseAsk(curr_ask, std::move(ask_result));
                    asks.erase(asks.begin());
                    break;
                }
                if (res == 2) {
                    //buyer failed
                    CloseBid(curr_bid, std::move(bid_result));
                    bids.erase(bids.begin());
                    break;
                }
                // update the offers and results
                curr_bid.quantity -= quantity_traded;
                curr_ask.quantity -= quantity_traded;

                bid_result.UpdateWithTrade(quantity_traded, clearing_price);
                ask_result.UpdateWithTrade(quantity_traded, clearing_price);

                // update per-tick metrics
                avg_price_this_tick = (avg_price_this_tick*units_traded_this_tick + clearing_price*quantity_traded)/(units_traded_this_tick + quantity_traded);
                avg_buy_price_this_tick = (avg_buy_price_this_tick*units_traded_this_tick + curr_bid.unit_price*quantity_traded)/(units_traded_this_tick + quantity_traded);

                units_traded_this_tick += quantity_traded;
                money_traded_this_tick += quantity_traded*clearing_price;
                num_trades_this_tick += 1;
            }

            if (curr_bid.quantity <= 0) {
                // Fulfilled buy order
                CloseBid(curr_bid, std::move(bid_result));
                bids.erase(bids.begin());
            }
            if (curr_ask.quantity <= 0) {
                // Fulfilled sell order
                CloseAsk(curr_ask, std::move(ask_result));
                asks.erase(asks.begin());
            }
        }

        for (auto bid : bids) {
            //optionally could return a partial-result here
            retained_bids.emplace_back(std::move(bid));
        }
        for (auto ask : asks) {
            retained_asks.emplace_back(std::move(ask));
        }
        bid_book[commodity] = std::move(retained_bids);
        ask_book[commodity] = std::move(retained_asks);
        // update history
        history.asks.add(commodity, supply);
        history.bids.add(commodity, demand);
        history.net_supply.add(commodity, supply-demand);
        history.trades.add(commodity, num_trades_this_tick);

        if (units_traded_this_tick > 0) {
            history.buy_prices.add(commodity, avg_buy_price_this_tick);
            history.prices.add(commodity, avg_price_this_tick);
        } else {
            // Set to same as last-tick's average if no trades occurred
            history.buy_prices.add(commodity, history.buy_prices.average(commodity, 1));
            history.prices.add(commodity, history.prices.average(commodity, 1));
        }
        logger->Log(Log::INFO, std::to_string(num_trades_this_tick) + " trades resolved from " + std::to_string(num_asks) + "/" + std::to_string(num_bids) + " asks/bids");

    bid_book_mutex.unlock();
    ask_book_mutex.unlock();
    }

    int RegisterNewAgent(const worker::CommandRequestOp<market::RegisterCommandComponent::Commands::RegisterCommand>& op) {
      using AssignPartitionCommand = improbable::restricted::Worker::Commands::AssignPartition;
      // Set parameters
      std::int32_t GetOpListTimeout = 500; // all times in ms unless stated otherwise
      std::int32_t WhileLoopTimeout = 500;

      worker::EntityId entity_id;
      ah::RegisterProgress progress = ah::NONE;
      messages::AIRole assigned_role = messages::AIRole::NONE;

      // Set callbacks
      {
        view.OnReserveEntityIdsResponse([&](const worker::ReserveEntityIdsResponseOp& op) {
          if (op.StatusCode == worker::StatusCode::kSuccess) {
            entity_id = *op.FirstEntityId;
            progress = ah::RESERVED_ID;
          } else {
            connection.SendLogMessage(
                worker::LogLevel::kError, unique_name,
                "Failed to reserve ID(s): error code : " +
                std::to_string(static_cast<std::uint8_t>(op.StatusCode)) +
                " message: " + op.Message);
          }
        });
        view.OnCreateEntityResponse([&](const worker::CreateEntityResponseOp& op) {
          if (op.StatusCode == worker::StatusCode::kSuccess) {
            connection.SendLogMessage(worker::LogLevel::kInfo, unique_name,
                                      "Successfully created entity");
            progress = ah::CREATED_ENTITY;
          } else {
            connection.SendLogMessage(worker::LogLevel::kWarn, unique_name,
                                      "Failed to create entity");
          };
        });
        view.OnCommandResponse<AssignPartitionCommand>(
            [&](const worker::CommandResponseOp<AssignPartitionCommand>& op) {
              if (op.StatusCode == worker::StatusCode::kSuccess) {
                connection.SendLogMessage(worker::LogLevel::kInfo, unique_name,
                                          "Successfully assigned partition.");
                progress = ah::ASSIGNED_PARTITION;
              } else {
                connection.SendLogMessage(
                    worker::LogLevel::kError, unique_name,
                    "Failed to assign partition: error code : " +
                        std::to_string(static_cast<std::uint8_t>(op.StatusCode)) +
                        " message: " + op.Message);
              }
            });
      }
      // Start
      connection.SendReserveEntityIdsRequest(1, {});
      //Wait for entity ID to be reserved
      auto now_time = to_unix_timestamp_ms(std::chrono::high_resolution_clock::now());
      do {
        view.Process(connection.GetOpList(GetOpListTimeout));
        if (progress == ah::RESERVED_ID) {
          // CREATE ENTITY HERE
          switch (op.Request.type()) {
          case messages::AgentType::MONITOR:
            CreateMonitorEntity(entity_id);
            break;
          case messages::AgentType::AI_TRADER:
            assigned_role = CreateAITraderEntity(entity_id, op.Request.requested_role());
            break;
          case messages::AgentType::HUMAN_TRADER:
            break;
          default:
            return false;
          }
        }
        if (to_unix_timestamp_ms(std::chrono::high_resolution_clock::now()) - now_time > WhileLoopTimeout) return 0;
      } while (progress != ah::RESERVED_ID);

      // Wait for create entity to be processed, then request partition assignment
      now_time = to_unix_timestamp_ms(std::chrono::high_resolution_clock::now());
      do {
        view.Process(connection.GetOpList(GetOpListTimeout));
        if (progress == ah::CREATED_ENTITY) {
          connection.SendCommandRequest<AssignPartitionCommand>(
              op.CallerWorkerEntityId, {entity_id}, /* default timeout */ {});
        }
        if (to_unix_timestamp_ms(std::chrono::high_resolution_clock::now()) - now_time > WhileLoopTimeout) return 0;
      } while(progress != ah::CREATED_ENTITY);

      // Wait for partition assignment to go through
      now_time = to_unix_timestamp_ms(std::chrono::high_resolution_clock::now());
      do {
        view.Process(connection.GetOpList(GetOpListTimeout));
        if (to_unix_timestamp_ms(std::chrono::high_resolution_clock::now()) - now_time > WhileLoopTimeout) return 0;
      } while(progress != ah::ASSIGNED_PARTITION);

      // Send successful response
      messages::RegisterResponse req_res;
      req_res.set_entity_id(entity_id);
      req_res.set_assigned_role(assigned_role);
      worker::List<commodity::Commodity> commodities;
      for (auto& good : known_commodities) {
        commodity::Commodity comm{
            good.second.name,
            good.second.size,
            good.second.market_component_id};
        commodities.emplace_back(comm);
      }
      req_res.set_listed_items(commodities);
      using RegisterTraderCommand = market::RegisterCommandComponent::Commands::RegisterCommand;
      connection.SendCommandResponse<RegisterTraderCommand>(op.RequestId, req_res);
      connection.SendLogMessage(worker::LogLevel::kInfo, "AuctionHouse",
      "Registered new" + RoleToString(assigned_role) +"trader with ID #" + std::to_string(entity_id));
      return true;
    }

  void CreateMonitorEntity(worker::EntityId monitor_entity_id) {
    worker::Entity monitor_entity;

    // Market component IDs start at 3010 and increment to 3015
    std::vector<improbable::ComponentSetInterest_Query> all_queries;
    for (auto& good : known_commodities) {
      improbable::ComponentSetInterest_QueryConstraint my_constraint;
      my_constraint.set_component_constraint(
          {3001});  // only markets have this MakeOfferCommandComponent
      improbable::ComponentSetInterest_Query my_query;
      my_query.set_constraint(my_constraint);

      my_query.set_result_component_id(
          {static_cast<unsigned int>(good.second.market_component_id)});
      all_queries.push_back(my_query);
    }
    {
      // Also have interest in Demographics component
      improbable::ComponentSetInterest_QueryConstraint my_constraint;
      my_constraint.set_component_constraint(
          {3016});  // only markets have this MakeOfferCommandComponent
      improbable::ComponentSetInterest_Query my_query;
      my_query.set_constraint(my_constraint);

      my_query.set_result_component_id({3016});  // DemographicInfo component Id
      all_queries.push_back(my_query);
    }

    worker::List<improbable::ComponentSetInterest_Query> const_queries;
    const_queries.insert(const_queries.begin(), all_queries.begin(), all_queries.end());

    improbable::ComponentSetInterest all_markets_interest;
    all_markets_interest.set_queries(const_queries);

    monitor_entity.Add<improbable::Metadata>({{"MonitorEntity"}});
    monitor_entity.Add<improbable::Position>({{2, 0, static_cast<double>(monitor_entity_id)}});
    monitor_entity.Add<improbable::Interest>({{{50, all_markets_interest}}});

    monitor_entity.Add<improbable::AuthorityDelegation>({{{50, monitor_entity_id}}});
    connection.SendCreateEntityRequest(monitor_entity, monitor_entity_id, {});
  }

  messages::AIRole CreateAITraderEntity(worker::EntityId trader_entity_id, messages::AIRole requested_role) {
    worker::Entity trader_entity;
    if (requested_role == messages::AIRole::NONE) {
      requested_role = ChooseNewClassWeighted();
    }
    switch (requested_role) {
    case messages::AIRole::FARMER:
      AddFarmerComponents(trader_entity, trader_entity_id);
      break;
    case messages::AIRole::WOODCUTTER:
      AddWoodcutterComponents(trader_entity, trader_entity_id);
      break;
    case messages::AIRole::COMPOSTER:
      AddComposterComponents(trader_entity, trader_entity_id);
      break;
    case messages::AIRole::MINER:
      AddMinerComponents(trader_entity, trader_entity_id);
      break;
    case messages::AIRole::REFINER:
      AddRefinerComponents(trader_entity, trader_entity_id);
      break;
    case messages::AIRole::BLACKSMITH:
      AddBlacksmithComponents(trader_entity, trader_entity_id);
      break;
    default:
      return messages::AIRole::NONE;
    }
    trader_entity.Add<improbable::Metadata>({{RoleToString(requested_role) + std::to_string(trader_entity_id)}});
    trader_entity.Add<improbable::Position>({{3, 0, static_cast<double>(trader_entity_id)}});

    trader_entity.Add<improbable::AuthorityDelegation>({{{4005, trader_entity_id}, {4004, 3}}}); // The AH partition entity is hardcoded to 3
    connection.SendCreateEntityRequest(trader_entity, trader_entity_id, {});

    return requested_role;
  }

  void AddFarmerComponents(worker::Entity& trader_entity, int entity_id) {
    // Interest for auction house markets
    improbable::ComponentSetInterest_QueryConstraint market_constraint;
    market_constraint.set_component_constraint({3001});  // only markets have this MakeOfferCommandComponent
    improbable::ComponentSetInterest_Query market_query;
    market_query.set_constraint(market_constraint).set_result_component_set_id({3021});


    // add interest for own inventory & buildings
    improbable::ComponentSetInterest_QueryConstraint self_constraint;
    self_constraint.set_entity_id_constraint(entity_id);
    improbable::ComponentSetInterest_Query self_query;
    self_query.set_constraint(self_constraint).set_result_component_set_id({4006}); //Inventory + AIBuildings component
    worker::List<improbable::ComponentSetInterest_Query> const_queries = {market_query, self_query};

    improbable::ComponentSetInterest trader_interest;
    trader_interest.set_queries(const_queries);
    trader_entity.Add<improbable::Interest>({{{4005, trader_interest}}});

    // Create production rules
    // 1 fert + 1 tool (10% break change) + 1 wood = 6 food
    trader::Building farm1 = {{{ToSchemaCommodity(known_commodities["food"]), 6, 1.0}},
                              {{ToSchemaCommodity(known_commodities["fertilizer"]), 1, 1.0},
                               {ToSchemaCommodity(known_commodities["tools"]), 1, 0.1},
                               {ToSchemaCommodity(known_commodities["wood"]), 1, 1}},
                              1,
                              "AIFarm1", false};
    // 1 fert + 1 wood = 3 food
    trader::Building farm2 = {{{ToSchemaCommodity(known_commodities["food"]), 3, 1.0}},
                              {{ToSchemaCommodity(known_commodities["fertilizer"]), 1, 1.0},
                               {ToSchemaCommodity(known_commodities["wood"]), 1, 1}},
                              2,
                              "AIFarm2", false};
    // 1 fert = 1 food
    trader::Building farm3 = {{{ToSchemaCommodity(known_commodities["food"]), 1, 1.0}},
                              {{ToSchemaCommodity(known_commodities["fertilizer"]), 1, 1.0}},
                              3,
                              "AIFarm3", false};
    trader_entity.Add<trader::AIBuildings>({{farm1, farm2, farm3}, 20});
    // Add starting inventory
    ::worker::Map<std::string, ::trader::InventoryItem> starting_inv = {
        {"food", {known_commodities["food"].size, 0}},
        {"tools", {known_commodities["tools"].size, 1}},
        {"wood", {known_commodities["wood"].size, 1}},
        {"fertilizer", {known_commodities["fertilizer"].size, 1}}
    };
    trader_entity.Add<trader::Inventory>({500, starting_inv, 20});

  }

  void AddWoodcutterComponents(worker::Entity& trader_entity, int entity_id) {
    // Interest for auction house markets
    improbable::ComponentSetInterest_QueryConstraint market_constraint;
    market_constraint.set_component_constraint({3001});  // only markets have this MakeOfferCommandComponent
    improbable::ComponentSetInterest_Query market_query;
    market_query.set_constraint(market_constraint).set_result_component_set_id({3022});


    // add interest for own inventory & buildings
    improbable::ComponentSetInterest_QueryConstraint self_constraint;
    self_constraint.set_entity_id_constraint(entity_id);
    improbable::ComponentSetInterest_Query self_query;
    self_query.set_constraint(self_constraint).set_result_component_set_id({4006}); //Inventory + AIBuildings component
    worker::List<improbable::ComponentSetInterest_Query> const_queries = {market_query, self_query};

    improbable::ComponentSetInterest trader_interest;
    trader_interest.set_queries(const_queries);
    trader_entity.Add<improbable::Interest>({{{4005, trader_interest}}});

    // Create production rules
    // 1 food + 1 tool (10% break change) = 2 wood
    trader::Building lumberyard1 = {{{ToSchemaCommodity(known_commodities["wood"]), 2, 1.0}},
                              {{ToSchemaCommodity(known_commodities["tools"]), 1, 0.1},
                               {ToSchemaCommodity(known_commodities["food"]), 1, 1}},
                              1,
                              "AILumberyard1", false};
    // 1 food = 1 wood
    trader::Building lumberyard2 = {{{ToSchemaCommodity(known_commodities["wood"]), 1, 1.0}},
                              {{ToSchemaCommodity(known_commodities["food"]), 1, 1}},
                              2,
                              "AILumberyard2", false};
    trader_entity.Add<trader::AIBuildings>({{lumberyard1, lumberyard2}, 20});
    // Add starting inventory
    ::worker::Map<std::string, ::trader::InventoryItem> starting_inv = {
        {"food", {known_commodities["food"].size, 1}},
        {"tools", {known_commodities["tools"].size, 1}},
        {"wood", {known_commodities["wood"].size, 0}},
    };
    trader_entity.Add<trader::Inventory>({500, starting_inv, 20});
  }

  void AddComposterComponents(worker::Entity& trader_entity, int entity_id) {
    // Interest for auction house markets
    improbable::ComponentSetInterest_QueryConstraint market_constraint;
    market_constraint.set_component_constraint({3001});  // only markets have this MakeOfferCommandComponent
    improbable::ComponentSetInterest_Query market_query;
    market_query.set_constraint(market_constraint).set_result_component_set_id({3023});


    // add interest for own inventory & buildings
    improbable::ComponentSetInterest_QueryConstraint self_constraint;
    self_constraint.set_entity_id_constraint(entity_id);
    improbable::ComponentSetInterest_Query self_query;
    self_query.set_constraint(self_constraint).set_result_component_set_id({4006}); //Inventory + AIBuildings component
    worker::List<improbable::ComponentSetInterest_Query> const_queries = {market_query, self_query};

    improbable::ComponentSetInterest trader_interest;
    trader_interest.set_queries(const_queries);
    trader_entity.Add<improbable::Interest>({{{4005, trader_interest}}});

    // Create production rules
    // 1 food  = 1 fert (50% succeed chance)
    trader::Building composter1 = {{{ToSchemaCommodity(known_commodities["fertilizer"]), 1, 0.5}},
                              {{ToSchemaCommodity(known_commodities["food"]), 1, 1}},
                              1,
                              "AIComposter1", false};

    trader_entity.Add<trader::AIBuildings>({{composter1}, 20});
    // Add starting inventory
    ::worker::Map<std::string, ::trader::InventoryItem> starting_inv = {
        {"food", {known_commodities["food"].size, 1}},
        {"fertilizer", {known_commodities["fertilizer"].size, 0}}
    };
    trader_entity.Add<trader::Inventory>({500, starting_inv, 20});
  }

  void AddMinerComponents(worker::Entity& trader_entity, int entity_id) {
    // Interest for auction house markets
    improbable::ComponentSetInterest_QueryConstraint market_constraint;
    market_constraint.set_component_constraint({3001});  // only markets have this MakeOfferCommandComponent
    improbable::ComponentSetInterest_Query market_query;
    market_query.set_constraint(market_constraint).set_result_component_set_id({3024});


    // add interest for own inventory & buildings
    improbable::ComponentSetInterest_QueryConstraint self_constraint;
    self_constraint.set_entity_id_constraint(entity_id);
    improbable::ComponentSetInterest_Query self_query;
    self_query.set_constraint(self_constraint).set_result_component_set_id({4006}); //Inventory + AIBuildings component
    worker::List<improbable::ComponentSetInterest_Query> const_queries = {market_query, self_query};

    improbable::ComponentSetInterest trader_interest;
    trader_interest.set_queries(const_queries);
    trader_entity.Add<improbable::Interest>({{{4005, trader_interest}}});

    // Create production rules
    // 1 food + 1 tools  = 4 ore
    trader::Building mine1 = {{{ToSchemaCommodity(known_commodities["ore"]), 4, 1}},
                                   {{ToSchemaCommodity(known_commodities["food"]), 1, 1},
                                    {ToSchemaCommodity(known_commodities["tools"]), 1, 0.1}},
                                   1,
                                   "AIMine1", false};
    // 1 food = 2 ore
    trader::Building mine2 = {{{ToSchemaCommodity(known_commodities["ore"]), 2, 1}},
                              {{ToSchemaCommodity(known_commodities["food"]), 1, 1}},
                              2,
                              "AIMine2", false};

    trader_entity.Add<trader::AIBuildings>({{mine1, mine2}, 20});
    // Add starting inventory
    ::worker::Map<std::string, ::trader::InventoryItem> starting_inv = {
        {"food", {known_commodities["food"].size, 1}},
        {"tools", {known_commodities["tools"].size, 1}},
        {"ore", {known_commodities["ore"].size, 0}},
    };
    trader_entity.Add<trader::Inventory>({500, starting_inv, 20});
  }

  void AddRefinerComponents(worker::Entity& trader_entity, int entity_id) {
    // Interest for auction house markets
    improbable::ComponentSetInterest_QueryConstraint market_constraint;
    market_constraint.set_component_constraint({3001});  // only markets have this MakeOfferCommandComponent
    improbable::ComponentSetInterest_Query market_query;
    market_query.set_constraint(market_constraint).set_result_component_set_id({3025});


    // add interest for own inventory & buildings
    improbable::ComponentSetInterest_QueryConstraint self_constraint;
    self_constraint.set_entity_id_constraint(entity_id);
    improbable::ComponentSetInterest_Query self_query;
    self_query.set_constraint(self_constraint).set_result_component_set_id({4006}); //Inventory + AIBuildings component
    worker::List<improbable::ComponentSetInterest_Query> const_queries = {market_query, self_query};

    improbable::ComponentSetInterest trader_interest;
    trader_interest.set_queries(const_queries);
    trader_entity.Add<improbable::Interest>({{{4005, trader_interest}}});

    // Create production rules
    // 1 food + 1 ore + 1 tools  = 1 metal [REPEATABLE]
    trader::Building smelter1 = {{{ToSchemaCommodity(known_commodities["metal"]), 1, 1}},
                              {{ToSchemaCommodity(known_commodities["food"]), 1, 1},
                                  {ToSchemaCommodity(known_commodities["ore"]), 1, 1},
                               {ToSchemaCommodity(known_commodities["tools"]), 1, 0.1}},
                              1,
                              "AISmelter1", true};
    // 1 food + 2 ore = 2 metal
    trader::Building smelter2 = {{{ToSchemaCommodity(known_commodities["metal"]), 2, 1}},
                              {{ToSchemaCommodity(known_commodities["food"]), 1, 1},
                               {ToSchemaCommodity(known_commodities["ore"]), 2, 1}},
                              2,
                              "AISmelter2", false};
    // 1 food + 1 ore = 1 metal
    trader::Building smelter3 = {{{ToSchemaCommodity(known_commodities["metal"]), 1, 1}},
                                 {{ToSchemaCommodity(known_commodities["food"]), 1, 1},
                                  {ToSchemaCommodity(known_commodities["ore"]), 1, 1}},
                                 3,
                                 "AISmelter3", false};
    trader_entity.Add<trader::AIBuildings>({{smelter1, smelter2, smelter3}, 20});
    // Add starting inventory
    ::worker::Map<std::string, ::trader::InventoryItem> starting_inv = {
        {"food", {known_commodities["food"].size, 1}},
        {"tools", {known_commodities["tools"].size, 1}},
        {"ore", {known_commodities["ore"].size, 1}},
        {"metal", {known_commodities["metal"].size, 0}}
    };
    trader_entity.Add<trader::Inventory>({500, starting_inv, 20});
  }

  void AddBlacksmithComponents(worker::Entity& trader_entity, int entity_id) {
    // Interest for auction house markets
    improbable::ComponentSetInterest_QueryConstraint market_constraint;
    market_constraint.set_component_constraint({3001});  // only markets have this MakeOfferCommandComponent
    improbable::ComponentSetInterest_Query market_query;
    market_query.set_constraint(market_constraint).set_result_component_set_id({3026});


    // add interest for own inventory & buildings
    improbable::ComponentSetInterest_QueryConstraint self_constraint;
    self_constraint.set_entity_id_constraint(entity_id);
    improbable::ComponentSetInterest_Query self_query;
    self_query.set_constraint(self_constraint).set_result_component_set_id({4006}); //Inventory + AIBuildings component

    worker::List<improbable::ComponentSetInterest_Query> const_queries = {market_query, self_query};

    improbable::ComponentSetInterest trader_interest;
    trader_interest.set_queries(const_queries);
    trader_entity.Add<improbable::Interest>({{{4005, trader_interest}}});

    // Create production rules
    // 1 food + 1 metal  = 1 tools [REPEATABLE]
    trader::Building forge1 = {{{ToSchemaCommodity(known_commodities["tools"]), 1, 1}},
                                 {{ToSchemaCommodity(known_commodities["food"]), 1, 1},
                                  {ToSchemaCommodity(known_commodities["metal"]), 1, 1}},
                                 1,
                                 "AIForge1", true};

    trader_entity.Add<trader::AIBuildings>({{forge1}, 20});
    // Add starting inventory
    ::worker::Map<std::string, ::trader::InventoryItem> starting_inv = {
        {"food", {known_commodities["food"].size, 1}},
        {"tools", {known_commodities["tools"].size, 0}},
        {"metal", {known_commodities["metal"].size, 1}},
    };
    trader_entity.Add<trader::Inventory>({500, starting_inv, 20});
  }

  void SendResult(AskResult& result) {
    messages::AskResult msg = {
        result.commodity,
        result.quantity_traded,
        result.quantity_untraded,
        result.avg_price,
        result.broker_fee_paid
    };
    using ReportAskOffer = trader::ReportOfferResultComponent::Commands::ReportAskOffer;
    logger->Log(Log::INFO, "Sending ask result: " + result.ToString());
    connection.SendCommandRequest<ReportAskOffer>(result.sender_id, msg, {});
  }
  void SendResult(BidResult& result) {
    messages::BidResult msg = {
        result.commodity,
        result.quantity_traded,
        result.quantity_untraded,
        result.bought_price,
        result.broker_fee_paid
    };
    using ReportBidOffer = trader::ReportOfferResultComponent::Commands::ReportBidOffer;
    logger->Log(Log::INFO, "Sending bid result: " + result.ToString());
    connection.SendCommandRequest<ReportBidOffer>(result.sender_id, msg, {});
  }
};

#endif//CPPBAZAARBOT_AUCTION_HOUSE_H
