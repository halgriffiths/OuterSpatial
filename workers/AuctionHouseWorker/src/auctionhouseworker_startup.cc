#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>

#include <improbable/view.h>
#include <improbable/worker.h>

// Schema includes
#include <improbable/restricted/system_components.h>
#include <improbable/standard_library.h>
#include <sample.h>
#include <market.h>
#include <trader.h>

#include <thread>
#include <string>

#include "../../outerspatial/outerspatial_engine.h"

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
        market::DemographicInfo,
        market::RegisterCommandComponent,
        market::MakeOfferCommandComponent,
        market::RequestShutdownComponent,
        market::RequestProductionComponent,
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
const std::uint32_t kCreateEntityTimeoutInMilliseconds = 500;

const worker::EntityId auctionhousePartitionId = 3;

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
    workerId = parameters.WorkerType + "_AuctionHouseWorker_" + get_random_characters(4);
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

  worker::View view{ComponentRegistry{}};

  bool is_connected = true;
  view.OnDisconnect([&](const worker::DisconnectOp& op) {
    std::cerr << "[disconnect] " << op.Reason << std::endl;
    is_connected = false;
  });

  using AssignPartitionCommand = improbable::restricted::Worker::Commands::AssignPartition;
  // In real code, we would probably want to retry here.
  view.OnCommandResponse<AssignPartitionCommand>(
      [&](const worker::CommandResponseOp<AssignPartitionCommand>& op) {
        if (op.StatusCode == worker::StatusCode::kSuccess) {
          connection.SendLogMessage(worker::LogLevel::kInfo, "AuctionHouse",
                                    "Successfully assigned partition.");
        } else {
          connection.SendLogMessage(worker::LogLevel::kError, "AuctionHouse",
                                    "Failed to assign partition: error code : " +
                                        std::to_string(static_cast<std::uint8_t>(op.StatusCode)) +
                                        " message: " + op.Message);
        }
      });
  view.OnAddComponent<improbable::restricted::Worker>(
      [&](worker::AddComponentOp<improbable::restricted::Worker> op) {
        connection.SendLogMessage(worker::LogLevel::kInfo, "AuctionHouse",
                                  "Worker with ID " + op.Data.worker_id() + " created component.");
      });
  view.OnCreateEntityResponse([&](const worker::CreateEntityResponseOp& op) {
    if (op.StatusCode == worker::StatusCode::kSuccess) {
      connection.SendLogMessage(worker::LogLevel::kInfo, "AuctionHouse",
                                "Successfully created entity");
    } else {
      connection.SendLogMessage(worker::LogLevel::kWarn, "AuctionHouse",
                                "Failed to create entity");
    };});
  // MY STUFF STARTS HERE

  connection.SendCommandRequest<AssignPartitionCommand>(
      connection.GetWorkerEntityId(), {auctionhousePartitionId}, /* default timeout */ {});
  auto AH_ptr = std::make_shared<AuctionHouse>(connection, view, 10, Log::INFO);
  auto last_tick_time = std::chrono::steady_clock::now();
  Commodity food("food", 0.5, 3010);
  Commodity wood("wood", 1, 3011);
  Commodity fertilizer("fertilizer", 0.1, 3012);
  Commodity ore("ore", 1, 3013);
  Commodity metal("metal", 1, 3014);
  Commodity tools("tools", 1, 3015);

  AH_ptr->RegisterCommodity(food);
  AH_ptr->RegisterCommodity(wood);
  AH_ptr->RegisterCommodity(fertilizer);
  AH_ptr->RegisterCommodity(ore);
  AH_ptr->RegisterCommodity(metal);
  AH_ptr->RegisterCommodity(tools);

  const int TARGET_TICK_TIME_MS = 500;
  int timedelta_ms;
  while (is_connected) {
    view.Process(connection.GetOpList(kGetOpListTimeoutInMilliseconds));

    AH_ptr->TickOnce();
    auto t_now = std::chrono::steady_clock::now();
    timedelta_ms = std::chrono::duration<double, std::milli>(t_now - last_tick_time)
                        .count();  // Amount of time since last tick, in milliseconds
    if (timedelta_ms < TARGET_TICK_TIME_MS) {
      std::this_thread::sleep_for(std::chrono::milliseconds{TARGET_TICK_TIME_MS - timedelta_ms});
    } else {
      std::cout << "Overran on tick! (took " + std::to_string(timedelta_ms) + " ms)" << std::endl;
    }
    last_tick_time = t_now;
  }

  return ErrorExitStatus;
}
