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
    auto market = view.Entities[ah_id].Get<market::FoodMarket>();
    if (!market) {
      return {};
    } else {
      return market->listing().price_info();
    }
  } else if (commodity == "wood") {
    auto market = view.Entities[ah_id].Get<market::WoodMarket>();
    if (!market) {
      return {};
    } else {
      return market->listing().price_info();
    }
  } else if (commodity == "fertilizer") {
    auto market = view.Entities[ah_id].Get<market::FertilizerMarket>();
    if (!market) {
      return {};
    } else {
      return market->listing().price_info();
    }
  } else if (commodity == "ore") {
    auto market = view.Entities[ah_id].Get<market::OreMarket>();
    if (!market) {
      return {};
    } else {
      return market->listing().price_info();
    }
  } else if (commodity == "metal") {
    auto market = view.Entities[ah_id].Get<market::MetalMarket>();
    if (!market) {
      return {};
    } else {
      return market->listing().price_info();
    }
  } else if (commodity == "tools") {
    auto market = view.Entities[ah_id].Get<market::ToolsMarket>();
    if (!market) {
      return {};
    } else {
      return market->listing().price_info();
    }
  } else {
    return {};
  }
}

std::string ToString(messages::BidOffer& offer) {
  std::string output("BID from ");
  output.append(std::to_string(offer.sender_id()))
      .append(": ")
      .append(offer.good())
      .append(" x")
      .append(std::to_string(offer.quantity()))
      .append(" @ $")
      .append(std::to_string(offer.unit_price()));
  return output;
}

std::string ToString(messages::AskOffer& offer) {
  std::string output("ASK from ");
  output.append(std::to_string(offer.sender_id()))
      .append(": ")
      .append(offer.good())
      .append(" x")
      .append(std::to_string(offer.quantity()))
      .append(" @ $")
      .append(std::to_string(offer.unit_price()));
  return output;
}
#endif  // OUTERSPATIALENGINE_TO_SCHEMA_H
