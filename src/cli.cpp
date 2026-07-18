#include "nodeset.h"

#include "opcua_client.h"

#include <boost/json.hpp>

#include <chrono>
#include <exception>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

struct ParsedArgs {
  std::string command;
  std::vector<std::string> positionals;
  std::map<std::string, std::string> options;
  SecurityOptions security;
  bool json = false;
};

void PrintUsage() {
  std::cout
      << "opcua-cli " << "0.1.0" << "\n\n"
      << "Commands:\n"
      << "  browse ENDPOINT [NODEID|/Objects] [--recursive] [--depth=N]\n"
      << "  read ENDPOINT NODEID [--attribute=Value]\n"
      << "  write ENDPOINT NODEID VALUE [--type=Int32]\n"
      << "  endpoints ENDPOINT\n"
      << "  watch ENDPOINT NODEID [--interval=N]\n"
      << "  generate:nodeset FILE [--output=generated] "
         "[--namespace=Generated::OpcUa]\n"
      << "  dump:nodeset ENDPOINT --output=FILE [--namespace=N]\n\n"
      << "Global options: --json, --debug, --debug-stderr, --debug-file=PATH,\n"
      << "  --security-policy=POLICY, --security-mode=MODE, --cert=PATH, "
         "--key=PATH,\n"
      << "  --ca=PATH, --username=USER, --password=PASS, --timeout=SECONDS\n";
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

ParsedArgs Parse(int argc, char** argv) {
  ParsedArgs parsed;
  if (argc < 2) {
    parsed.options["help"] = "true";
    return parsed;
  }
  parsed.command = argv[1];
  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--json" || arg == "-j") {
      parsed.json = true;
    } else if (arg == "--recursive" || arg == "--debug" || arg == "-d" ||
               arg == "--debug-stderr") {
      parsed.options[arg.substr(arg.rfind('-') + 1)] = "true";
    } else if (arg == "-u" || arg == "-p" || arg == "-s" || arg == "-m" ||
               arg == "-t") {
      if (++i >= argc)
        throw std::runtime_error("Missing value for " + arg);
      std::string key = arg == "-u"   ? "username"
                        : arg == "-p" ? "password"
                        : arg == "-s" ? "security-policy"
                        : arg == "-m" ? "security-mode"
                                      : "timeout";
      parsed.options[key] = argv[i];
    } else if (StartsWith(arg, "--")) {
      auto eq = arg.find('=');
      if (eq == std::string::npos) {
        parsed.options[arg.substr(2)] = "true";
      } else {
        parsed.options[arg.substr(2, eq - 2)] = arg.substr(eq + 1);
      }
    } else {
      parsed.positionals.push_back(arg);
    }
  }

  auto get = [&](const std::string& key, const std::string& fallback = "") {
    auto it = parsed.options.find(key);
    return it == parsed.options.end() ? fallback : it->second;
  };
  parsed.security.policy = get("security-policy", "None");
  parsed.security.mode = get("security-mode", "None");
  parsed.security.cert = get("cert");
  parsed.security.key = get("key");
  parsed.security.ca = get("ca");
  parsed.security.username = get("username");
  parsed.security.password = get("password");
  parsed.security.debug =
      parsed.options.count("debug") > 0 || parsed.options.count("d") > 0;
  parsed.security.debug_stderr = parsed.options.count("debug-stderr") > 0;
  parsed.security.debug_file = get("debug-file");
  if (parsed.options.count("timeout")) {
    parsed.security.timeout_seconds = std::stod(parsed.options["timeout"]);
  }
  return parsed;
}

std::string GetOption(const ParsedArgs& args,
                      const std::string& name,
                      const std::string& fallback = "") {
  auto it = args.options.find(name);
  return it == args.options.end() ? fallback : it->second;
}

void RequirePositionals(const ParsedArgs& args, std::size_t count) {
  if (args.positionals.size() < count) {
    throw std::runtime_error("Command '" + args.command +
                             "' expects at least " + std::to_string(count) +
                             " positional argument(s)");
  }
}

boost::json::object ReadToJson(const ReadResult& result) {
  return {
      {"NodeId", result.node_id},
      {"Attribute", result.attribute},
      {"Value", result.value},
      {"Type", result.type},
      {"Status", result.status},
      {"Source", result.source_timestamp},
      {"Server", result.server_timestamp},
  };
}

boost::json::object BrowseToJson(const BrowseEntry& entry) {
  boost::json::array children;
  for (const auto& child : entry.children) {
    children.push_back(BrowseToJson(child));
  }
  return {
      {"name", entry.name},
      {"nodeId", entry.node_id},
      {"nodeClass", entry.node_class},
      {"children", std::move(children)},
  };
}

boost::json::object WriteToJson(const WriteResult& result) {
  return {
      {"NodeId", result.node_id},
      {"Value", result.value},
      {"Type", result.type},
      {"Status", result.status},
  };
}

boost::json::object EndpointToJson(const EndpointInfo& endpoint) {
  boost::json::array tokens;
  for (const auto& token : endpoint.user_token_policies) {
    tokens.push_back(boost::json::value(token));
  }
  return {
      {"endpoint", endpoint.endpoint_url},
      {"security", endpoint.security_policy},
      {"mode", endpoint.security_mode},
      {"auth", std::move(tokens)},
  };
}

void PrintBrowseTree(const std::vector<BrowseEntry>& entries,
                     const std::string& prefix = "") {
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const bool last = i + 1 == entries.size();
    const auto& entry = entries[i];
    std::cout << prefix << (last ? "`-- " : "|-- ") << entry.name << " ("
              << entry.node_id << ") [" << entry.node_class << "]\n";
    PrintBrowseTree(entry.children, prefix + (last ? "    " : "|   "));
  }
}

void PrintRead(const ReadResult& result) {
  std::cout << "NodeId:     " << result.node_id << "\n"
            << "Attribute:  " << result.attribute << "\n"
            << "Value:      " << result.value << "\n"
            << "Type:       " << result.type << "\n"
            << "Status:     " << result.status << "\n";
  if (!result.source_timestamp.empty())
    std::cout << "Source:     " << result.source_timestamp << "\n";
  if (!result.server_timestamp.empty())
    std::cout << "Server:     " << result.server_timestamp << "\n";
}

void PrintEndpoints(const std::vector<EndpointInfo>& endpoints) {
  for (const auto& endpoint : endpoints) {
    std::cout << "Endpoint: " << endpoint.endpoint_url << "\n"
              << "Security: " << endpoint.security_policy
              << " (mode: " << endpoint.security_mode << ")\n"
              << "Auth:     ";
    for (std::size_t i = 0; i < endpoint.user_token_policies.size(); ++i) {
      if (i != 0)
        std::cout << ", ";
      std::cout << endpoint.user_token_policies[i];
    }
    std::cout << "\n\n";
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    ParsedArgs args = Parse(argc, argv);
    if (args.command == "--help" || args.command == "-h" ||
        args.options.count("help") > 0) {
      PrintUsage();
      return 0;
    }
    if (args.command == "--version" || args.command == "-v") {
      std::cout << "opcua-cli 0.1.0\n";
      return 0;
    }

    if (args.command == "generate:nodeset") {
      RequirePositionals(args, 1);
      GenerateNodeset(args.positionals[0],
                      GetOption(args, "output", "generated"),
                      GetOption(args, "namespace", "Generated::OpcUa"));
      std::cout << "Generated: " << GetOption(args, "output", "generated")
                << "/NodeIds.h\n";
      return 0;
    }

    if (args.command == "dump:nodeset") {
      RequirePositionals(args, 1);
      std::string output = GetOption(args, "output");
      if (output.empty())
        throw std::runtime_error("dump:nodeset requires --output=FILE");
      std::optional<int> ns;
      if (args.options.count("namespace"))
        ns = std::stoi(args.options["namespace"]);
      DumpNodesetPlaceholder(output, ns);
      std::cout << "Written: " << output << "\n";
      return 0;
    }

    OpcuaClient client(args.security);
    if (args.command == "endpoints") {
      RequirePositionals(args, 1);
      auto endpoints = client.Endpoints(args.positionals[0]);
      if (args.json) {
        boost::json::array values;
        for (const auto& endpoint : endpoints) {
          values.push_back(EndpointToJson(endpoint));
        }
        std::cout << boost::json::serialize(values) << "\n";
      } else {
        PrintEndpoints(endpoints);
      }
      return 0;
    }

    RequirePositionals(args, 1);
    client.Connect(args.positionals[0]);

    if (args.command == "browse") {
      std::string target =
          args.positionals.size() > 1 ? args.positionals[1] : "/Objects";
      bool recursive = args.options.count("recursive") > 0;
      int depth = std::stoi(GetOption(args, "depth", "3"));
      auto entries = client.Browse(target, recursive, depth);
      if (args.json) {
        boost::json::array values;
        for (const auto& entry : entries)
          values.push_back(BrowseToJson(entry));
        std::cout << boost::json::serialize(values) << "\n";
      } else {
        PrintBrowseTree(entries);
      }
    } else if (args.command == "read") {
      RequirePositionals(args, 2);
      auto result = client.Read(args.positionals[1],
                                GetOption(args, "attribute", "Value"));
      if (args.json) {
        std::cout << boost::json::serialize(ReadToJson(result)) << "\n";
      } else {
        PrintRead(result);
      }
    } else if (args.command == "write") {
      RequirePositionals(args, 3);
      auto result = client.Write(args.positionals[1], args.positionals[2],
                                 GetOption(args, "type"));
      if (args.json) {
        std::cout << boost::json::serialize(WriteToJson(result)) << "\n";
      } else {
        std::cout << "NodeId:  " << result.node_id << "\n"
                  << "Value:   " << result.value << "\n"
                  << "Type:    " << result.type << "\n"
                  << "Status:  " << result.status << "\n";
      }
      if (result.bad) {
        return 1;
      }
    } else if (args.command == "watch") {
      RequirePositionals(args, 2);
      auto interval = static_cast<std::uint64_t>(
          std::stoull(GetOption(args, "interval", "1000")));
      while (true) {
        auto result = client.Read(args.positionals[1], "Value");
        // Flush per tick: with stdout on a pipe the stream is block-buffered,
        // and an endless watch would otherwise show nothing for minutes.
        if (args.json) {
          std::cout << boost::json::serialize(ReadToJson(result)) << std::endl;
        } else {
          std::cout << result.value << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(interval));
      }
    } else {
      throw std::runtime_error("Unknown command: " + args.command);
    }
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }
  return 0;
}
