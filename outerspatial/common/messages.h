//
// Created by henry on 06/12/2021.
//

#ifndef CPPBAZAARBOT_MESSAGES_H
#define CPPBAZAARBOT_MESSAGES_H

// IWY
#include "../traders/inventory.h"
#include "../common/commodity.h"
#include <memory>
#include <utility>

class Trader;

namespace Msg {
    enum MessageType {
        EMPTY,
        REGISTER_REQUEST,
        REGISTER_RESPONSE,
        BID_OFFER,
        ASK_OFFER,
        BID_RESULT,
        ASK_RESULT,
        SHUTDOWN_NOTIFY,
        SHUTDOWN_COMMAND
    };
}

struct EmptyMessage {
    std::string ToString() const {
        std::string output("Empty message");
        return output;
    }
};
struct RegisterRequest {
    int sender_id;
    std::weak_ptr<Trader> trader_pointer;
    RegisterRequest(int sender_id, std::weak_ptr<Trader> new_trader)
            : sender_id(sender_id)
            , trader_pointer(std::move(new_trader)) {};

    std::string ToString() const {
        std::string output("RegistrationRequest from ");
        output.append(std::to_string(sender_id))
                .append(" ");
        return output;
    }
};

struct RegisterResponse {
    int sender_id;
    bool accepted;
    std::optional<std::string> rejection_reason;

    RegisterResponse(int sender_id, bool accepted, std::optional<std::string> reason = std::nullopt)
            : sender_id(sender_id)
            , accepted(accepted)
            , rejection_reason(std::move(reason)) {};

    std::string ToString() const {
        std::string output("RegistrationResponse from ");
        output.append(std::to_string(sender_id))
                .append(": ");
        if (accepted) {
            output.append("OK");
        } else {
            output.append("FAILED - ");
            if (rejection_reason) {
                output.append(*rejection_reason);
            }
        }
        return output;
    }
};

struct BidResult {
    int sender_id;
    std::string commodity;
    bool broker_fee_paid = false;
    int quantity_untraded = 0;
    int quantity_traded = 0;
    double bought_price = 0;
    double original_price = 0;

    BidResult(int sender_id, std::string commodity, double original_price)
            : sender_id(sender_id)
            , commodity(std::move(commodity))
            , original_price(original_price) {};

    void UpdateWithTrade(int trade_quantity, double unit_price) {
        bought_price = (bought_price*quantity_traded + unit_price*trade_quantity)/(trade_quantity + quantity_traded);
        quantity_traded += trade_quantity;
    }

    void UpdateWithNoTrade(int remainder) {
        quantity_untraded += remainder;
    }

    std::string ToString() const {
        std::string output("BID RESULT from ");
        if (quantity_traded > 0) {
            output.append(std::to_string(sender_id))
                    .append(": Bought ")
                    .append(commodity)
                    .append(" x")
                    .append(std::to_string(quantity_traded))
                    .append(" @ avg price $")
                    .append(std::to_string(bought_price))
                    .append(" (")
                    .append(std::to_string(quantity_traded))
                    .append("/")
                    .append(std::to_string(quantity_traded+quantity_untraded))
                    .append(" bought)");
        } else {
            output.append(std::to_string(sender_id))
                    .append(": Failed to buy ")
                    .append(commodity)
                    .append(" (")
                    .append(std::to_string(quantity_traded))
                    .append("/")
                    .append(std::to_string(quantity_traded+quantity_untraded))
                    .append(" bought)");
        }

        return output;
    }
};

struct AskResult {
    int sender_id;
    std::string commodity;
    bool broker_fee_paid = false;
    int quantity_untraded = 0;
    int quantity_traded = 0;
    double avg_price = 0;

    AskResult(int sender_id, std::string commodity)
            : sender_id(sender_id)
            , commodity(std::move(commodity)) {};

    void UpdateWithTrade(int trade_quantity, double unit_price) {
        avg_price = (avg_price*quantity_traded + unit_price*trade_quantity)/(trade_quantity + quantity_traded);
        quantity_traded += trade_quantity;
    }

    void UpdateWithNoTrade(int remainder) {
        quantity_untraded += remainder;
    }

    std::string ToString() const {
        std::string output("ASK RESULT from ");
        if (quantity_traded > 0) {
            output.append(std::to_string(sender_id))
                    .append(": Sold   ")
                    .append(commodity)
                    .append(" x")
                    .append(std::to_string(quantity_traded))
                    .append(" @ avg price $")
                    .append(std::to_string(avg_price))
                    .append(" (")
                    .append(std::to_string(quantity_traded))
                    .append("/")
                    .append(std::to_string(quantity_untraded+quantity_traded))
                    .append(" sold)");
        } else {
            output.append(std::to_string(sender_id))
                    .append(": Failed to sell ")
                    .append(commodity)
                    .append(" (")
                    .append(std::to_string(quantity_traded))
                    .append("/")
                    .append(std::to_string(quantity_untraded+quantity_traded))
                    .append(" sold)");
        }

        return output;
    }
};

struct BidOffer {
    std::uint64_t expiry_ms; //unix time in ns
    int sender_id;
    std::string commodity;
    int quantity;
    double unit_price;
    BidOffer(int sender_id, std::string  commodity_name, int quantity, double unit_price, std::uint64_t expiry_ms = 0)
            : sender_id(sender_id)
            , commodity(std::move(commodity_name))
            , quantity(quantity)
            , unit_price(unit_price)
            , expiry_ms(expiry_ms) {};

    std::string ToString() const {
        std::string output("BID from ");
        output.append(std::to_string(sender_id))
                .append(": ")
                .append(commodity)
                .append(" x")
                .append(std::to_string(quantity))
                .append(" @ $")
                .append(std::to_string(unit_price));
        return output;
    }
};

struct AskOffer {
    std::uint64_t expiry_ms; //unix time in ns
    int sender_id;
    std::string commodity;
    int quantity;
    double unit_price;

    AskOffer(int sender_id, std::string  commodity_name, int quantity, double unit_price, std::uint64_t expiry_ms = 0)
            : sender_id(sender_id)
            , commodity(std::move(commodity_name))
            , quantity(quantity)
            , unit_price(unit_price)
            , expiry_ms(expiry_ms) {};

    std::string ToString() const {
        std::string output("ASK from ");
        output.append(std::to_string(sender_id))
                .append(": ")
                .append(commodity)
                .append(" x")
                .append(std::to_string(quantity))
                .append(" @ $")
                .append(std::to_string(unit_price));
        return output;
    }
};

bool operator< (const BidOffer& a, const BidOffer& b) {
    return a.unit_price < b.unit_price;
}
bool operator< (const AskOffer& a, const AskOffer& b) {
    return a.unit_price > b.unit_price;
}

bool operator< (const BidResult& a, const BidResult& b) {
    return a.original_price < b.original_price;
}
bool operator< (const AskResult& a, const AskResult& b) {
    return a.avg_price > b.avg_price;
}

struct ShutdownNotify {
    int sender_id;

    std::string class_name;
    int age_at_death;
    ShutdownNotify(int sender_id, std::string class_name, int age_at_death)
            : sender_id(sender_id)
            , class_name(std::move(class_name))
            , age_at_death(age_at_death) {};

    std::string ToString() const {
        return std::string("Shutdown notification received ("+class_name+", age "+std::to_string(age_at_death));
    }
};

struct ShutdownCommand {
    int sender_id;
    ShutdownCommand(int sender_id)
            : sender_id(sender_id) {};

    std::string ToString() const {
        return std::string("Shutdown command received");
    }
};

class Message {
public:
    int sender_id; //originator of message
    std::optional<EmptyMessage> empty_message = std::nullopt;
    std::optional<RegisterRequest> register_request = std::nullopt;
    std::optional<RegisterResponse> register_response = std::nullopt;
    std::optional<BidOffer> bid_offer = std::nullopt;
    std::optional<AskOffer> ask_offer = std::nullopt;
    std::optional<BidResult> bid_result = std::nullopt;
    std::optional<AskResult> ask_result = std::nullopt;
    std::optional<ShutdownNotify> shutdown_notify = std::nullopt;
    std::optional<ShutdownCommand> shutdown_command = std::nullopt;

    Message(int sender_id)
        : sender_id(sender_id)
        , type(Msg::EMPTY)
        , empty_message(EmptyMessage()) {};
    
    Msg::MessageType GetType() {
        return type;
    }
    Message* AddRegisterRequest(RegisterRequest msg) {
        if (type != Msg::EMPTY) {
            return this; //disallow multiple messages
        }
        type = Msg::REGISTER_REQUEST;
        register_request = std::move(msg);
        return this;
    }
    Message* AddRegisterResponse(RegisterResponse msg) {
        if (type != Msg::EMPTY) {
            return this; //disallow multiple messages
        }
        type = Msg::REGISTER_RESPONSE;
        register_response = std::move(msg);
        return this;
    }
    Message* AddBidOffer(BidOffer msg) {
        if (type != Msg::EMPTY) {
            return this; //disallow multiple messages
        }
        type = Msg::BID_OFFER;
        bid_offer = std::move(msg);
        return this;
    }
    Message* AddBidResult(BidResult msg) {
        if (type != Msg::EMPTY) {
            return this; //disallow multiple messages
        }
        type = Msg::BID_RESULT;
        bid_result = std::move(msg);
        return this;
    }
    Message* AddAskOffer(AskOffer msg) {
        if (type != Msg::EMPTY) {
            return this; //disallow multiple messages
        }
        type = Msg::ASK_OFFER;
        ask_offer = std::move(msg);
        return this;
    }
    Message* AddAskResult(AskResult msg) {
        if (type != Msg::EMPTY) {
            return this; //disallow multiple messages
        }
        type = Msg::ASK_RESULT;
        ask_result = std::move(msg);
        return this;
    }
    Message* AddShutdownNotify(ShutdownNotify msg) {
        if (type != Msg::EMPTY) {
            return this; //disallow multiple messages
        }
        type = Msg::SHUTDOWN_NOTIFY;
        shutdown_notify = std::move(msg);
        return this;
    }
    Message* AddShutdownCommand(ShutdownCommand msg) {
        if (type != Msg::EMPTY) {
            return this; //disallow multiple messages
        }
        type = Msg::SHUTDOWN_COMMAND;
        shutdown_command = std::move(msg);
        return this;
    }

    std::string ToString() const {
        if (type == Msg::EMPTY) {
            return empty_message->ToString();
        } else if (type == Msg::REGISTER_REQUEST) {
            return register_request->ToString();
        } else if (type == Msg::REGISTER_RESPONSE) {
            return register_response->ToString();
        } else if (type == Msg::BID_OFFER) {
            return bid_offer->ToString();
        } else if (type == Msg::BID_RESULT) {
            return bid_result->ToString();
        } else if (type == Msg::ASK_OFFER) {
            return ask_offer->ToString();
        } else if (type == Msg::ASK_RESULT) {
            return ask_result->ToString();
        } else if (type == Msg::SHUTDOWN_NOTIFY) {
            return shutdown_notify->ToString();
        } else if (type == Msg::SHUTDOWN_COMMAND) {
            return shutdown_command->ToString();
        } else {
            return "ERR: unknown message type";
        }
    }
    
private:
    Msg::MessageType type;
};

#endif//CPPBAZAARBOT_MESSAGES_H
