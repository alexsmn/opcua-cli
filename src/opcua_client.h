#ifndef SRC_OPCUA_CLIENT_H_
#define SRC_OPCUA_CLIENT_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

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
};

struct ReadResult {
  std::string node_id;
  std::string attribute;
  std::string value;
  std::string type;
  std::string status;
  std::string source_timestamp;
  std::string server_timestamp;
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

struct EndpointInfo {
  std::string endpoint_url;
  std::string security_policy;
  std::string security_mode;
  std::vector<std::string> user_token_policies;
};

class OpcuaClient {
 public:
  explicit OpcuaClient(SecurityOptions options);
  ~OpcuaClient();

  OpcuaClient(const OpcuaClient&) = delete;
  OpcuaClient& operator=(const OpcuaClient&) = delete;

  void Connect(const std::string& endpoint);
  void Disconnect();

  std::vector<BrowseEntry> Browse(const std::string& target,
                                  bool recursive,
                                  int depth);
  ReadResult Read(const std::string& node_id, const std::string& attribute);
  WriteResult Write(const std::string& node_id,
                    const std::string& value,
                    const std::string& type);
  std::vector<EndpointInfo> Endpoints(const std::string& endpoint);
  std::vector<ReadResult> Poll(const std::string& node_id,
                               std::uint64_t interval_ms,
                               bool once);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

std::string StatusToString(std::uint32_t status_code);

#endif  // SRC_OPCUA_CLIENT_H_
