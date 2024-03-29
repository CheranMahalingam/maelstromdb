#include "query.h"

namespace cli {

Query::Query() {
}

void Query::Parse(int argc, char* argv[]) {
  static struct option long_options[] = {
    {"cluster", required_argument, NULL, 'c'},
    {"help", no_argument, NULL, 'h'},
  };

  std::vector<std::string> cluster;
  std::string command;
  while (true) {
    int c = getopt_long(argc, argv, "c:h", long_options, NULL);

    if (c == -1) {
      break;
    }

    switch (c) {
      case 'c':
        cluster = SplitCommaSeparated(optarg);
        break;
      case 'h':
        Help();
        exit(0);
      default:
        std::cerr << "Invalid option provided " << c << "\n";
        Help();
        exit(1);
    }
  }

  optind++;
  if (optind == argc) {
    std::cerr << "Expected additional argument to be provided\n";
    Help();
    exit(1);
  }
  command = argv[optind];

  Execute(cluster, command);
}

void Query::Help() {
}

void Query::Execute(std::vector<std::string>& addresses, std::string command) {
  std::cout << "Attempting to create read-only query...\n";
  raft::LeaderProxy proxy(addresses);
  protocol::raft::ClientQuery_Response reply;
  auto status = proxy.ClientQuery(command, reply);

  std::cout << "Query successful? " << (status.ok() ? "Yes" : "No") << "\n";
  if (status.ok()) {
    std::cout << "Query response: " << reply.response() << "\n";
  } else {
    std::cout << "Query error: " << status.error_message() << "\n";
  }
}

}

