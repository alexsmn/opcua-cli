#ifndef SRC_OPCUA_CLIENT_H_
#define SRC_OPCUA_CLIENT_H_

#include <boost/json/value.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nodeset.h"

struct SecurityOptions {
  std::string policy = "None";
  std::string mode = "None";
  std::string cert;
  std::string key;
  std::string ca;
  std::string username;
  std::string password;
  double timeout_seconds = 5.0;
  bool debug = false;
  bool debug_stderr = false;
  std::string debug_file;
};

struct BrowseEntry {
  std::string name;
  std::string node_id;
  std::string node_class;
  std::vector<BrowseEntry> children;
  // Set when browsing *this* node's children failed during a --recursive
  // crawl. Without it a node the server refused and a node with no children
  // both render as a bare leaf, which is the same defect as an empty top-level
  // browse one level down.
  std::string child_status;
};

struct BrowseResult {
  std::vector<BrowseEntry> entries;
  // Status of the Browse service call for the requested node. Carried out
  // rather than discarded: an empty `entries` on its own cannot distinguish
  // "the server refused" from "this node has no children", and conflating
  // those is exactly the kind of misreport this tool exists to avoid.
  std::string status;
  // True when the server answered the top-level browse with a Bad status; the
  // CLI exits nonzero so scripts can tell a failed browse from an empty one.
  bool bad = false;
  std::string hint;
};

struct ReadResult {
  std::string node_id;
  std::string attribute;
  // Single-line rendering of the value: a scalar as-is, an array as
  // "[a, b, c]". `watch` prints this per sample, so it must stay one line.
  std::string value;
  // Set when the value is an array: one rendered element per entry, in order.
  // The index carries meaning for the arrays this tool exists to inspect —
  // element N of the NamespaceArray *is* namespace index N — so `read`
  // prints them indexed rather than as a blob.
  std::optional<std::vector<std::string>> elements;
  // Typed rendering for --json: scalars stay scalars, arrays become real JSON
  // arrays. Null when the read returned no value.
  boost::json::value json_value;
  std::string type;
  std::string status;
  std::string source_timestamp;
  std::string server_timestamp;
  // Plain-language explanation set when the status code is likely to be read
  // as a tool malfunction rather than as the server's correct answer (e.g.
  // BadAttributeIdInvalid for Value on an Object node).
  std::string hint;
};

struct WriteResult {
  std::string node_id;
  std::string value;
  std::string type;
  std::string status;
  // True when the server answered with a Bad status; the CLI exits nonzero so
  // scripts can detect a rejected write without parsing the status text.
  bool bad = false;
};

// One select clause of an event notification, answered by the server.
struct EventField {
  // The field as requested on the command line: "EventId", "2:Vendor/Code".
  std::string name;
  // Human rendering, never empty. A field the server sent with no value at all
  // renders "<null>" and a zero-length ByteString renders "<empty>", because
  // an EventId that arrives absent is the defect this command exists to
  // surface — OPC UA Part 5 §6.4.2 makes EventId mandatory on every event, so
  // an invisible rendering would hide a real server bug.
  std::string value;
  // Typed rendering for --json; JSON null when the field carried no value.
  boost::json::value json_value;
  // OPC UA data type name of the field, "Null" when it carried no value.
  std::string type;
};

struct EventNotification {
  // 1-based arrival order within this run.
  std::uint64_t sequence = 0;
  // One entry per select clause, in the order requested. A server that returns
  // fewer fields than clauses has the missing ones filled in as null.
  std::vector<EventField> fields;
};

// What the server made of the subscription request, reported once the
// monitored item exists and before any event arrives.
struct EventSubscriptionInfo {
  std::uint32_t subscription_id = 0;
  std::uint32_t monitored_item_id = 0;
  double publishing_interval_ms = 0.0;
  // Select clauses the server accepted the item with but answered Bad for, as
  // "FieldName: status". Those fields arrive null on every event, which must
  // not be mistaken for the server reporting a null value.
  std::vector<std::string> rejected_fields;
  // Set when the node's own EventNotifier attribute says it raises no events.
  // A warning rather than an error: the authoritative answer is the server's
  // CreateMonitoredItems status, and a server that misreports its own
  // attribute may still deliver.
  std::string warning;
};

struct EndpointInfo {
  std::string endpoint_url;
  std::string security_policy;
  std::string security_mode;
  std::vector<std::string> user_token_policies;
  // discoveryUrls of the ApplicationDescription embedded in the endpoint
  // (OPC UA Part 4 §7.2). Separate from endpoint_url, and worth seeing: a
  // client that reconnects via FindServers dials one of these, so a server
  // advertising an address only reachable inside its own network strands the
  // reconnect while GetEndpoints still looks perfectly healthy.
  std::vector<std::string> discovery_urls;
};

class OpcuaClient {
 public:
  explicit OpcuaClient(SecurityOptions options);
  ~OpcuaClient();

  OpcuaClient(const OpcuaClient&) = delete;
  OpcuaClient& operator=(const OpcuaClient&) = delete;

  void Connect(const std::string& endpoint);
  void Disconnect();

  BrowseResult Browse(const std::string& target, bool recursive, int depth);
  ReadResult Read(const std::string& node_id, const std::string& attribute);
  WriteResult Write(const std::string& node_id,
                    const std::string& value,
                    const std::string& type);
  std::vector<EndpointInfo> Endpoints(const std::string& endpoint);
  std::vector<ReadResult> Poll(const std::string& node_id,
                               std::uint64_t interval_ms,
                               bool once);

  // Creates a subscription and a monitored item on node_id's EventNotifier
  // attribute, filtered by an EventFilter whose select clauses are `fields`
  // (browse paths below BaseEventType). Calls on_ready once the server has
  // accepted the item, then on_event for each event that arrives, and returns
  // when `count` events have been received or `duration_seconds` of wall-clock
  // have elapsed — with neither it runs until interrupted. The subscription is
  // deleted before returning.
  void SubscribeEvents(
      const std::string& node_id,
      const std::vector<std::string>& fields,
      std::optional<double> duration_seconds,
      std::optional<std::uint64_t> count,
      const std::function<void(const EventSubscriptionInfo&)>& on_ready,
      const std::function<void(const EventNotification&)>& on_event);

  // Crawls the address space breadth-first from root_node, following forward
  // references, and returns the captured nodes plus the server namespace
  // array. When namespace_filter is set, only nodes in that namespace index
  // are emitted (the crawl still traverses through other namespaces to reach
  // them). Stops after max_nodes have been emitted. When include_values is
  // set, each Variable's current value is read and kept as a comment — this
  // is off by default because reading device-backed values can block on
  // device I/O and slow the crawl by orders of magnitude.
  NodesetDump DumpNodeset(const std::string& root_node,
                          std::optional<int> namespace_filter,
                          std::size_t max_nodes,
                          bool include_values);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

std::string StatusToString(std::uint32_t status_code);

// The BaseEventType fields every conforming server defines, in the order OPC
// UA Part 5 §6.4.2 lists them:
// https://reference.opcfoundation.org/Core/Part5/v105/docs/6.4.2
std::vector<std::string> DefaultEventFields();

#endif  // SRC_OPCUA_CLIENT_H_
