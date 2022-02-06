//
// Created by henry on 15/12/2021.
//

#ifndef CPPBAZAARBOT_METRICS_H
#define CPPBAZAARBOT_METRICS_H

#include "../traders/AI_trader.h"
# include <regex>
#include <fstream>

class PlayerTrader;

namespace {
    // SRC: https://www.jeremymorgan.com/tutorials/c-programming/how-to-capture-the-output-of-a-linux-command-in-c/
    std::string GetStdoutFromCommand(std::string cmd) {
        std::string data;
        FILE * stream;
        const int max_buffer = 256;
        char buffer[max_buffer];
        cmd.append(" 2>&1");

        stream = popen(cmd.c_str(), "r");

        if (stream) {
            while (!feof(stream))
                if (fgets(buffer, max_buffer, stream) != NULL) data.append(buffer);
            pclose(stream);
        }
        return data;
    }
}

// Intended to be stored in-memory, this lightweight metric tracker is for human player UI purposes
namespace metrics{
    enum RegisterProgress {
      NONE,
      RESERVED_ID,
      CREATED_MONITOR,
      ASSIGNED_PARTITION
    };
}

class LocalMetrics {
public:
    std::vector<std::string> tracked_goods;
    std::vector<std::string> tracked_roles;
    History local_history = {};
    metrics::RegisterProgress progress = metrics::NONE;
private:
    friend PlayerTrader;

    int curr_tick = 0;
    std::uint64_t offset;
    std::uint64_t start_time;
    std::uint64_t prev_time;
    worker::Connection& connection;
    worker::View& view;


  public:
    worker::EntityId monitor_entity_id;

    LocalMetrics(worker::Connection& connection, worker::View& view, std::uint64_t start_time, const std::vector<std::string>& tracked_goods, std::vector<std::string> tracked_roles)
    : tracked_goods(tracked_goods)
    , tracked_roles(tracked_roles)
    , start_time(start_time)
    , connection(connection)
    , view(view)
     {
        offset = to_unix_timestamp_ms(std::chrono::system_clock::now()) - start_time;
        for (auto& item : tracked_goods) {
            local_history.initialise(item);
        }
       MakeCallbacks();
    }

    void PrintSummary() {
      for (auto& good : tracked_goods) {
        std::cout << "# " << good << std::endl;
        if (local_history.exists(good)) {
          std::cout << "Price (avg): " << local_history.prices.most_recent[good] << " (" << local_history.prices.t_average(good, -1) << ")" << std::endl;;
        } else {
          std::cout << "Good " << good << " not found in local history\n";
        }
      }
    }

  void MakeCallbacks() {
    view.OnReserveEntityIdsResponse([&](const worker::ReserveEntityIdsResponseOp& op) {
      if (op.StatusCode == worker::StatusCode::kSuccess) {
        monitor_entity_id = *op.FirstEntityId;
        progress = metrics::RESERVED_ID;
      } else {
        std::cout << "Received failure code: " << op.Message << std::endl;
      }
    });
    view.OnCreateEntityResponse([&](const worker::CreateEntityResponseOp& op) {
      if (op.StatusCode == worker::StatusCode::kSuccess) {
        connection.SendLogMessage(worker::LogLevel::kInfo, "Monitor",
                                  "Successfully created entity");
        progress = metrics::CREATED_MONITOR;
      } else {
        connection.SendLogMessage(worker::LogLevel::kWarn, "Monitor",
                                  "Failed to create entity");
      };});
    using AssignPartitionCommand = improbable::restricted::Worker::Commands::AssignPartition;
    view.OnCommandResponse<AssignPartitionCommand>(
        [&](const worker::CommandResponseOp<AssignPartitionCommand>& op) {
          if (op.StatusCode == worker::StatusCode::kSuccess) {
            connection.SendLogMessage(worker::LogLevel::kInfo, "Monitor",
                                      "Successfully assigned partition.");
            progress = metrics::ASSIGNED_PARTITION;
          } else {
            connection.SendLogMessage(worker::LogLevel::kError, "Monitor",
                                      "Failed to assign partition: error code : " +
                                      std::to_string(static_cast<std::uint8_t>(op.StatusCode)) +
                                      " message: " + op.Message);
          }
        });
    view.OnComponentUpdate<market::FoodMarket>(
        [&](const worker::ComponentUpdateOp<market::FoodMarket >& op) {
          std::string commodity = "food";
          worker::EntityId entity_id = op.EntityId;
          market::FoodMarket::Update update = op.Update;
          local_history.prices.add(commodity, update.listing()->price_info().curr_price());
          local_history.net_supply.add(commodity, update.listing()->price_info().curr_net_supply());
        });
    view.OnComponentUpdate<market::WoodMarket>(
        [&](const worker::ComponentUpdateOp<market::WoodMarket >& op) {
          std::string commodity = "wood";
          worker::EntityId entity_id = op.EntityId;
          market::WoodMarket::Update update = op.Update;
          local_history.prices.add(commodity, update.listing()->price_info().curr_price());
          local_history.net_supply.add(commodity, update.listing()->price_info().curr_net_supply());
        });
    view.OnComponentUpdate<market::FertilizerMarket>(
        [&](const worker::ComponentUpdateOp<market::FertilizerMarket >& op) {
          std::string commodity = "fertilizer";
          worker::EntityId entity_id = op.EntityId;
          market::FertilizerMarket::Update update = op.Update;
          local_history.prices.add(commodity, update.listing()->price_info().curr_price());
          local_history.net_supply.add(commodity, update.listing()->price_info().curr_net_supply());
        });
    view.OnComponentUpdate<market::OreMarket>(
        [&](const worker::ComponentUpdateOp<market::OreMarket >& op) {
          std::string commodity = "ore";
          worker::EntityId entity_id = op.EntityId;
          market::OreMarket::Update update = op.Update;
          local_history.prices.add(commodity, update.listing()->price_info().curr_price());
          local_history.net_supply.add(commodity, update.listing()->price_info().curr_net_supply());
        });
    view.OnComponentUpdate<market::MetalMarket>(
        [&](const worker::ComponentUpdateOp<market::MetalMarket >& op) {
          std::string commodity = "metal";
          worker::EntityId entity_id = op.EntityId;
          market::MetalMarket::Update update = op.Update;
          local_history.prices.add(commodity, update.listing()->price_info().curr_price());
          local_history.net_supply.add(commodity, update.listing()->price_info().curr_net_supply());
        });
    view.OnComponentUpdate<market::ToolsMarket>(
        [&](const worker::ComponentUpdateOp<market::ToolsMarket >& op) {
          std::string commodity = "tools";
          worker::EntityId entity_id = op.EntityId;
          market::ToolsMarket::Update update = op.Update;
          local_history.prices.add(commodity, update.listing()->price_info().curr_price());
          local_history.net_supply.add(commodity, update.listing()->price_info().curr_net_supply());
        });
  }
  void CreateMonitorEntity() {
    worker::Entity monitor_entity;

    // Market component IDs start at 3010 and increment to 3015
    uint market_id = 3010;
    std::vector<improbable::ComponentSetInterest_Query> all_queries;
    for (auto& good : tracked_goods) {
        improbable::ComponentSetInterest_QueryConstraint my_constraint;
        my_constraint.set_component_constraint({market_id});
        improbable::ComponentSetInterest_Query my_query;
        my_query.set_constraint(my_constraint);

        my_query.set_result_component_id({market_id});
        market_id++;
        all_queries.push_back(my_query);
    }
    worker::List<improbable::ComponentSetInterest_Query> const_queries;
    const_queries.insert(const_queries.begin(), all_queries.begin(), all_queries.end());

    improbable::ComponentSetInterest all_markets_interest;
    all_markets_interest.set_queries(const_queries);

    monitor_entity.Add<improbable::Metadata>({{"MonitorEntity"}});
    monitor_entity.Add<improbable::Position>({{3, 0, 0}});
    monitor_entity.Add<improbable::Interest>({{{50, all_markets_interest}}});

    monitor_entity.Add<improbable::AuthorityDelegation>({{{50, monitor_entity_id}}});
    connection.SendCreateEntityRequest(monitor_entity, monitor_entity_id, {500});
  }
};

// Intended to be stored serverside, this produces datafiles on-disk which can be checked for debugging/analysis purposes
class GlobalMetrics {
public:
    std::vector<std::string> tracked_goods;
    std::vector<std::string> tracked_roles;
    int total_deaths = 0;
    double avg_overall_age = 0;
    std::map<std::string, int> deaths_per_class;
    std::map<std::string, double> age_per_class;
    std::map<std::string, std::vector<std::pair<double, double>>> avg_price_metrics;

    double avg_lifespan = 0;

private:
    std::string folder = "global_tmp/";
    std::shared_ptr<std::mutex> file_mutex;
    int curr_tick = 0;
    std::uint64_t offset;
    std::uint64_t start_time;

    std::map<std::string, std::vector<std::pair<double, double>>> net_supply_metrics;
    std::map<std::string, std::vector<std::pair<double, double>>> avg_trades_metrics;
    std::map<std::string, std::vector<std::pair<double, double>>> avg_asks_metrics;
    std::map<std::string, std::vector<std::pair<double, double>>> avg_bids_metrics;
    std::map<std::string, std::vector<std::pair<double, double>>> num_alive_metrics;

    std::map<std::string, std::unique_ptr<std::ofstream>> data_files;

    int lookback = 1;
public:
    GlobalMetrics(std::uint64_t start_time, const std::vector<std::string>& tracked_goods, std::vector<std::string> tracked_roles, std::shared_ptr<std::mutex> mutex)
            : start_time(start_time)
            , tracked_goods(tracked_goods)
            , tracked_roles(tracked_roles)
            , file_mutex(mutex) {
        offset = to_unix_timestamp_ms(std::chrono::system_clock::now()) - start_time;
        init_datafiles();

        for (auto& good : tracked_goods) {
            net_supply_metrics[good] = {};
            avg_price_metrics[good] = {};
            avg_trades_metrics[good] = {};
            avg_asks_metrics[good] = {};
            avg_bids_metrics[good] = {};
        }
        for (auto& role : tracked_roles) {
            num_alive_metrics[role] = {};
            age_per_class[role] = 0;
            deaths_per_class[role] = 0;
        }
    }

    void init_datafiles() {
        file_mutex->lock();
        for (auto& good : tracked_goods) {
            data_files[good] = std::make_unique<std::ofstream>();
            data_files[good]->open((folder+good + ".dat").c_str(), std::ios::trunc);
            *(data_files[good].get()) << "# raw data file for " << good << std::endl;
            *(data_files[good].get()) << "0 0\n";
        }
        file_mutex->unlock();
    }
    void update_datafiles(std::uint64_t stop_time) {
        file_mutex->lock();
        for (auto& item : data_files) {
            item.second->close();
            item.second->open((folder+item.first + ".dat").c_str(), std::ios::app);
            int num = 0;
            double total_value = 0;
            double total_time_s = 0;
            auto it = avg_price_metrics.at(item.first).rbegin();
            while (it != avg_price_metrics.at(item.first).rend() && it->first >= stop_time/1000) {
                total_value += it->second;
                total_time_s += it->first;
                num++;
                it++;
            }
            if (num > 0) {
                double avg_value = total_value/num;
                double avg_time = total_time_s/num;
                *(item.second.get()) << avg_time << " " << avg_value << "\n";
            }
        }
        file_mutex->unlock();
    }
    void CollectMetrics(const std::shared_ptr<AuctionHouse>& auction_house) {
        auto local_curr_time = to_unix_timestamp_ms(std::chrono::system_clock::now());
        double time_passed_s = (double)(local_curr_time - offset - start_time) / 1000;
        for (auto& good : tracked_goods) {
            double price = auction_house->MostRecentPrice(good);
            double asks = auction_house->AverageHistoricalAsks(good, lookback);
            double bids = auction_house->AverageHistoricalBids(good, lookback);
            double trades = auction_house->AverageHistoricalTrades(good, lookback);

            avg_price_metrics[good].emplace_back(time_passed_s, price);
            avg_trades_metrics[good].emplace_back(time_passed_s, trades);
            avg_asks_metrics[good].emplace_back(time_passed_s, asks);
            avg_bids_metrics[good].emplace_back(time_passed_s, bids);

            net_supply_metrics[good].emplace_back(time_passed_s, asks-bids);
        }

        auto res = auction_house->GetDemographics();
        avg_lifespan = res.first;
        auto demographics = res.second;
        for (auto& role : tracked_roles) {
            num_alive_metrics[role].emplace_back(time_passed_s, demographics[role]);
        }

        curr_tick++;
    }

    void TrackDeath(const std::string& class_name, int age) {
        avg_overall_age = (avg_overall_age*total_deaths + age)/(total_deaths+1);
        total_deaths++;

        age_per_class[class_name] = (age_per_class[class_name]*deaths_per_class[class_name] + age)/(deaths_per_class[class_name]+1);
        deaths_per_class[class_name]++;
        // TODO: Either finish this or remove it
        //std::map<std::string, double> age_per_class;
    }
};
#endif//CPPBAZAARBOT_METRICS_H
