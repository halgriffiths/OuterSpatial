//
// Created by henry on 06/12/2021.
//
#ifndef CPPBAZAARBOT_INVENTORY_H
#define CPPBAZAARBOT_INVENTORY_H
#include "../common/commodity.h"
#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include <unordered_set>

// Cut-down version of Inventory with only metadata
class CommodityBelief {
public:
    std::string name;
    int ideal;
    double cost;

    CommodityBelief(std::string commodity_name, int ideal, double original_cost)
    : name(std::move(commodity_name))
    , ideal(ideal)
    , cost(original_cost) {}
};

class CommodityBeliefs {
public:
  std::map<std::string, CommodityBelief> commodity_beliefs;
  CommodityBeliefs() = default;

  void InitializeBelief(std::string commodity, int ideal_quantity = 0, double original_cost = 0.0) {
    commodity_beliefs[commodity] = CommodityBelief(commodity, ideal_quantity, original_cost);
  }

  void UpdateCostFromProduction(const std::string& name, int quantity, double unit_price) {
    if (commodity_beliefs.count(name) != 1) {
      return;// no entry found
    }
    double alpha = 0.2;
    if (unit_price > 0) {
      if (commodity_beliefs[name].cost == 0.0) {
        commodity_beliefs[name].cost = unit_price;
      }
      for (int i = 0; i < quantity; i++) {
        // update EWMA
        commodity_beliefs[name].cost = alpha*unit_price + (1 - alpha)*commodity_beliefs[name].cost;
      }
    }
  }

  double GetCost(const std::string& name) const {
    if (commodity_beliefs.count(name) != 1) {
      return 0;// no entry found
    }
    return commodity_beliefs.at(name).cost;
  }

  int GetIdeal(const std::string& name) const {
    if (commodity_beliefs.count(name) != 1) {
      return 0;// no entry found
    }
    return commodity_beliefs.at(name).ideal;
  }
};

class InventoryItem {
public:
    InventoryItem() = default;
    InventoryItem(std::string commodity_name) : name(std::move(commodity_name)) {};
    InventoryItem(Commodity commodity, int starting_quantity, int ideal)
        : name(commodity.name)
    , stored(starting_quantity)
    , ideal_quantity(ideal)
    , size(commodity.size) { };

    InventoryItem(std::string commodity_name, int starting_quantity, int ideal = 0)
        : name(std::move(commodity_name))
        , stored(starting_quantity)
        , ideal_quantity(ideal) {};
    std::string name = "default_commodity";
    int stored = 0;
    int ideal_quantity = 0;
    double original_cost = 0.1;
    double size = 1;            //default to 1 unit of commodity per unit of inventory space
};

class Inventory {
public:
    double max_size = 50;
    std::map<std::string, InventoryItem> inventory;

    Inventory() = default;
    Inventory(double max_size, const std::vector<InventoryItem> &starting_inv)
    : max_size(max_size) {
        for (const auto& item : starting_inv) {
            inventory[item.name] = item;
        }
    };

    void SetItem(const std::string& name, InventoryItem &new_item) {
        inventory[name] = new_item;
    }

    bool SetIdeal(const std::string& name, int ideal_quantity) {
        if (inventory.count(name) != 1) {
            return false;// no entry found
        }
        inventory[name].ideal_quantity = ideal_quantity;
        return true;
    }
    void SetCost(const std::string& name, double cost) {
        if (inventory.count(name) != 1) {
            return;// no entry found
        }
        inventory[name].original_cost = cost;
    }
    //ignores space constraints - must be checked by trader
    void AddItem(const std::string& name, int quantity, std::optional<double> unit_price = std::nullopt) {
        if (unit_price && *unit_price > 0) {
            if (inventory[name].stored > 0) {
                // update avg orig. cost
                inventory[name].original_cost = (inventory[name].original_cost * inventory[name].stored + quantity*(*unit_price)) / (inventory[name].stored + quantity);

            } else {
                inventory[name].original_cost = *unit_price;
            }
        }
        inventory[name].stored += quantity;
    }
    //ignores space constraints - must be checked by trader
    void TakeItem(const std::string& name, int quantity, std::optional<double> unit_price = std::nullopt) {
        inventory[name].stored -= quantity;
    }

    int Query(const std::string& name) {
        if (inventory.count(name) != 1) {
            return 0; // no entry found
        }
        return inventory[name].stored;
    }

    double QueryCost(const std::string& name) {
        if (inventory.count(name) != 1) {
            return 0;; // no entry found
        }
        return inventory[name].original_cost;
    }

    std::optional<InventoryItem> GetItem(const std::string& name) {
        if (inventory.count(name) != 1) {
            return std::nullopt;// no entry found
        }
        return inventory[name];
    }

    double GetUsedSpace() {
        double used = 0;
        for (const auto &item : inventory) {
            used += item.second.stored * item.second.size;
        }
        return used;
    }

    double GetEmptySpace() {
        return max_size - GetUsedSpace();
    }

    std::optional<double> ChangeItem(const std::string& name, int delta, double unit_cost) {
        if (inventory.count(name) != 1) {
            return std::nullopt;// no entry found
        }
        InventoryItem *item_entry = &inventory[name];
        if (unit_cost > 0) {
            if (item_entry->stored <= 0) {
                item_entry->stored = delta;
                item_entry->original_cost = unit_cost;
            } else {
                // average out original cost
                item_entry->original_cost = (item_entry->stored * item_entry->original_cost + delta * unit_cost) / (item_entry->stored + delta);
                item_entry->stored += delta;
            }
        } else {
            item_entry->stored += delta;
            // item_entry->original_cost is unchanged?
        }

        if (item_entry->stored < 0) {
            item_entry->stored = 0;
            item_entry->original_cost = 0;
        }
        return item_entry->original_cost;//return current unit cost
    }

    int Surplus(const std::string& name) {
        auto stored = Query(name);
        if (!stored) {
            return 0;
        }

        int target = inventory[name].ideal_quantity;
        if (stored > target) {
            return stored - target;
        }
        return 0;
    }

    int Shortage(const std::string& name) {
        auto stored = Query(name);

        int target = inventory[name].ideal_quantity;
        if (stored < target) {
            return target - stored;
        }
        return 0;
    }
    double GetSize(std::string commodity) {
        if (inventory.count(commodity) != 1) {
            return 0;// no entry found
        };
        return inventory[commodity].size;
    }
};
#endif//CPPBAZAARBOT_INVENTORY_H
