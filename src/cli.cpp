#include "nodeset.h"

#include "opcua_client.h"

#include <boost/json.hpp>

#include <chrono>
#include <cstdint>
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
      << "  subscribe ENDPOINT NODEID[,NODEID...] [--duration=SECONDS]\n"
      << "  watch ENDPOINT NODEID [--interval=MS] [--duration=SECONDS] "
         "[--count=N]\n"
      << "  events ENDPOINT [NODEID|i=2253] [--duration=SECONDS] [--count=N] "
         "[--select=FIELD,...]\n"
      << "  generate:nodeset FILE [--output=generated] "
         "[--namespace=Generated::OpcUa]\n"
      << "  dump:nodeset ENDPOINT --output=FILE [--namespace=N] "
         "[--root=NODEID] [--max-nodes=N] [--values]\n\n"
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
    } else if (arg == "-d") {
      parsed.options["debug"] = "true";
      // Long boolean flags deliberately fall through to the generic "--" case
      // below, which strips exactly the two leading dashes. They used to be
      // special-cased with substr(rfind('-') + 1), which keeps only the text
      // after the *last* hyphen: --debug-stderr was stored as "stderr", so the
      // lookup for "debug-stderr" never matched and the flag did nothing at
      // all. Any hyphenated flag added to that list would have broken the same
      // way, so the special case is gone rather than patched.
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

// Self-termination limits shared by `watch` and `events`: --duration is
// wall-clock seconds and --count is a sample/event count, whichever is reached
// first ends the run, and with neither it runs until interrupted. Deliberately
// separate from --timeout, which is the connect/request timeout.
std::optional<double> ParseDuration(const ParsedArgs& args) {
  auto it = args.options.find("duration");
  return it == args.options.end()
             ? std::nullopt
             : std::optional<double>(std::stod(it->second));
}

std::optional<std::uint64_t> ParseCount(const ParsedArgs& args) {
  auto it = args.options.find("count");
  return it == args.options.end()
             ? std::nullopt
             : std::optional<std::uint64_t>(std::stoull(it->second));
}

void RequirePositionals(const ParsedArgs& args, std::size_t count) {
  if (args.positionals.size() < count) {
    throw std::runtime_error("Command '" + args.command +
                             "' expects at least " + std::to_string(count) +
                             " positional argument(s)");
  }
}

boost::json::object ReadToJson(const ReadResult& result) {
  boost::json::object object{
      // Value is the typed rendering: a scalar stays a scalar, an array is a
      // real JSON array. Consumers can index it instead of parsing text.
      {"NodeId", result.node_id},          {"Attribute", result.attribute},
      {"Value", result.json_value},        {"Type", result.type},
      {"Status", result.status},           {"Source", result.source_timestamp},
      {"Server", result.server_timestamp},
  };
  if (result.elements) {
    object["Count"] = result.elements->size();
  }
  if (!result.hint.empty()) {
    object["Hint"] = result.hint;
  }
  return object;
}

boost::json::object BrowseToJson(const BrowseEntry& entry) {
  boost::json::array children;
  for (const auto& child : entry.children) {
    children.push_back(BrowseToJson(child));
  }
  boost::json::object object{
      {"name", entry.name},
      {"nodeId", entry.node_id},
      {"nodeClass", entry.node_class},
      {"children", std::move(children)},
  };
  if (!entry.child_status.empty()) {
    // Present only when the sub-browse failed, so `children: []` alone never
    // has to carry two meanings.
    object["childStatus"] = entry.child_status;
  }
  return object;
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
  boost::json::array discovery_urls;
  for (const auto& url : endpoint.discovery_urls) {
    discovery_urls.push_back(boost::json::value(url));
  }
  return {
      {"endpoint", endpoint.endpoint_url},
      {"security", endpoint.security_policy},
      {"mode", endpoint.security_mode},
      {"auth", std::move(tokens)},
      {"discovery", std::move(discovery_urls)},
  };
}

void PrintBrowseTree(const std::vector<BrowseEntry>& entries,
                     const std::string& prefix = "") {
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const bool last = i + 1 == entries.size();
    const auto& entry = entries[i];
    std::cout << prefix << (last ? "`-- " : "|-- ") << entry.name << " ("
              << entry.node_id << ") [" << entry.node_class << "]";
    if (!entry.child_status.empty()) {
      // A refused sub-browse would otherwise look like a childless node.
      std::cout << "  !! browse failed: " << entry.child_status;
    }
    std::cout << "\n";
    PrintBrowseTree(entry.children, prefix + (last ? "    " : "|   "));
  }
}

void PrintRead(const ReadResult& result) {
  std::cout << "NodeId:     " << result.node_id << "\n"
            << "Attribute:  " << result.attribute << "\n";
  if (result.elements) {
    // Arrays are listed one element per line with the index, because for the
    // arrays worth reading — NamespaceArray above all — element N *is*
    // index N, and that mapping is the whole point of the read.
    std::cout << "Value:      " << result.type << "[" << result.elements->size()
              << "]\n";
    for (std::size_t i = 0; i < result.elements->size(); ++i) {
      std::cout << "  [" << i << "] " << (*result.elements)[i] << "\n";
    }
  } else {
    std::cout << "Value:      " << result.value << "\n";
  }
  std::cout << "Type:       " << result.type << "\n"
            << "Status:     " << result.status << "\n";
  if (!result.source_timestamp.empty())
    std::cout << "Source:     " << result.source_timestamp << "\n";
  if (!result.server_timestamp.empty())
    std::cout << "Server:     " << result.server_timestamp << "\n";
  if (!result.hint.empty())
    std::cout << "Hint:       " << result.hint << "\n";
}

// Splits a comma-separated option value ("EventId,Message") into its entries.
std::vector<std::string> SplitList(const std::string& value) {
  std::vector<std::string> entries;
  std::size_t start = 0;
  while (true) {
    const auto comma = value.find(',', start);
    std::string entry = value.substr(
        start, comma == std::string::npos ? std::string::npos : comma - start);
    if (!entry.empty()) {
      entries.push_back(std::move(entry));
    }
    if (comma == std::string::npos) {
      return entries;
    }
    start = comma + 1;
  }
}

// Event lines are space-separated Name=Value pairs, so a value carrying a
// space (a Message, typically) is quoted and escaped to keep the pairs
// separable. Hex EventIds and NodeIds never trip this.
std::string QuoteIfNeeded(const std::string& value) {
  if (value.find_first_of(" \t\n\"\\") == std::string::npos) {
    return value;
  }
  std::string out = "\"";
  for (const char character : value) {
    switch (character) {
      case '"':
      case '\\':
        out += '\\';
        out += character;
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += character;
    }
  }
  return out + "\"";
}

void PrintEvent(const EventNotification& event) {
  std::string line;
  for (const auto& field : event.fields) {
    if (!line.empty()) {
      line += ' ';
    }
    line += field.name + "=" + QuoteIfNeeded(field.value);
  }
  // Flush per event: with stdout on a pipe the stream is block-buffered, and a
  // long subscription would otherwise show nothing for minutes.
  std::cout << line << std::endl;
}

boost::json::object EventToJson(const EventNotification& event) {
  boost::json::object fields;
  for (const auto& field : event.fields) {
    // Typed: numbers stay numbers, and a field the server sent with no value
    // is JSON null rather than an empty string, so `jq 'select(.Fields.EventId
    // == null)'` finds exactly the events this command exists to catch.
    fields[field.name] = field.json_value;
  }
  return {
      {"Seq", event.sequence},
      {"Fields", std::move(fields)},
  };
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
    std::cout << "\n";
    // The address a reconnecting client is sent to, which need not be the one
    // it dialled (OPC UA Part 4 §7.2 ApplicationDescription.discoveryUrls).
    if (!endpoint.discovery_urls.empty()) {
      std::cout << "Discovery: ";
      for (std::size_t i = 0; i < endpoint.discovery_urls.size(); ++i) {
        if (i != 0)
          std::cout << ", ";
        std::cout << endpoint.discovery_urls[i];
      }
      std::cout << "\n";
    }
    std::cout << "\n";
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
      const std::string root = GetOption(args, "root", "i=84");
      const std::size_t max_nodes = static_cast<std::size_t>(
          std::stoull(GetOption(args, "max-nodes", "100000")));
      const bool include_values = args.options.count("values") > 0;

      OpcuaClient client(args.security);
      client.Connect(args.positionals[0]);
      NodesetDump dump =
          client.DumpNodeset(root, ns, max_nodes, include_values);
      WriteNodeset(output, dump);
      std::cout << "Written: " << output << " (" << dump.nodes.size()
                << " nodes";
      if (dump.nodes.size() >= max_nodes)
        std::cout << ", stopped at --max-nodes";
      std::cout << ")\n";
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
      auto result = client.Browse(target, recursive, depth);
      if (args.json) {
        boost::json::array values;
        for (const auto& entry : result.entries)
          values.push_back(BrowseToJson(entry));
        if (result.bad) {
          // Object rather than the bare array only on failure, so the shape
          // scripts already parse is unchanged when the browse succeeds.
          boost::json::object failure{
              {"nodes", std::move(values)},
              {"Status", result.status},
          };
          if (!result.hint.empty())
            failure["Hint"] = result.hint;
          std::cout << boost::json::serialize(failure) << "\n";
        } else {
          std::cout << boost::json::serialize(values) << "\n";
        }
      } else {
        PrintBrowseTree(result.entries);
        if (result.bad) {
          // On stderr: an empty stdout is the honest answer for a failed
          // browse, and scripts piping stdout must not pick up the diagnosis
          // as if it were a node.
          std::cerr << "error: browse failed: " << result.status << "\n";
          if (!result.hint.empty())
            std::cerr << "hint:  " << result.hint << "\n";
        }
      }
      if (result.bad) {
        return 1;
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
      const auto interval = std::chrono::milliseconds(
          std::stoull(GetOption(args, "interval", "1000")));
      using Clock = std::chrono::steady_clock;
      std::optional<Clock::time_point> deadline;
      if (const auto seconds = ParseDuration(args)) {
        deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                      std::chrono::duration<double>(*seconds));
      }
      const std::optional<std::uint64_t> count = ParseCount(args);

      for (std::uint64_t taken = 0; !count || taken < *count; ++taken) {
        auto result = client.Read(args.positionals[1], "Value");
        // Flush per tick: with stdout on a pipe the stream is block-buffered,
        // and a long watch would otherwise show nothing for minutes.
        if (args.json) {
          std::cout << boost::json::serialize(ReadToJson(result)) << std::endl;
        } else {
          std::cout << result.value << std::endl;
        }
        const auto next = Clock::now() + interval;
        // Stop rather than sleep past the deadline, so --duration bounds the
        // total runtime instead of rounding up to the next whole interval.
        if (deadline && next >= *deadline) {
          break;
        }
        std::this_thread::sleep_until(next);
      }
    } else if (args.command == "subscribe") {
      // One subscription, many items, per-item delivery counts.
      //
      // `watch` cannot answer this: it polls Read and never subscribes, so it
      // reports that a node is readable, not that the server publishes it. A
      // node can also be delivered fine on a subscription of its own and
      // starved when it shares one, which is why every node here goes on the
      // SAME subscription.
      RequirePositionals(args, 2);
      const std::vector<std::string> nodes = SplitList(args.positionals[1]);
      std::vector<std::uint64_t> counts;
      std::vector<double> last_seen;
      std::vector<char> server_deleted;
      client.SubscribeValues(
          nodes, ParseDuration(args),
          [&](std::uint32_t subscription_id,
              const std::vector<std::uint32_t>& item_ids,
              const std::vector<std::string>& item_errors) {
            if (args.json) {
              return;
            }
            std::cout << "Subscription: " << subscription_id << "\n";
            for (std::size_t i = 0; i < nodes.size(); ++i) {
              std::cout << "  " << nodes[i] << "  item="
                        << (item_errors[i].empty()
                                ? std::to_string(item_ids[i])
                                : ("REFUSED " + item_errors[i]))
                        << "\n";
            }
            std::cout << "Collecting..." << std::endl;
          },
          counts, last_seen, server_deleted);

      if (args.json) {
        boost::json::array rows;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
          rows.push_back(boost::json::object{
              {"node", nodes[i]},
              {"count", counts[i]},
              {"last_seen_s", last_seen[i]},
              {"server_deleted", server_deleted[i] != 0}});
        }
        std::cout << boost::json::serialize(rows) << std::endl;
      } else {
        for (std::size_t i = 0; i < nodes.size(); ++i) {
          std::cout << nodes[i] << "  count=" << counts[i];
          if (last_seen[i] < 0) {
            std::cout << "  last=never";
          } else {
            std::cout << "  last=" << last_seen[i] << "s";
          }
          if (server_deleted[i]) {
            std::cout << "  SERVER-DELETED";
          }
          std::cout << "\n";
        }
        std::cout << std::flush;
      }
    } else if (args.command == "events") {
      // The Server object is an event notifier on every server, so it is the
      // one node this command can default to and still be useful.
      const std::string node =
          args.positionals.size() > 1 ? args.positionals[1] : "i=2253";
      const std::vector<std::string> fields =
          args.options.count("select") ? SplitList(args.options["select"])
                                       : DefaultEventFields();

      client.SubscribeEvents(
          node, fields, ParseDuration(args), ParseCount(args),
          [&](const EventSubscriptionInfo& info) {
            // Everything about the subscription itself goes to stderr: stdout
            // is the stream of events and nothing else, so a pipe stays
            // parseable.
            if (!info.warning.empty()) {
              std::cerr << "warning: " << info.warning << "\n";
            }
            for (const auto& rejected : info.rejected_fields) {
              std::cerr << "warning: server rejected select clause " << rejected
                        << " — that field arrives null on every event\n";
            }
            std::cerr << "subscribed to events on " << node << " (subscription "
                      << info.subscription_id << ", item "
                      << info.monitored_item_id << ", publishing every "
                      << info.publishing_interval_ms << " ms)\n";
          },
          [&](const EventNotification& event) {
            if (args.json) {
              std::cout << boost::json::serialize(EventToJson(event))
                        << std::endl;
            } else {
              PrintEvent(event);
            }
          });
    } else {
      throw std::runtime_error("Unknown command: " + args.command);
    }
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << "\n";
    return 1;
  }
  return 0;
}
