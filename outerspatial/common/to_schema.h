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
#endif  // OUTERSPATIALENGINE_TO_SCHEMA_H
