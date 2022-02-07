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

class AuctionHouse : public Agent {
public:
    History history;
    std::atomic_bool destroyed = false;
    
    std::string unique_name;
private:
    // debug info
    int num_deaths;
    int total_age;

    int TICK_TIME_MS = 10; //ms
    std::atomic<bool> queue_active = true;
    std::thread message_thread;

    std::mutex bid_book_mutex;
    std::mutex ask_book_mutex;
//    std::mutex known_traders_mutex;

    int MAX_PROCESSED_MESSAGES_PER_FLUSH = 800;
    double SALES_TAX = 0.08;
    double BROKER_FEE = 0.03;
    int ticks = 0;
//    std::mt19937 rng_gen = std::mt19937(std::random_device()());
    std::map<std::string, Commodity> known_commodities;
    std::map<int, std::shared_ptr<Trader>> known_traders;  //key = trader-id
    std::map<std::string, int> demographics = {};

    std::map<std::string, std::vector<std::pair<BidOffer, BidResult>>> bid_book = {};
    std::map<std::string, std::vector<std::pair<AskOffer, AskResult>>> ask_book = {};
    FileLogger logger;

public:
    double spread_profit = 0;
    AuctionHouse(worker::Connection& connection, worker::View& view, int auction_house_id, Log::LogLevel verbosity)
        : Agent(auction_house_id, connection, view)
        , unique_name(std::string("AH")+std::to_string(id))
        , logger(FileLogger(verbosity, unique_name)) {
        ConstructInitialAuctionHouseEntity(auction_house_id);
        message_thread = std::thread([this] { MessageLoop(); });
    }

    ~AuctionHouse() override {
        logger.Log(Log::DEBUG, "Destroying auction house");
        ShutdownMessageThread();
        known_traders.clear();
    }
    int GetNumTraders() const {
        return (int) known_traders.size();
    }

    std::pair<double, std::map<std::string, int>> GetDemographics() const {
        return {(num_deaths > 0) ? total_age / num_deaths : 0, demographics};
    }

    void MessageLoop() {
        while (true) {
            if (!queue_active) {
                return;
            }
            FlushInbox();
            FlushOutbox();
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }

    void Shutdown(){
        destroyed = true;
    }

    void ShutdownMessageThread() {
        queue_active = false;
        if (message_thread.joinable()) {
            message_thread.join();
        }
        logger.Log(Log::INFO, "Message thread shutdown");
        // Now message thread is gone we can safely send shutdown commands via the main thread
        auto shutdown_command = Message(id).AddShutdownCommand({id});
        for (auto& recipient : known_traders) {
            known_traders[recipient.first]->ReceiveMessage(*shutdown_command);
        }
    }

    void SendDirect(Message outgoing_message, std::shared_ptr<Agent>& recipient) {
        logger.Log(Log::WARN, "Using SendDirect method to reach unregistered trader");
        logger.LogSent(recipient->id, Log::DEBUG, outgoing_message.ToString());
        recipient->ReceiveMessage(std::move(outgoing_message));
    }
    void FlushOutbox() {
        logger.Log(Log::DEBUG, "Flushing outbox");
        auto outgoing = outbox.pop();
        int num_processed = 0;
        while (outgoing && num_processed < MAX_PROCESSED_MESSAGES_PER_FLUSH) {
            if (known_traders.find(outgoing->first) == known_traders.end()) {
                logger.Log(Log::DEBUG, "Failed to send message, unknown recipient " + std::to_string(outgoing->first));
            } else {
                logger.LogSent(outgoing->first, Log::DEBUG, outgoing->second.ToString());
                known_traders[outgoing->first]->ReceiveMessage(std::move(outgoing->second));
            }
            num_processed++;
            outgoing = outbox.pop();
        }
        if (num_processed == MAX_PROCESSED_MESSAGES_PER_FLUSH) {
            logger.Log(Log::WARN, "Outbox not fully flushed (tick "+std::to_string(ticks)+", " + std::to_string(inbox.size())+ " remaining)");
        }
        logger.Log(Log::DEBUG, "Flush finished (sent " + std::to_string(num_processed)+")");
    }
    void FlushInbox() {
        logger.Log(Log::DEBUG, "Flushing inbox");
        auto incoming_message = inbox.pop();
        int num_processed = 0;
        while (incoming_message && num_processed < MAX_PROCESSED_MESSAGES_PER_FLUSH) {
            logger.LogReceived(incoming_message->sender_id, Log::DEBUG, incoming_message->ToString());
            if (incoming_message->GetType() == Msg::EMPTY) {
                //no-op
            } else if (incoming_message->GetType() == Msg::BID_OFFER) {
                ProcessBid(*incoming_message);
            } else if (incoming_message->GetType() == Msg::ASK_OFFER) {
                ProcessAsk(*incoming_message);
            } else if (incoming_message->GetType() == Msg::REGISTER_REQUEST) {
                ProcessRegistrationRequest(*incoming_message);
            } else if (incoming_message->GetType() == Msg::SHUTDOWN_NOTIFY){
                ProcessShutdownNotify(*incoming_message);
            } else {
                logger.Log(Log::ERROR, "Unknown/unsupported message type");
            }
            num_processed++;
            incoming_message = inbox.pop();
        }
        if (num_processed == MAX_PROCESSED_MESSAGES_PER_FLUSH) {
            logger.Log(Log::WARN, "Inbox not fully flushed (tick "+std::to_string(ticks)+", " + std::to_string(inbox.size())+ " remaining)");
        }
        logger.Log(Log::DEBUG, "Flush finished (received " + std::to_string(num_processed)+")");
    }

    // Message processing
    void ProcessBid(Message& message) {
        auto bid = message.bid_offer;
        if (!bid) {
            logger.Log(Log::ERROR, "Malformed bid_offer message");
            return; //drop
        }
        bid_book_mutex.lock();
        bid_book[bid->commodity].push_back({*bid, {id, bid->commodity, bid->unit_price} });
        bid_book_mutex.unlock();
    }
    void ProcessAsk(Message& message) {
        auto ask = message.ask_offer;
        if (!ask) {
            logger.Log(Log::ERROR, "Malformed ask_offer message");
            return; //drop
        }
        ask_book_mutex.lock();
        ask_book[ask->commodity].push_back({*ask, {id, ask->commodity} });
        ask_book_mutex.unlock();
    }
    void ProcessRegistrationRequest(Message& message) {
        auto request = message.register_request;
        if (!request) {
            logger.Log(Log::ERROR, "Malformed register_request message");
            return; //drop
        }
        // check no id clash
        auto requested_id = message.sender_id;
        if (requested_id == id) {
            auto msg = Message(id);
            msg.AddRegisterResponse(RegisterResponse(id, false, "ID clash with auction house"));
            std::shared_ptr<Agent> ptr = request->trader_pointer.lock();
            SendDirect(msg, ptr);
            return;
        }

        if (known_traders.find(requested_id) != known_traders.end()) {
            auto msg = Message(id);
            msg.AddRegisterResponse(RegisterResponse(id, false, "ID clash with existing trader"));
            std::shared_ptr<Agent> ptr = request->trader_pointer.lock();
            SendDirect(msg, ptr);
            return;
        }

        // Otherwise, OK the request and register
        auto res = request->trader_pointer.lock();
        if (!res) {
            logger.Log(Log::ERROR, "Failed to convert weak_ptr to shared, unable to reply to reg request from "+std::to_string(requested_id));
            return;
        }
        auto type = res->class_name;
        if (demographics.count(type) != 1) {
            demographics[res->class_name] = 1;
        } else {
            demographics[res->class_name] += 1;
        }
        known_traders[requested_id] = std::move(res);
        auto msg = Message(id).AddRegisterResponse(RegisterResponse(id, true));
        SendMessage(*msg, requested_id);
    }
    void ProcessShutdownNotify(Message& message) {
        demographics[message.shutdown_notify->class_name] -= 1;
        num_deaths += 1;
        total_age += message.shutdown_notify->age_at_death;
        logger.Log(Log::INFO, "Deregistered trader "+std::to_string(message.sender_id));
        known_traders.erase(message.sender_id);
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
    int NumKnownTraders() const {
        return (int) known_traders.size();
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
                logger.Log(Log::WARN, "AH thread overran on tick "+ std::to_string(ticks) + ": took " + std::to_string(elapsed) +"/" + std::to_string(TICK_TIME_MS) + "ms )");
            }
            if (to_unix_timestamp_ms(std::chrono::system_clock::now()) > expiry_ms) {
              logger.Log(Log::ERROR, "Shutting down (expiry time reached)");
              Shutdown();
            }
        }
    }

    void TickOnce() {
      for (const auto& item : known_commodities) {
        ResolveOffers(item.first);
      }
      logger.Log(Log::INFO, "Net spread profit for tick" + std::to_string(ticks) + ": " + std::to_string(spread_profit));

      UpdatePriceInfoComponent<market::FoodMarket>("food");
      UpdatePriceInfoComponent<market::WoodMarket>("wood");
      UpdatePriceInfoComponent<market::FertilizerMarket>("fertilizer");
      UpdatePriceInfoComponent<market::OreMarket>("ore");
      UpdatePriceInfoComponent<market::MetalMarket>("metal");
      UpdatePriceInfoComponent<market::ToolsMarket>("tools");

    }
private:
    // SPATIALOS CONCEPTS
    bool ConstructInitialAuctionHouseEntity(worker::EntityId AH_id) {
      if (!IsConnected()) {
        return false;
      }
      worker::Entity AH_entity;

      AH_entity.Add<improbable::Metadata>({{"AuctionHouseEntity"}});
      AH_entity.Add<improbable::Persistence>({});
      AH_entity.Add<improbable::Position>({{0, 0, 0}});
      AH_entity.Add<improbable::AuthorityDelegation>({{{3020, 3}}});
      AH_entity.Add<market::RegisterCommandComponent>({});
      AH_entity.Add<market::MakeOfferCommandComponent>({});
      AH_entity.Add<market::FoodMarket>({{{
                                              "food",
                                              0.5
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
                                              1
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
                                                    0.1
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
                                             1
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
                                               1
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
                                               1
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
      view.OnCommandRequest<RegisterTraderCommand>(
          [&](const worker::CommandRequestOp<RegisterTraderCommand>& op) {
            connection.SendLogMessage(worker::LogLevel::kInfo, "AuctionHouse",
                                      "Received register request.\nCallerWorkerEntityId: " + std::to_string(op.CallerWorkerEntityId)
                                      + "\nEntityId: "+std::to_string(op.EntityId)
                                      + "\n(manually provided) Sender Id: " + std::to_string(op.Request.sender_id()));});

      view.OnCommandRequest<MakeBidOfferCommand>(
          [&](const worker::CommandRequestOp<MakeBidOfferCommand>& op) {
            std::cout << "Bid Offer received from #" << op.EntityId << "/" << op.Request.sender_id() <<  std::endl;
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
          });
    }
    // Transaction functions
    bool CheckBidStake(BidOffer& offer) {
        if (offer.quantity < 0 || offer.unit_price <= 0) {
            logger.Log(Log::WARN, "Rejected nonsensical bid: " + offer.ToString());
            return false;
        }

        //we refund the agent (if applicable) upon transaction resolution
        auto res = known_traders[offer.sender_id]->HasMoney(offer.quantity*offer.unit_price);
        if (!res) {
            logger.Log(Log::DEBUG, "Failed to take Bid stake: " + offer.ToString());
            return false;
        }
        return true;
    }
    bool CheckAskStake(AskOffer& offer) {
        if (offer.quantity < 0 || offer.unit_price <= 0) {
            logger.Log(Log::WARN, "Rejected nonsensical ask: " + offer.ToString());
            return false;
        }
        //we refund the agent (if applicable) upon transaction resolution
        auto res = known_traders[offer.sender_id]->HasCommodity(offer.commodity, offer.quantity);
        if (!res) {
            logger.Log(Log::DEBUG, "Failed to take Ask stake: " + offer.ToString());
            return false;
        }
        return true;
    }
    void CloseBid(const BidOffer& bid, BidResult bid_result) {
        if (bid.quantity > 0) {
            // partially unfilled
            bid_result.UpdateWithNoTrade(bid.quantity);
        }
        if (known_traders.find(bid.sender_id) != known_traders.end()) {
            SendMessage(*Message(id).AddBidResult(std::move(bid_result)), bid.sender_id);
        }
    }
    void CloseAsk(const AskOffer& ask, AskResult ask_result) {
        if (ask.quantity > 0) {
            // partially unfilled
            ask_result.UpdateWithNoTrade(ask.quantity);
        }
        if (known_traders.find(ask.sender_id) != known_traders.end()) {
            SendMessage(*Message(id).AddAskResult(std::move(ask_result)), ask.sender_id);
        }
    }

    // 0 - success
    // 1 - seller failed
    // 2 - buyer failed
    int MakeTransaction(const std::string& commodity, int buyer, int seller, int quantity, double clearing_price) {
        // take from seller
        auto actual_quantity = known_traders[seller]->TryTakeCommodity(commodity, quantity, 0, true);
        if (actual_quantity == 0) {
            // this may be unrecoverable, not sure
            logger.Log(Log::WARN, "Seller lacks good! Aborting trade");
            return 1;
        }
        auto actual_money = known_traders[buyer]->TryTakeMoney(actual_quantity*clearing_price, true);
        if (actual_money == 0) {
            // this may be unrecoverable, not sure
            logger.Log(Log::ERROR, "Buyer lacks money! Aborting trade");
            return 2;
        }

        known_traders[buyer]->TryAddCommodity(commodity, actual_quantity, clearing_price, false);
        //take sales tax from seller
        double profit = actual_quantity*clearing_price;
        known_traders[seller]->AddMoney(profit*(1-SALES_TAX));
        spread_profit += profit*SALES_TAX;

        auto info_msg = std::string("Made trade: ") + std::to_string(seller) + std::string(" >>> ") + std::to_string(buyer) + std::string(" : ") + commodity + std::string(" x") + std::to_string(quantity) + std::string(" @ $") + std::to_string(clearing_price);
        logger.Log(Log::INFO, info_msg);
        return 0;
    }

    void TakeBrokerFee(BidOffer& offer, BidResult& result) {
        if (known_traders.find(offer.sender_id) == known_traders.end()) {
            return; //trader not found
        }
        double fee = offer.quantity*offer.unit_price*BROKER_FEE;
        auto res = known_traders[offer.sender_id]->TryTakeMoney(fee, true);
        if (res > 0) {
            spread_profit += fee;
            result.broker_fee_paid = true;
        } else {
            //failed to take broker fee
            return;
        }
    }
    void TakeBrokerFee(AskOffer& offer, AskResult& result) {
        if (known_traders.find(offer.sender_id) == known_traders.end()) {
            return; //trader not found
        }
        double fee = offer.quantity*offer.unit_price*BROKER_FEE;
        auto res = known_traders[offer.sender_id]->TryTakeMoney(fee, true);
        if (res > 0) {
            spread_profit += fee;
            result.broker_fee_paid = true;
        } else {
            //failed to take broker fee
            return;
        }
    }

    bool ValidateBid(BidOffer& curr_bid, BidResult& bid_result, std::int64_t resolve_time) {
        if (known_traders.find(curr_bid.sender_id) == known_traders.end()) {
            return false; //trader not found
        }

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
        if (known_traders.find(curr_ask.sender_id) == known_traders.end()) {
            return false; //trader not found
        }
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

    bid_book_mutex.unlock();
    ask_book_mutex.unlock();
    }

};

#endif//CPPBAZAARBOT_AUCTION_HOUSE_H
