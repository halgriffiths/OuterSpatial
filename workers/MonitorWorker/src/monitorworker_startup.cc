#include <improbable/standard_library.h>
#include <improbable/worker.h>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>

#include <signal.h>
#include <unistd.h>
#include <atomic>

#include <improbable/restricted/system_components.h>
#include <improbable/standard_library.h>
#include <sample.h>
#include <market.h>
#include <trader.h>

// Use this to make a worker::ComponentRegistry. This worker doesn't use any components yet
// For example use worker::Components<improbable::Position, improbable::Metadata> to track these
// common components
using ComponentRegistry =
    worker::Schema<
      market::MetalMarket,
      market::FoodMarket,
      market::FertilizerMarket,
      market::WoodMarket,
      market::MetalMarket,
      market::OreMarket,
      market::ToolsMarket,
      improbable::AuthorityDelegation>;

// Constants and parameters
const int ErrorExitStatus = 1;
const std::string kLoggerName = "startup.cc";
const std::uint32_t kGetOpListTimeoutInMilliseconds = 100;

// Connection helpers
worker::Connection ConnectWithLocator(const std::string hostname, const std::string login_token,
                                      const worker::ConnectionParameters& connection_parameters) {
  worker::LogsinkParameters logsink_params;
  logsink_params.Type = worker::LogsinkType::kStdout;
  logsink_params.FilterParameters.CustomFilter = [](worker::LogCategory categories,
                                                    worker::LogLevel level) -> bool {
    return level >= worker::LogLevel::kWarn ||
        (level >= worker::LogLevel::kInfo && categories & worker::LogCategory::kLogin);
  };
  worker::LocatorParameters locator_parameters;
  locator_parameters.Logsinks.emplace_back(logsink_params);
  locator_parameters.PlayerIdentity.LoginToken = login_token;
  locator_parameters.UseInsecureConnection = true;

  worker::Locator locator{hostname, locator_parameters};

  auto future = locator.ConnectAsync(ComponentRegistry{}, connection_parameters);
  return future.Get();
}

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

void CreateMonitorEntities(worker::Connection& connection, worker::EntityId base_id) {
  worker::Entity monitor_entity;

  //FoodMarket id = 3010

  // Task: Create an entity which has interest over component id 3010
  improbable::ComponentSetInterest_QueryConstraint my_constraint;
//  my_constraint.set_component_constraint({3010});
  improbable::ComponentSetInterest_Query my_query;
  my_query.set_constraint(my_constraint);

  improbable::ComponentSetInterest my_component_set_interest;
  my_component_set_interest.set_queries({my_query});

  monitor_entity.Add<improbable::Metadata>({{"MonitorEntity"}});
  monitor_entity.Add<improbable::Position>({{3, 0, 0}});
  monitor_entity.Add<improbable::Interest>({{{3010, my_component_set_interest}}});
  monitor_entity.Add<improbable::AuthorityDelegation>({{{base_id + 1, 58}}});

  connection.SendCreateEntityRequest(monitor_entity, base_id, {500});

  worker::Entity monitor_partition_entity;
  monitor_partition_entity.Add<improbable::Metadata>({{"MonitorPartitionEntity"}});
  monitor_partition_entity.Add<improbable::Position>({{4, 0, 0}});

  connection.SendCreateEntityRequest(monitor_partition_entity, base_id+1, {500});
  std::cout << "Create requests sent." << std::endl;
}

std::atomic<bool> quit(false);
void got_signal(int s)
{
  std::cout << "SIG CAUGHT: " << s << std::endl;
  quit.store(true);
}

// Entry point
int main(int argc, char** argv) {
  auto now = std::chrono::high_resolution_clock::now();
  std::srand(
      std::chrono::time_point_cast<std::chrono::nanoseconds>(now).time_since_epoch().count());

  auto print_usage = [&]() {
    std::cout << "Usage: External receptionist <hostname> <port> <worker_id>" << std::endl;
    std::cout << "       External locator <hostname> <login_token>";
    std::cout << std::endl;
    std::cout << "Connects to SpatialOS" << std::endl;
    std::cout << "    <hostname>       - hostname of the receptionist or locator to connect to.";
    std::cout << std::endl;
    std::cout << "    <port>           - port to use if connecting through the receptionist.";
    std::cout << std::endl;
    std::cout << "    <worker_id>      - name of the worker assigned by SpatialOS." << std::endl;
    std::cout << "    <login_token>   - token to use when connecting through the locator.";
    std::cout << std::endl;
  };

  worker::ConnectionParameters parameters;
  parameters.WorkerType = "MonitorWorker";
  parameters.Network.ConnectionType = worker::NetworkConnectionType::kKcp;
  parameters.Network.Kcp.SecurityType = worker::NetworkSecurityType::kInsecure;
  parameters.Network.UseExternalIp = true;

  worker::LogsinkParameters logsink_params;
  logsink_params.Type = worker::LogsinkType::kStdout;
  logsink_params.FilterParameters.CustomFilter = [](worker::LogCategory categories,
                                                    worker::LogLevel level) -> bool {
    return level >= worker::LogLevel::kWarn ||
        (level >= worker::LogLevel::kInfo && categories & worker::LogCategory::kLogin);
  };
  parameters.Logsinks.emplace_back(logsink_params);
  parameters.EnableLoggingAtStartup = true;

  std::vector<std::string> arguments;

  // if no arguments are supplied, use the defaults for a local deployment
  if (argc == 1) {
    arguments = {"receptionist", "localhost", "7777",
                 "MonitorWorker_" + get_random_characters(4)};
  } else {
    arguments = std::vector<std::string>(argv + 1, argv + argc);
  }

  const std::string connection_type = arguments[0];
  if (connection_type != "receptionist" && connection_type != "locator") {
    print_usage();
    return ErrorExitStatus;
  }

  const bool use_locator = connection_type == "locator";

  if ((use_locator && arguments.size() != 5) || (!use_locator && arguments.size() != 4)) {
    print_usage();
    return ErrorExitStatus;
  }

  // Connect with locator or receptionist
  worker::Connection connection = use_locator
      ? ConnectWithLocator(arguments[1], arguments[2], parameters)
      : ConnectWithReceptionist(arguments[1], atoi(arguments[2].c_str()), arguments[3], parameters);

  if (connection.GetConnectionStatusCode() != worker::ConnectionStatusCode::kSuccess) {
    std::cerr << "Worker connection failed: " << connection.GetConnectionStatusDetailString()
              << std::endl;
    return 1;
  }

  connection.SendLogMessage(worker::LogLevel::kInfo, kLoggerName, "Connected successfully");

  // Register callbacks and run the worker main loop.
  worker::Dispatcher dispatcher{ComponentRegistry{}};

  bool is_connected = true;
  dispatcher.OnDisconnect([&](const worker::DisconnectOp& op) {
    std::cerr << "[disconnect] " << op.Reason << std::endl;
    is_connected = false;
  });

  // MY STUFF STARTS HERE
  struct sigaction sa;
  sa.sa_handler = got_signal;
  sigfillset(&sa.sa_mask);
  sigaction(SIGINT,&sa,NULL);

  worker::EntityId monitor_entity_id, monitor_partition_entity_id;
  bool entities_created = false;
  bool ids_reserved = false;
  dispatcher.OnReserveEntityIdsResponse([&](const worker::ReserveEntityIdsResponseOp& op) {
    if (op.StatusCode == worker::StatusCode::kSuccess) {
      monitor_entity_id = *op.FirstEntityId;
      monitor_partition_entity_id = *op.FirstEntityId + 1;
      ids_reserved = true;
    } else {
      std::cout << "Received failure code: " << op.Message << std::endl;
    }
  });
  dispatcher.OnCreateEntityResponse([&](const worker::CreateEntityResponseOp& op) {
    if (op.StatusCode == worker::StatusCode::kSuccess) {
      connection.SendLogMessage(worker::LogLevel::kInfo, "Monitor",
                                "Successfully created entity");
    } else {
      connection.SendLogMessage(worker::LogLevel::kWarn, "Monitor",
                                "Failed to create entity");
    };});
  // Reserve 2 ids
  connection.SendReserveEntityIdsRequest(2, {1000});
  // Create an entity with Interest and AuthorityDelegation components
  // Create a PartitionEntity
  // AssignPartitionCommand to gain entity 1
  while (is_connected) {
    if (quit.load()) return ErrorExitStatus;

    dispatcher.Process(connection.GetOpList(kGetOpListTimeoutInMilliseconds));
    if (ids_reserved) {
      std::cout << "Reserved!" << std::endl;
      ids_reserved = false;
//      CreateMonitorEntities(connection, monitor_entity_id);
      worker::Entity monitor_partition_entity;
      monitor_partition_entity.Add<improbable::Metadata>({{"MonitorPartitionEntity"}});
      monitor_partition_entity.Add<improbable::Position>({{4, 0, 0}});

      connection.SendCreateEntityRequest(monitor_partition_entity, monitor_partition_entity_id, {500});
      std::cout << "Create requests sent with id: #" << monitor_partition_entity_id << std::endl;
    }
    if (entities_created) {
      std::cout << "Created!" << std::endl;
      entities_created = false;
    }
  }

  return ErrorExitStatus;
}
