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
class LocalMetrics {
public:
    std::vector<std::string> tracked_goods;
    std::vector<std::string> tracked_roles;
    History local_history = {};

private:
    friend PlayerTrader;

    int curr_tick = 0;
    std::uint64_t offset;
    std::uint64_t start_time;
    std::uint64_t prev_time;
public:
    LocalMetrics(std::uint64_t start_time, const std::vector<std::string>& tracked_goods, std::vector<std::string> tracked_roles)
    : start_time(start_time)
    , tracked_goods(tracked_goods)
    , tracked_roles(tracked_roles) {
        offset = to_unix_timestamp_ms(std::chrono::system_clock::now()) - start_time;
        for (auto& item : tracked_goods) {
            local_history.initialise(item);
        }
    }

    void CollectAuctionHouseMetrics(const std::shared_ptr<AuctionHouse>& auction_house) {
        auto local_curr_time = to_unix_timestamp_ms(std::chrono::system_clock::now());
        double time_passed_s = (double)(local_curr_time - offset - start_time) / 1000;
        std::uint64_t lookback_ms = local_curr_time - prev_time;
        for (auto& good : tracked_goods) {
            double price = auction_house->t_AverageHistoricalPrice(good, lookback_ms);
            double asks = auction_house->t_AverageHistoricalAsks(good, lookback_ms);
            double bids = auction_house->t_AverageHistoricalBids(good, lookback_ms);

            local_history.prices.add(good, price);
            local_history.asks.add(good, asks);
            local_history.bids.add(good, bids);
            local_history.net_supply.add(good, asks-bids);
        }
        curr_tick++;
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
