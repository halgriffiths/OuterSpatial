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



messages::AIRole ChooseNewClassRandom(std::mt19937& gen) {
  std::uniform_int_distribution<> random_job(2, 4); // hardcoded to match messages.schema
  return static_cast<messages::AIRole>(random_job(gen));
}


#endif //BAZAAR_BOT_H
