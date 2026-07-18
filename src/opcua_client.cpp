#include "opcua_client.h"

#include <open62541pp/open62541pp.hpp>

#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {

std::string ToString(std::string_view value) {
  return std::string(value);
}

std::string ToString(const opcua::String& value) {
  return ToString(static_cast<std::string_view>(value));
}

std::string StatusToString(opcua::StatusCode status_code) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "0x%08X", status_code.get());
  std::string name = ToString(status_code.name());
  if (name == "Unknown StatusCode") {
    // Codes outside open62541's table (e.g. vendor-specific server codes):
    // classify by the severity bits, OPC UA Part 4 §7.38,
    // https://reference.opcfoundation.org/Core/Part4/v105/docs/7.38
    name = status_code.isBad()         ? "Bad (vendor-specific)"
           : status_code.isUncertain() ? "Uncertain (vendor-specific)"
                                       : "Good (vendor-specific)";
  }
  return name + " (" + buffer + ")";
}

std::string NodeClassName(opcua::NodeClass node_class) {
  switch (node_class) {
    case opcua::NodeClass::Object:
      return "Object";
    case opcua::NodeClass::Variable:
      return "Variable";
    case opcua::NodeClass::Method:
      return "Method";
    case opcua::NodeClass::ObjectType:
      return "ObjectType";
    case opcua::NodeClass::VariableType:
      return "VariableType";
    case opcua::NodeClass::ReferenceType:
      return "ReferenceType";
    case opcua::NodeClass::DataType:
      return "DataType";
    case opcua::NodeClass::View:
      return "View";
    default:
      return "Unspecified";
  }
}

std::string SecurityModeName(opcua::MessageSecurityMode mode) {
  switch (mode) {
    case opcua::MessageSecurityMode::None:
      return "None";
    case opcua::MessageSecurityMode::Sign:
      return "Sign";
    case opcua::MessageSecurityMode::SignAndEncrypt:
      return "SignAndEncrypt";
    default:
      return "Invalid";
  }
}

std::string UserTokenName(opcua::UserTokenType type) {
  switch (type) {
    case opcua::UserTokenType::Anonymous:
      return "Anonymous";
    case opcua::UserTokenType::Username:
      return "UserName";
    case opcua::UserTokenType::Certificate:
      return "Certificate";
    case opcua::UserTokenType::IssuedToken:
      return "IssuedToken";
    default:
      return "Unknown";
  }
}

opcua::MessageSecurityMode ParseSecurityMode(const std::string& mode) {
  if (mode == "None") {
    return opcua::MessageSecurityMode::None;
  }
  if (mode == "Sign") {
    return opcua::MessageSecurityMode::Sign;
  }
  if (mode == "SignAndEncrypt") {
    return opcua::MessageSecurityMode::SignAndEncrypt;
  }
  throw std::runtime_error("Unsupported security mode: " + mode);
}

opcua::NodeId ParseNodeId(const std::string& text) {
  unsigned int ns = 0;
  std::string rest = text;
  if (rest.rfind("ns=", 0) == 0) {
    const auto semi = rest.find(';');
    if (semi == std::string::npos) {
      throw std::runtime_error("Invalid NodeId: missing ';' after namespace");
    }
    ns = static_cast<unsigned int>(std::stoul(rest.substr(3, semi - 3)));
    rest = rest.substr(semi + 1);
  }
  if (rest.rfind("i=", 0) == 0) {
    return opcua::NodeId(static_cast<opcua::NamespaceIndex>(ns),
                         static_cast<uint32_t>(std::stoul(rest.substr(2))));
  }
  if (rest.rfind("s=", 0) == 0) {
    return opcua::NodeId(static_cast<opcua::NamespaceIndex>(ns),
                         rest.substr(2));
  }
  throw std::runtime_error("Unsupported NodeId format: " + text +
                           " (supported: i=, ns=N;i=, ns=N;s=)");
}

std::string NodeIdToString(const opcua::NodeId& id) {
  return ToString(opcua::toString(id));
}

opcua::AttributeId AttributeId(const std::string& name) {
  if (name == "Value")
    return opcua::AttributeId::Value;
  if (name == "DisplayName")
    return opcua::AttributeId::DisplayName;
  if (name == "BrowseName")
    return opcua::AttributeId::BrowseName;
  if (name == "DataType")
    return opcua::AttributeId::DataType;
  if (name == "NodeClass")
    return opcua::AttributeId::NodeClass;
  if (name == "Description")
    return opcua::AttributeId::Description;
  if (name == "AccessLevel")
    return opcua::AttributeId::AccessLevel;
  if (name == "NodeId")
    return opcua::AttributeId::NodeId;
  throw std::runtime_error("Unsupported attribute: " + name);
}

std::string VariantTypeName(const opcua::Variant& variant) {
  if (variant.type() == nullptr) {
    return "Null";
  }
  return variant.type()->typeName;
}

template <typename T>
std::string ScalarToString(const opcua::Variant& variant) {
  return std::to_string(variant.scalar<T>());
}

std::string VariantToString(const opcua::Variant& variant) {
  if (!variant.isScalar() || variant.data() == nullptr) {
    return variant.arrayLength() > 0 ? "<array>" : "null";
  }
  if (variant.isType<bool>()) {
    return variant.scalar<bool>() ? "true" : "false";
  }
  if (variant.isType<UA_SByte>())
    return ScalarToString<UA_SByte>(variant);
  if (variant.isType<UA_Byte>())
    return ScalarToString<UA_Byte>(variant);
  if (variant.isType<UA_Int16>())
    return ScalarToString<UA_Int16>(variant);
  if (variant.isType<UA_UInt16>())
    return ScalarToString<UA_UInt16>(variant);
  if (variant.isType<UA_Int32>())
    return ScalarToString<UA_Int32>(variant);
  if (variant.isType<UA_UInt32>())
    return ScalarToString<UA_UInt32>(variant);
  if (variant.isType<UA_Int64>())
    return ScalarToString<UA_Int64>(variant);
  if (variant.isType<UA_UInt64>())
    return ScalarToString<UA_UInt64>(variant);
  if (variant.isType<UA_Float>())
    return ScalarToString<UA_Float>(variant);
  if (variant.isType<UA_Double>())
    return ScalarToString<UA_Double>(variant);
  if (variant.isType<opcua::String>())
    return ToString(variant.scalar<opcua::String>());
  if (variant.isType<opcua::LocalizedText>())
    return ToString(variant.scalar<opcua::LocalizedText>().text());
  if (variant.isType<opcua::QualifiedName>())
    return ToString(variant.scalar<opcua::QualifiedName>().name());
  if (variant.isType<opcua::NodeId>())
    return NodeIdToString(variant.scalar<opcua::NodeId>());
  return "<" + VariantTypeName(variant) + ">";
}

std::string DateTimeToString(opcua::DateTime value) {
  if (value.get() == 0) {
    return "";
  }
  const UA_DateTimeStruct dts = value.toStruct();
  char buffer[40];
  std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02uZ",
                dts.year, dts.month, dts.day, dts.hour, dts.min, dts.sec);
  return buffer;
}

opcua::Variant MakeVariant(const std::string& value, const std::string& type) {
  if (type == "Boolean") {
    return opcua::Variant(value == "true" || value == "1");
  }
  if (type == "SByte") {
    return opcua::Variant(static_cast<UA_SByte>(std::stoi(value)));
  }
  if (type == "Byte") {
    return opcua::Variant(static_cast<UA_Byte>(std::stoul(value)));
  }
  if (type == "Int16") {
    return opcua::Variant(static_cast<UA_Int16>(std::stoi(value)));
  }
  if (type == "UInt16") {
    return opcua::Variant(static_cast<UA_UInt16>(std::stoul(value)));
  }
  if (type == "Int32" || type.empty()) {
    return opcua::Variant(static_cast<UA_Int32>(std::stoi(value)));
  }
  if (type == "UInt32") {
    return opcua::Variant(static_cast<UA_UInt32>(std::stoul(value)));
  }
  if (type == "Int64") {
    return opcua::Variant(static_cast<UA_Int64>(std::stoll(value)));
  }
  if (type == "UInt64") {
    return opcua::Variant(static_cast<UA_UInt64>(std::stoull(value)));
  }
  if (type == "Float") {
    return opcua::Variant(static_cast<UA_Float>(std::stof(value)));
  }
  if (type == "Double") {
    return opcua::Variant(static_cast<UA_Double>(std::stod(value)));
  }
  if (type == "String") {
    return opcua::Variant(value);
  }
  throw std::runtime_error("Unsupported write type: " + type);
}

const char* LogLevelName(opcua::LogLevel level) {
  switch (level) {
    case opcua::LogLevel::Trace:
      return "trace";
    case opcua::LogLevel::Debug:
      return "debug";
    case opcua::LogLevel::Info:
      return "info";
    case opcua::LogLevel::Warning:
      return "warn";
    case opcua::LogLevel::Error:
      return "error";
    case opcua::LogLevel::Fatal:
      return "fatal";
  }
  return "log";
}

// Builds the client log function honoring the --debug/--debug-stderr/
// --debug-file options. stdout is reserved for command output (including
// machine-readable --json), so logs never go there unless --debug explicitly
// asks for full logging on stdout; without --debug only warnings and worse
// are shown, on stderr. `file` outlives the returned function (owned by
// OpcuaClient::Impl).
opcua::LogFunction MakeLogFunction(const SecurityOptions& options,
                                   std::FILE* file) {
  std::FILE* destination = file != nullptr        ? file
                           : options.debug_stderr ? stderr
                           : options.debug        ? stdout
                                                  : stderr;
  const opcua::LogLevel min_level =
      options.debug ? opcua::LogLevel::Debug : opcua::LogLevel::Warning;
  return [destination, min_level](opcua::LogLevel level,
                                  opcua::LogCategory /*category*/,
                                  std::string_view message) {
    if (level < min_level) {
      return;
    }
    std::fprintf(destination, "[%s] %.*s\n", LogLevelName(level),
                 static_cast<int>(message.size()), message.data());
    std::fflush(destination);
  };
}

std::vector<BrowseEntry> BrowseNode(opcua::Client& client,
                                    const opcua::NodeId& node_id,
                                    bool recursive,
                                    int depth) {
  opcua::BrowseDescription description(
      node_id, opcua::BrowseDirection::Forward,
      opcua::ReferenceTypeId::HierarchicalReferences, true,
      opcua::NodeClass::Unspecified, opcua::BrowseResultMask::All);

  opcua::BrowseResult response =
      opcua::services::browse(client, description, 0);
  std::vector<BrowseEntry> entries;
  if (response.statusCode().isBad()) {
    return entries;
  }

  for (const auto& ref : response.references()) {
    if (!ref.nodeId().isLocal() ||
        ref.nodeId().nodeId().identifierType() == opcua::NodeIdType::Guid) {
      continue;
    }
    BrowseEntry entry;
    entry.name = ToString(ref.displayName().text());
    entry.node_id = NodeIdToString(ref.nodeId().nodeId());
    entry.node_class = NodeClassName(ref.nodeClass());
    if (recursive && depth > 1) {
      entry.children =
          BrowseNode(client, ref.nodeId().nodeId(), true, depth - 1);
    }
    entries.push_back(std::move(entry));
  }
  return entries;
}

}  // namespace

struct OpcuaClient::Impl {
  explicit Impl(SecurityOptions opts) : options(std::move(opts)) {
    if (!options.debug_file.empty()) {
      log_file.reset(std::fopen(options.debug_file.c_str(), "a"));
      if (log_file == nullptr) {
        throw std::runtime_error("Cannot open debug file: " +
                                 options.debug_file);
      }
    }
    opcua::ClientConfig config;
    config.setLogger(MakeLogFunction(options, log_file.get()));
    config.setTimeout(static_cast<uint32_t>(options.timeout_seconds * 1000.0));
    config.setSecurityMode(ParseSecurityMode(options.mode));
    if (!options.username.empty()) {
      config.setUserIdentityToken(
          opcua::UserNameIdentityToken(options.username, options.password));
    }
    client = std::make_unique<opcua::Client>(std::move(config));
  }

  struct FileCloser {
    void operator()(std::FILE* file) const { std::fclose(file); }
  };

  SecurityOptions options;
  // Declared before `client` so the log destination outlives the client's
  // logger on destruction.
  std::unique_ptr<std::FILE, FileCloser> log_file;
  std::unique_ptr<opcua::Client> client;
};

OpcuaClient::OpcuaClient(SecurityOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
OpcuaClient::~OpcuaClient() = default;

void OpcuaClient::Connect(const std::string& endpoint) {
  try {
    impl_->client->connect(endpoint);
  } catch (const opcua::BadStatus& error) {
    throw std::runtime_error("Connect failed: " + StatusToString(error.code()));
  }
}

void OpcuaClient::Disconnect() {
  impl_->client->disconnect();
}

std::vector<BrowseEntry> OpcuaClient::Browse(const std::string& target,
                                             bool recursive,
                                             int depth) {
  const opcua::NodeId id = target.empty() || target == "/Objects"
                               ? opcua::NodeId(opcua::ObjectId::ObjectsFolder)
                               : ParseNodeId(target);
  return BrowseNode(*impl_->client, id, recursive, depth);
}

ReadResult OpcuaClient::Read(const std::string& node_id_text,
                             const std::string& attribute) {
  const opcua::NodeId node_id = ParseNodeId(node_id_text);
  const opcua::AttributeId attribute_id = AttributeId(attribute);
  auto data_value = opcua::services::readAttribute(
      *impl_->client, node_id, attribute_id, opcua::TimestampsToReturn::Both);

  ReadResult result;
  result.node_id = node_id_text;
  result.attribute = attribute;
  result.status = StatusToString(data_value.code());
  if (data_value) {
    const opcua::DataValue& value = data_value.value();
    result.status = StatusToString(value.status());
    if (value.hasValue()) {
      result.value = VariantToString(value.value());
      result.type = VariantTypeName(value.value());
    }
    if (value.hasSourceTimestamp()) {
      result.source_timestamp = DateTimeToString(value.sourceTimestamp());
    }
    if (value.hasServerTimestamp()) {
      result.server_timestamp = DateTimeToString(value.serverTimestamp());
    }
  }
  return result;
}

WriteResult OpcuaClient::Write(const std::string& node_id_text,
                               const std::string& value,
                               const std::string& type) {
  const opcua::NodeId node_id = ParseNodeId(node_id_text);
  opcua::StatusCode status = opcua::services::writeValue(
      *impl_->client, node_id, MakeVariant(value, type));
  return {node_id_text, value, type.empty() ? "Int32" : type,
          StatusToString(status), status.isBad()};
}

std::vector<EndpointInfo> OpcuaClient::Endpoints(const std::string& endpoint) {
  std::vector<EndpointInfo> result;
  for (const auto& endpoint_description :
       impl_->client->getEndpoints(endpoint)) {
    EndpointInfo info;
    info.endpoint_url = ToString(endpoint_description.endpointUrl());
    info.security_policy = ToString(endpoint_description.securityPolicyUri());
    info.security_mode = SecurityModeName(endpoint_description.securityMode());
    for (const auto& token : endpoint_description.userIdentityTokens()) {
      info.user_token_policies.push_back(UserTokenName(token.tokenType()));
    }
    result.push_back(std::move(info));
  }
  return result;
}

std::vector<ReadResult> OpcuaClient::Poll(const std::string& node_id,
                                          std::uint64_t interval_ms,
                                          bool once) {
  std::vector<ReadResult> values;
  do {
    values.push_back(Read(node_id, "Value"));
    if (once) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
  } while (true);
  return values;
}

std::string StatusToString(std::uint32_t status_code) {
  return StatusToString(opcua::StatusCode(status_code));
}
