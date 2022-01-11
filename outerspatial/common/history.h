//
// Created by henry on 07/12/2021.
//

#ifndef CPPBAZAARBOT_HISTORY_H
#define CPPBAZAARBOT_HISTORY_H
#include <map>
#include <vector>
#include <atomic>

enum LogType {
    PRICE,
    ASK,
    BID,
    TRADE,
    NET_SUPPLY
};

class HistoryLog {
    int max_size = 60000; //10 min worth of data @ 10ms frametime
public:
    LogType type;
    std::map<std::string, std::vector<std::pair<double, std::int64_t>>> log;
    std::map<std::string, std::atomic<double>> most_recent;
    HistoryLog(LogType log_type)
    : type(log_type) {
        log = {};
    }

    void initialise(const std::string& name) {
        if (log.count(name) > 0) {
            return;// already registered
        }
        double starting_value = (type == LogType::PRICE) ? 10 : 0;
        log[name].emplace_back(starting_value, to_unix_timestamp_ms(std::chrono::system_clock::now()));
        most_recent[name] = starting_value;
    }

    void add(const std::string& name, double amount) {
        if (log.count(name) != 1) {
            return;// no entry found
        }
        if (log[name].size() == max_size) {
            log[name].erase(log[name].begin());
        }
        log[name].emplace_back(amount, to_unix_timestamp_ms(std::chrono::system_clock::now()));
        most_recent[name] = amount;
    }

    double average(const std::string& name, int range) const {
        if (log.count(name) != 1) {
            return 0;// no entry found
        }
        int log_length = log.at(name).size();
        if (log_length < range) {
            range = log_length;
        }

        double total = 0;
        for (int i = log_length - range; i < log_length; i++) {
            total += log.at(name)[i].first;
        }
        return total/range;
    }
    // time-based average
    double t_average(const std::string& name, std::int64_t duration) const {
        if (log.count(name) != 1) {
            return 0;// no entry found
        }
        auto start_time = log.at(name).back().second - duration;
        double total = 0;
        int range = 0;
        auto it = log.at(name).rbegin();
        while (it != log.at(name).rend() && it->second >= start_time) {
            total += it->first;
            range++;
            it++;
        }
        return total/range;
    }

    double percentage_change(const std::string& name, int window) const {
        double prev_value;
        if (window <= log.at(name).size()) {
            prev_value = log.at(name)[log.at(name).size() - window].first;
        } else {
            prev_value = log.at(name)[0].first;
        }

        double curr_value = log.at(name).back().first;
        return 100*(curr_value- prev_value)/prev_value;
    }

    double t_percentage_change(const std::string& name, std::int64_t duration) const {
        if (log.count(name) != 1) {
            return 0;// no entry found
        }
        auto start_time = log.at(name).back().second - duration;
        double prev_value;
        auto it = log.at(name).rbegin();
        while (it != log.at(name).rend() && it->second >= start_time) {
            it++;
        }
        if (it == log.at(name).rend()) {
            prev_value = log.at(name).front().first;
        } else {
            prev_value = it->first;
        }

        double curr_value = log.at(name).back().first;
        return 100*(curr_value- prev_value)/prev_value;
    }

    std::vector<std::pair<double, double>> get_history(const std::string& name, std::int64_t start_time) {
        std::vector<std::pair<double, double>> output = {};
        if (log.count(name) != 1) {
            return output;// no entry found
        }
        for (auto& item : log[name]) {
            if (item.second >= start_time) {
                output.emplace_back(item.second, item.first);
            }
        }
        return output;
    }
};

class History{
public:
    HistoryLog prices;
    HistoryLog buy_prices;
    HistoryLog asks;
    HistoryLog bids;
    HistoryLog net_supply;
    HistoryLog trades;

    History()
        : prices(HistoryLog(PRICE))
        , buy_prices(HistoryLog(PRICE))
        , asks(HistoryLog(ASK))
        , bids(HistoryLog(BID))
        , trades(HistoryLog(TRADE))
        , net_supply(HistoryLog(NET_SUPPLY)) { };


    void initialise(const std::string& name) {
        prices.initialise(name);
        buy_prices.initialise(name);
        asks.initialise(name);
        bids.initialise(name);
        trades.initialise(name);
        net_supply.initialise(name);
    }
};

#endif//CPPBAZAARBOT_HISTORY_H
