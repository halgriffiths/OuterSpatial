#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include <improbable/view.h>
#include <improbable/worker.h>

#include <improbable/restricted/system_components.h>
#include <improbable/standard_library.h>
#include <sample.h>
#include <market.h>
#include <trader.h>

#include "../../outerspatial/outerspatial_engine.h"
#include <thread>

// This keeps track of all components and component sets that this worker uses.
// Used to make a worker::ComponentRegistry.
using ComponentRegistry =
worker::Schema<
    market::MetalMarket,
    market::FoodMarket,
    market::FertilizerMarket,
    market::WoodMarket,
    market::MetalMarket,
    market::OreMarket,
    market::ToolsMarket,
    market::RegisterCommandComponent,
    market::MakeOfferCommandComponent,
    market::RequestShutdownComponent,
    market::RequestProductionComponent,
    market::DemographicInfo,
    trader::Inventory,
    trader::AIBuildings,
    trader::ReportOfferResultComponent,
    sample::LoginListenerSet,
    sample::PositionSet,
    improbable::Interest,
    improbable::Position,
    improbable::Metadata,
    improbable::Persistence,
    improbable::AuthorityDelegation,
    improbable::restricted::Worker,
    improbable::restricted::Partition>;

// Constants and parameters
const int ErrorExitStatus = 1;
const std::string kLoggerName = "startup.cc";
const std::uint32_t kGetOpListTimeoutInMilliseconds = 100;

const worker::EntityId listenerEntity = 1;
const worker::EntityId AItraderPartitionId = 4;

worker::Connection
ConnectWithReceptionist(const std::string hostname, const std::uint16_t port,
                        const std::string& worker_id,
                        const worker::ConnectionParameters& connection_parameters) {
  auto future = worker::Connection::ConnectAsync(ComponentRegistry{}, hostname, port, worker_id,
                                                 connection_parameters);
  return future.Get();
}

std::string get_random_characters(size_t count) {
  const auto randchar = []() -> char {
    const char charset[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    const auto max_index = sizeof(charset) - 1;
    return charset[std::rand() % max_index];
  };
  std::string str(count, 0);
  std::generate_n(str.begin(), count, randchar);
  return str;
}

// Entry point
int main(int argc, char** argv) {
  auto now = std::chrono::high_resolution_clock::now();
  std::srand(
      std::chrono::time_point_cast<std::chrono::nanoseconds>(now).time_since_epoch().count());

  std::cout << "[local] Worker started " << std::endl;

  auto print_usage = [&]() {
    std::cout << "Usage: Managed receptionist <hostname> <port> <worker_id>" << std::endl;
    std::cout << std::endl;
    std::cout << "Connects to SpatialOS" << std::endl;
    std::cout << "    <hostname>      - hostname of the receptionist or locator to connect to.";
    std::cout << std::endl;
    std::cout << "    <port>          - port to use if connecting through the receptionist.";
    std::cout << std::endl;
    std::cout << "    <worker_id>     - (optional) name of the worker assigned by SpatialOS."
              << std::endl;
    std::cout << std::endl;
  };

  std::vector<std::string> arguments;

  // if no arguments are supplied, use the defaults for a local deployment
  if (argc == 1) {
    arguments = {"receptionist", "localhost", "7777"};
  } else {
    arguments = std::vector<std::string>(argv + 1, argv + argc);
  }

  if (arguments.size() != 4 && arguments.size() != 3) {
    print_usage();
    return ErrorExitStatus;
  }

  worker::ConnectionParameters parameters;
  parameters.WorkerType = "Managed";
  parameters.Network.ConnectionType = worker::NetworkConnectionType::kTcp;
  parameters.Network.Tcp.SecurityType = worker::NetworkSecurityType::kInsecure;
  parameters.Network.UseExternalIp = false;

  worker::LogsinkParameters logsink_params;
  logsink_params.Type = worker::LogsinkType::kStdout;
  logsink_params.FilterParameters.CustomFilter = [](worker::LogCategory categories,
                                                    worker::LogLevel level) -> bool {
    return level >= worker::LogLevel::kWarn ||
        (level >= worker::LogLevel::kInfo && categories & worker::LogCategory::kLogin);
  };
  parameters.Logsinks.emplace_back(logsink_params);
  parameters.EnableLoggingAtStartup = true;

  std::string workerId;

  // When running as an external worker using 'spatial local worker launch'
  // The WorkerId isn't passed, so we generate a random one
  if (arguments.size() == 4) {
    workerId = arguments[3];
  } else {
    workerId = parameters.WorkerType + "_AITraderWorker_" + get_random_characters(4);
  }

  std::cout << "[local] Connecting to SpatialOS as " << workerId << "..." << std::endl;

  // Connect with receptionist
  worker::Connection connection =
      ConnectWithReceptionist(arguments[1], atoi(arguments[2].c_str()), workerId, parameters);

  if (connection.GetConnectionStatusCode() != worker::ConnectionStatusCode::kSuccess) {
    std::cerr << "Worker connection failed: " << connection.GetConnectionStatusDetailString()
              << std::endl;
    return 1;
  }

  connection.SendLogMessage(worker::LogLevel::kInfo, kLoggerName, "Connected successfully");


  std::unique_ptr<worker::View> view = std::make_unique<worker::View>(ComponentRegistry{});
  bool is_connected = true;
  view->OnDisconnect([&](const worker::DisconnectOp& op) {
    std::cerr << "[disconnect] " << op.Reason << std::endl;
    is_connected = false;
  });

  using AssignPartitionCommand = improbable::restricted::Worker::Commands::AssignPartition;

  // In real code, we would probably want to retry here.
  view->OnCommandResponse<AssignPartitionCommand>(
      [&](const worker::CommandResponseOp<AssignPartitionCommand>& op) {
        if (op.StatusCode == worker::StatusCode::kSuccess) {
          connection.SendLogMessage(worker::LogLevel::kInfo, "Server",
                                    "Successfully assigned partition.");
        } else {
          connection.SendLogMessage(worker::LogLevel::kError, "Server",
                                    "Failed to assign partition: error code : " +
                                        std::to_string(static_cast<std::uint8_t>(op.StatusCode)) +
                                        " message: " + op.Message);
        }
      });

  view->OnAddComponent<improbable::restricted::Worker>(
      [&](worker::AddComponentOp<improbable::restricted::Worker> op) {
        connection.SendLogMessage(worker::LogLevel::kInfo, "Server",
                                  "Worker with ID " + op.Data.worker_id() + " connected.");
      });
  // MY STUFF STARTS HERE

  using RegisterTraderCommand = market::RegisterCommandComponent::Commands::RegisterCommand;
  int ah_id = 10;
  auto rng_gen = std::mt19937(std::random_device()());

  const int TARGET_TICK_TIME_MS = 500;
  int timedelta_ms;

  std::shared_ptr<AITrader> ai_trader_ptr;
  messages::AIRole requested_role;

  while (is_connected) {
    requested_role = ChooseNewClassRandom(rng_gen);
    messages::RegisterRequest reg_req{messages::AgentType::AI_TRADER, requested_role};
    connection.SendCommandRequest<RegisterTraderCommand>(ah_id, reg_req, {1000});
    ai_trader_ptr = std::make_shared<AITrader>(connection, *view, ah_id, requested_role, 1000, Log::INFO);
    connection.SendLogMessage(worker::LogLevel::kInfo, "AITraderWorkerStartup", "Creating new trader of type: " + RoleToString(requested_role));
    auto last_tick_time = std::chrono::steady_clock::now();
    while (ai_trader_ptr->status != DESTROYED) {
      view->Process(connection.GetOpList(kGetOpListTimeoutInMilliseconds));

      ai_trader_ptr->PrintInventory();
      ai_trader_ptr->TickOnce();

      auto t_now = std::chrono::steady_clock::now();
      timedelta_ms = std::chrono::duration<double, std::milli>(t_now - last_tick_time)
          .count();  // Amount of time since last tick, in seconds
      while (timedelta_ms < TARGET_TICK_TIME_MS) {
        view->Process(connection.GetOpList(kGetOpListTimeoutInMilliseconds));
        t_now = std::chrono::steady_clock::now();
        timedelta_ms = std::chrono::duration<double, std::milli>(t_now - last_tick_time)
            .count();  // Amount of time since last tick, in milliseconds
      }
      last_tick_time = t_now;
    }

    std::cout << "Worker destroyed! Creating replacement..." << std::endl;
    //terrible hack to give "shutdown confirmed" message time to send
    ai_trader_ptr.reset();
    connection.AlphaFlush();

    view.reset();
    view = std::make_unique<worker::View>(ComponentRegistry{});
    view->OnDisconnect([&](const worker::DisconnectOp& op) {
      std::cerr << "[disconnect] " << op.Reason << std::endl;
      is_connected = false;
    });
  }
  return ErrorExitStatus;
}
