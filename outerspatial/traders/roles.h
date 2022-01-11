//
// Created by henry on 14/12/2021.
//

#ifndef CPPBAZAARBOT_ROLES_H
#define CPPBAZAARBOT_ROLES_H

#include "AI_trader.h"

class EmptyRole : public Role {
    void TickRole(AITrader& trader) override {};
};

class RoleFarmer : public Role {
public:
    RoleFarmer(int min_cost) : Role("fertilizer", min_cost) {};
    void TickRole(AITrader& trader) override {
        bool has_wood = (0 < trader.Query("wood"));
        bool has_tools = (0 < trader.Query("tools"));
        bool has_fertilizer = (0 < trader.Query("fertilizer"));

        if (!has_fertilizer) {
            LoseMoney(trader, trader.GetIdleTax());
            return;
        }
        Consume(trader, "fertilizer", 1);
        if (has_tools && has_wood) {
            // 10% chance tools break
            Consume(trader, "tools", 1, 0.1);
            Consume(trader, "wood", 1);
            Produce(trader, "food", 6);
        } else if (has_wood){
            Consume(trader, "wood", 1);
            Produce(trader, "food", 3);
        } else {
            Produce(trader, "food", 1);
        }
    }
};

class RoleWoodcutter : public Role {
public:
    RoleWoodcutter(int min_cost) : Role("food", min_cost) {};
    void TickRole(AITrader& trader) override {
        bool has_food = (0 < trader.Query("food"));
        bool has_tools = (0 < trader.Query("tools"));

        if (!has_food) {
            LoseMoney(trader, trader.GetIdleTax());//$2 idleness fine
            return;
        }

        if (has_tools) {
            // 10% chance tools break
            Consume(trader, "tools", 1, 0.1);
            Consume(trader, "food", 1);
            Produce(trader, "wood", 2);
        } else {
            Consume(trader, "food", 1);
            Produce(trader, "wood", 1);
        }
    }
};

class RoleComposter : public Role {
public:
    RoleComposter(int min_cost) : Role("food", min_cost) {};
    void TickRole(AITrader& trader) override {
        bool has_food = (0 < trader.Query("food"));
        if (!has_food) {
            LoseMoney(trader, trader.GetIdleTax());
            return;
        }
        Consume(trader, "food", 1);
        Produce(trader, "fertilizer", 1, 0.5);
    }
};

class RoleBlacksmith : public Role {
public:
    RoleBlacksmith(int min_cost) : Role("food", min_cost) {};
    void TickRole(AITrader& trader) override {
        bool has_food = (0 < trader.Query("food"));
        int amount_metal = trader.Query("metal");

        if (!has_food) {
            LoseMoney(trader, trader.GetIdleTax());
        }

        Consume(trader, "food", 1);

        if (amount_metal > 0) {
            Consume(trader, "metal", amount_metal);
            Produce(trader, "tools", amount_metal);
        }
    };
};

class RoleMiner : public Role {
public:
    RoleMiner(int min_cost) : Role("food", min_cost) {};
    void TickRole(AITrader& trader) override {
        bool has_food = (0 < trader.Query("food"));
        bool has_tools = (0 < trader.Query("tools"));

        if (!has_food) {
            LoseMoney(trader, trader.GetIdleTax());
        }
        Consume(trader, "food", 1);
        if (has_tools) {
            Consume(trader, "tools", 1, 0.1);
            Produce(trader, "ore", 4);
        } else {
            Produce(trader, "ore", 2);
        }
    };
};

class RoleRefiner : public Role {
public:
    RoleRefiner(int min_cost) : Role("food", min_cost) {};
    void TickRole(AITrader& trader) override {
        bool has_food = (0 < trader.Query("food"));
        bool has_tools = (0 < trader.Query("tools"));
        int amount_ore = trader.Query("ore");
        if (!has_food) {
            LoseMoney(trader, trader.GetIdleTax());
        }
        Consume(trader, "food", 1);

        if (has_tools) {
            Consume(trader, "tools", 1, 0.1);
            Consume(trader, "ore", amount_ore);
            Produce(trader, "metal", amount_ore);
        } else {
            //convert up to 2 ore into metal if no tools
            int quantity = std::min(amount_ore, 2);
            Consume(trader, "ore", quantity);
            Produce(trader, "metal", quantity);
        }
    };
};
#endif//CPPBAZAARBOT_ROLES_H
