//
// Created by henry on 06/12/2021.
//

#ifndef CPPBAZAARBOT_COMMODITY_H
#define CPPBAZAARBOT_COMMODITY_H

// simplest form of Commodity, detailing the name and size (eg: "wood", 1)
class Commodity {
public:
    double size;   // how much space a single unit consumes in inventory
    explicit Commodity(std::string commodity_name = "default_commodity", double commodity_size = 1)
           : name(commodity_name)
           , size(commodity_size) {};
    std::string name;
};

// extended form for Auction House, which also contains some trade data
class CommodityInfo : public Commodity {

};
#endif//CPPBAZAARBOT_COMMODITY_H
