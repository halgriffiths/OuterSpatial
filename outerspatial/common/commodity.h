//
// Created by henry on 06/12/2021.
//

#ifndef CPPBAZAARBOT_COMMODITY_H
#define CPPBAZAARBOT_COMMODITY_H

// simplest form of Commodity, detailing the name and size (eg: "wood", 1)
class Commodity {
public:
    std::string name;
    double size;   // how much space a single unit consumes in inventory
    int market_component_id;
   explicit Commodity(std::string commodity_name = "default_commodity", double commodity_size = 1, int market_component_id = 0)
           : name(commodity_name)
           , size(commodity_size)
           , market_component_id(market_component_id) {};
};

// extended form for Auction House, which also contains some trade data
class CommodityInfo : public Commodity {

};
#endif//CPPBAZAARBOT_COMMODITY_H
