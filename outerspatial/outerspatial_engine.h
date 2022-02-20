#ifndef BAZAAR_BOT_H
#define BAZAAR_BOT_H

#include <improbable/view.h>
#include <improbable/worker.h>

#include "common/to_schema.h"
#include "common/concurrency.h"
#include "common/agent.h"
#include "common/messages.h"
#include "common/commodity.h"

#include "traders/inventory.h"

#include "auction/auction_house.h"

#include "metrics/logger.h"
#include "metrics/metrics.h"
#include "metrics/display.h"

#include "traders/AI_trader.h"
#include "traders/fake_trader.h"
#include "traders/human_trader.h"

//std::shared_ptr<AITrader> CreateAndRegister(int id,
//                                               const std::shared_ptr<AuctionHouse>& auction_house,
//                                               std::shared_ptr<Role> AI_logic,
//                                               const std::string& name,
//                                               double starting_money,
//                                               double inv_capacity,
//                                               const std::vector<InventoryItem> inv,
//                                               int tick_time_ms,
//                                               Log::LogLevel log_level
//) {
//    auto trader = std::make_shared<AITrader>(id, auction_house, std::move(AI_logic), name, starting_money, inv_capacity, inv, tick_time_ms,  log_level);
//    trader->SendMessage(*Message(id).AddRegisterRequest(std::move(RegisterRequest(trader->id, trader))), auction_house->id);
//    trader->TickOnce();
//    return trader;
//}

int RandomChoice(int num_weights, std::vector<double>& weights, std::mt19937& gen) {
    double sum_of_weight = 0;
    for(int i=0; i<num_weights; i++) {
        sum_of_weight += weights[i];
    }
    std::uniform_real_distribution<> random(0, sum_of_weight);
    double rnd = random(gen);
    for(int i=0; i<num_weights; i++) {
        if(rnd < weights[i])
            return i;
        rnd -= weights[i];
    }
    return -1;
}

std::string GetProducer(std::string& commodity) {
    if (commodity == "food") {
        return "farmer";
    } else if (commodity == "fertilizer") {
        return "composter";
    } else if (commodity == "wood") {
        return "woodcutter";
    } else if (commodity == "ore") {
        return "miner";
    } else if (commodity == "metal") {
        return "refiner";
    } else if (commodity == "tools") {
        return "blacksmith";
    } else {
        return "null";
    }
}

messages::AIRole ChooseNewClassRandom(std::mt19937& gen) {
  std::uniform_int_distribution<> random_job(0, 7); // hardcoded to match messages.schema
  return static_cast<messages::AIRole>(random_job(gen));
}

std::string ChooseNewClassWeighted(std::vector<std::string>& tracked_goods, std::shared_ptr<AuctionHouse>& auction_house, std::mt19937& gen) {
    std::vector<double> weights;
    double gamma = -0.02;
    //auction house ticks at 10ms
    int lookback_time_ms = 1000;
    for (auto& commodity : tracked_goods) {
        double supply = auction_house->t_AverageHistoricalSupply(commodity, lookback_time_ms);
//        double supply = auction_house->AverageHistoricalAsks(commodity, 100) - auction_house->AverageHistoricalBids(commodity, 100);
        weights.push_back(std::exp(gamma*supply));
    }
    int choice = RandomChoice((int) weights.size(),  weights, gen);
    return GetProducer(tracked_goods[choice]);
}


#endif //BAZAAR_BOT_H
