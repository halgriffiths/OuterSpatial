//
// Created by henry on 09/02/2022.
//

#ifndef OUTERSPATIALENGINE_TO_SCHEMA_H
#define OUTERSPATIALENGINE_TO_SCHEMA_H

#include "commodity.h"
#include "messages.h"

commodity::Commodity ToSchemaCommodity(Commodity& comm) {
  return {comm.name, comm.size, comm.market_component_id};
}

messages::BidOffer ToSchemaBidOffer(BidOffer& offer, worker::EntityId sender_entity_id = 0) {
  sender_entity_id = (sender_entity_id == 0) ? offer.sender_id : sender_entity_id;
  return {sender_entity_id, offer.commodity, offer.expiry_ms, offer.quantity, offer.unit_price};
}

messages::AskOffer ToSchemaAskOffer(AskOffer& offer, worker::EntityId sender_entity_id = 0) {
  sender_entity_id = (sender_entity_id == 0) ? offer.sender_id : sender_entity_id;
  return {sender_entity_id, offer.commodity, offer.expiry_ms, offer.quantity, offer.unit_price};
}

std::optional<market::PriceInfo> ToPriceInfo(worker::View& view, worker::EntityId ah_id, const std::string& commodity) {
  if (commodity == "food") {
    return view.Entities[ah_id].Get<market::FoodMarket>()->listing().price_info();
  } else if (commodity == "wood") {
    return view.Entities[ah_id].Get<market::WoodMarket>()->listing().price_info();
  } else if (commodity == "fertilizer") {
    return view.Entities[ah_id].Get<market::FertilizerMarket>()->listing().price_info();
  } else if (commodity == "ore") {
    return view.Entities[ah_id].Get<market::OreMarket>()->listing().price_info();
  } else if (commodity == "metal") {
    return view.Entities[ah_id].Get<market::MetalMarket>()->listing().price_info();
  } else if (commodity == "tools") {
    return view.Entities[ah_id].Get<market::ToolsMarket>()->listing().price_info();
  }
}
#endif  // OUTERSPATIALENGINE_TO_SCHEMA_H
