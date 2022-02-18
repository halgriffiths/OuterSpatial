
//
// Created by henry on 06/12/2021.
//

#ifndef CPPBAZAARBOT_LOGGER_H
#define CPPBAZAARBOT_LOGGER_H

#include <utility>
#include <iomanip>
#include <iostream>
#include <ostream>

namespace Log{
    enum LogLevel {
        SILENT,
        ERROR,
        WARN,
        INFO, //Received messages (but not sent message) are reported
        DEBUG //Everything is logged
    };
}

// Base class makes no logs
class Logger {
protected:
    std::string name;
public:
    Log::LogLevel verbosity;
    Logger(Log::LogLevel verbosity, std::string name)
        : verbosity(verbosity)
        , name(name) {};

    virtual void LogInternal(std::string raw_message) const {}; //no-op
    void LogSent(int to, Log::LogLevel level, std::string message) const {
        if (level > verbosity) {
            return;
        }
        char header_string[100]; //yolo lmao
        snprintf(header_string, 100, "[%s][Sent    ] %s >>> %d - ",LogLevelToCString(level), name.c_str(), to);
        LogInternal(std::string(header_string) + message);
    }

    void LogReceived(int from, Log::LogLevel level, std::string message) const {
        if (level > verbosity) {
            return;
        }
        char header_string[100]; //yolo lmao
        snprintf(header_string, 100, "[%s][Received] %s <<< %d - ", LogLevelToCString(level), name.c_str(), from);
        LogInternal(std::string(header_string) + message);
    }

    void Log(Log::LogLevel level, std::string message) const {
        if (level > verbosity) {
            return;
        }
        char header_string[100]; //yolo lmao
        snprintf(header_string, 100, "[%s] %s: ",LogLevelToCString(level), name.c_str());
        LogInternal(std::string(header_string) + message);
    }


private:
    const char* LogLevelToCString(Log::LogLevel level) const {
        const char* level_name;
        if (level == Log::ERROR) {
            level_name = "ERROR";
        } else if (level == Log::WARN) {
            level_name = "WARN";
        } else if (level == Log::INFO) {
            level_name = "INFO";
        } else if (level == Log::DEBUG) {
            level_name = "DEBUG";
        } else {
            level_name = "UNKNOWN";
        }
        return level_name;
    }
};
class SpatialLogger : public Logger {
  worker::Connection& conn;
public:
  SpatialLogger(Log::LogLevel verbosity, std::string name, worker::Connection& connection)
      : Logger(verbosity, name)
       , conn(connection){ };

  void LogInternal(std::string raw_message) const override {
    raw_message += "\n";
    std::fwrite(raw_message.c_str(), 1, raw_message.size()+1, stdout);
    conn.SendLogMessage(worker::LogLevel::kWarn, name, raw_message);
  }
};
class ConsoleLogger : public Logger {
public:
    ConsoleLogger(Log::LogLevel verbosity, std::string name)
        : Logger(verbosity, name) {
      };

    void LogInternal(std::string raw_message) const override {
        raw_message += "\n";
        std::fwrite(raw_message.c_str(), 1, raw_message.size()+1, stdout);
    }
};

class FileLogger : public Logger {
private:
    FILE * log_file;
public:
    FileLogger(Log::LogLevel verbosity, std::string unique_name)
        : Logger(verbosity, unique_name) {
        //keep file open since we log frequently
        //TODO: Programmatically find relative path
        log_file = std::fopen (("/home/henry/OuterSpatial/outerspatial/logs/" + unique_name + "_log.txt").c_str(), "w");
        std::fwrite("# Log file\n", 1, 11, log_file);
    };
    FileLogger(FileLogger& logger)
      : Logger(logger.verbosity, logger.name) {
      log_file = std::fopen (("/home/henry/OuterSpatial/outerspatial/logs/" + logger.name + "_log.txt").c_str(), "w");
    }

    ~FileLogger() {
      std::fclose(log_file);
    }
    void LogInternal(std::string raw_message) const override {
        raw_message += "\n";
        std::fwrite(raw_message.c_str(), 1, raw_message.size(), log_file);
    }
};

#endif//CPPBAZAARBOT_LOGGER_H
