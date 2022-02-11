//
// Created by henry on 07/12/2021.
//

#ifndef CPPBAZAARBOT_AGENT_H
#define CPPBAZAARBOT_AGENT_H

#include "messages.h"
#include <memory>
#include <utility>

class AuctionHouse;
// All an agent is is an entity with an id, capable of sending and receiving messages
class Agent {
protected:
    worker::Connection& connection;
    worker::View& view;
public:
    int id;
    int ticks = 0;
    Agent(int agent_id, worker::Connection& connection, worker::View& view)
      : connection(connection)
      , view(view)
      , id(agent_id) {};
    virtual ~Agent() = default;
    void ReceiveMessage(const Message incoming_message) {
        //TODO: Remove this stub (and this class?)
    }
    void SendMessage(Message& outgoing_message, int recipient) {
      //TODO: Remove this stub
    }
  protected:
    bool IsConnected() {
      return (connection.GetConnectionStatusCode() == worker::ConnectionStatusCode::kSuccess);
    }
    //Set up callback functions on 'view'
    virtual void MakeCallbacks() {};

};

// A Trader is an Agent capable of interacting with an AuctionHouse
class Trader : public Agent {
public:
    Trader(int id, std::string name, worker::Connection& connection, worker::View& view)
        : Agent(id, connection, view)
        , class_name(std::move(name)){};
    ~Trader() override = default;

    virtual bool HasMoney(double quantity) {return false;};
    virtual bool HasCommodity(const std::string& commodity, int quantity) {return false;};

    std::string GetClassName() {return class_name;};
protected:
    friend AuctionHouse;
    std::string class_name;
    virtual double TryTakeMoney(double quantity, bool atomic) {};
    virtual void ForceTakeMoney(double quantity) {};
    virtual void AddMoney(double quantity) {};
    virtual int TryAddCommodity(const std::string& commodity, int quantity, std::optional<double> unit_price, bool atomic) {return 0;};
    virtual int TryTakeCommodity(const std::string& commodity, int quantity, std::optional<double> unit_price, bool atomic) {return 0;};
};
#endif//CPPBAZAARBOT_AGENT_H
