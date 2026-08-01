#include "opcua_client.h"

#include <open62541pp/open62541pp.hpp>

#include <boost/json.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <vector>

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

// Canonical OPC UA NodeId text without the surrounding quotes that
// opcua::toString adds — the form UANodeSet uses for NodeId attributes and
// references, and the form every command emits: i=2253, ns=7;i=310,
// ns=1;s=the.node. Quoted output could not be pasted back into another
// invocation, and in --json it forced consumers to strip the inner quotes.
std::string FormatNodeId(const opcua::NodeId& id) {
  std::string out;
  if (id.namespaceIndex() != 0) {
    out += "ns=" + std::to_string(id.namespaceIndex()) + ";";
  }
  switch (id.identifierType()) {
    case opcua::NodeIdType::Numeric:
      out += "i=" + std::to_string(*id.identifierIf<uint32_t>());
      break;
    case opcua::NodeIdType::String:
      out += "s=" + ToString(*id.identifierIf<opcua::String>());
      break;
    case opcua::NodeIdType::Guid:
    case opcua::NodeIdType::ByteString:
      // Rare in a SCADA address space; fall back to the library rendering
      // with its quotes stripped.
      out = ToString(opcua::toString(id));
      if (out.size() >= 2 && out.front() == '"' && out.back() == '"') {
        out = out.substr(1, out.size() - 2);
      }
      break;
  }
  return out;
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
  if (name == "EventNotifier")
    return opcua::AttributeId::EventNotifier;
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

// Calls `visit` with element `index` of the variant, statically typed as the
// variant's data type. Scalars are treated as a single element at index 0, so
// scalar and array values share one rendering path. Returns false when the
// data type is not one rendered natively (structures, ExtensionObjects, ...),
// leaving it to the caller to fall back to the type name.
template <typename Visitor>
bool VisitVariantElement(const opcua::Variant& variant,
                         std::size_t index,
                         Visitor&& visit) {
  const auto dispatch = [&]<typename T>() {
    if (!variant.isType<T>()) {
      return false;
    }
    visit(variant.isScalar() ? variant.scalar<T>() : variant.array<T>()[index]);
    return true;
  };
  return dispatch.template operator()<bool>() ||
         dispatch.template operator()<UA_SByte>() ||
         dispatch.template operator()<UA_Byte>() ||
         dispatch.template operator()<UA_Int16>() ||
         dispatch.template operator()<UA_UInt16>() ||
         dispatch.template operator()<UA_Int32>() ||
         dispatch.template operator()<UA_UInt32>() ||
         dispatch.template operator()<UA_Int64>() ||
         dispatch.template operator()<UA_UInt64>() ||
         dispatch.template operator()<UA_Float>() ||
         dispatch.template operator()<UA_Double>() ||
         dispatch.template operator()<opcua::String>() ||
         dispatch.template operator()<opcua::ByteString>() ||
         dispatch.template operator()<opcua::LocalizedText>() ||
         dispatch.template operator()<opcua::QualifiedName>() ||
         dispatch.template operator()<opcua::NodeId>() ||
         dispatch.template operator()<opcua::DateTime>() ||
         dispatch.template operator()<opcua::StatusCode>();
}

std::string DateTimeToString(opcua::DateTime value);

// A ByteString is opaque bytes, not text, so it is rendered as lowercase hex:
// the value can then be compared and pasted. Two renderings are deliberately
// *visible* rather than empty. A zero-length ByteString reads "<empty>",
// because the ByteString this tool most often shows is an EventId, which OPC
// UA Part 5 §6.4.2 makes mandatory on every event — an event carrying no
// EventId is a server defect, and rendering it as nothing at all would hide
// exactly the thing worth seeing. Long blobs (a certificate, say) are cut at
// 64 bytes with the true length appended rather than flooding a line.
std::string ByteStringToString(const opcua::ByteString& value) {
  if (value.empty()) {
    return "<empty>";
  }
  static constexpr char kHexDigits[] = "0123456789abcdef";
  constexpr std::size_t kMaxBytes = 64;
  const std::size_t shown = std::min(value.size(), kMaxBytes);
  std::string out;
  out.reserve(shown * 2 + 24);
  for (std::size_t i = 0; i < shown; ++i) {
    const auto byte = static_cast<unsigned char>(value.data()[i]);
    out += kHexDigits[byte >> 4U];
    out += kHexDigits[byte & 0x0FU];
  }
  if (shown < value.size()) {
    out += "…(" + std::to_string(value.size()) + " bytes)";
  }
  return out;
}

// Shortest round-trip rendering. std::to_string would print a Double with six
// fixed decimals, which silently turns a 1e-9 measurement into "0.000000" —
// unacceptable in a tool whose whole job is reporting what a server holds.
template <typename T>
std::string FloatToString(T value) {
  char buffer[64];
  const auto [end, error] =
      std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (error != std::errc()) {
    return std::to_string(value);
  }
  return std::string(buffer, end);
}

std::string ElementToString(const opcua::Variant& variant, std::size_t index) {
  std::string out;
  const bool rendered =
      VisitVariantElement(variant, index, [&out](const auto& element) {
        using T = std::decay_t<decltype(element)>;
        if constexpr (std::is_same_v<T, bool>) {
          out = element ? "true" : "false";
        } else if constexpr (std::is_same_v<T, opcua::String>) {
          out = ToString(element);
        } else if constexpr (std::is_same_v<T, opcua::ByteString>) {
          out = ByteStringToString(element);
        } else if constexpr (std::is_same_v<T, opcua::LocalizedText>) {
          out = ToString(element.text());
        } else if constexpr (std::is_same_v<T, opcua::QualifiedName>) {
          out = ToString(element.name());
        } else if constexpr (std::is_same_v<T, opcua::NodeId>) {
          // Canonical "ns=2;i=1001" text, not the library's quoted form: it
          // can be pasted straight back into another opcua-cli invocation.
          out = FormatNodeId(element);
        } else if constexpr (std::is_same_v<T, opcua::DateTime>) {
          out = DateTimeToString(element);
        } else if constexpr (std::is_same_v<T, opcua::StatusCode>) {
          out = StatusToString(element);
        } else if constexpr (std::is_floating_point_v<T>) {
          out = FloatToString(element);
        } else {
          out = std::to_string(element);
        }
      });
  return rendered ? out : "<" + VariantTypeName(variant) + ">";
}

// Typed JSON for one element. Numbers stay numbers so `jq` arithmetic works;
// everything else becomes the same text `read` shows.
boost::json::value ElementToJson(const opcua::Variant& variant,
                                 std::size_t index) {
  boost::json::value out;
  const bool rendered =
      VisitVariantElement(variant, index, [&](const auto& element) {
        using T = std::decay_t<decltype(element)>;
        if constexpr (std::is_same_v<T, bool>) {
          out = element;
        } else if constexpr (std::is_floating_point_v<T>) {
          // JSON has no NaN/Infinity literal and a device can absolutely
          // report one, so fall back to the text form rather than emit
          // something no JSON parser will accept.
          if (std::isfinite(element)) {
            out = static_cast<double>(element);
          } else {
            out = boost::json::string(std::to_string(element));
          }
        } else if constexpr (std::is_signed_v<T>) {
          out = static_cast<std::int64_t>(element);
        } else if constexpr (std::is_integral_v<T>) {
          out = static_cast<std::uint64_t>(element);
        } else {
          out = boost::json::string(ElementToString(variant, index));
        }
      });
  return rendered ? out : boost::json::value();
}

// A variant with no data type at all carries no value. Anything else is either
// a scalar or an array — and an array of length zero is still an array, not a
// null: "this server serves no namespaces" and "this attribute is unset" are
// different answers and must not render alike.
bool IsNullVariant(const opcua::Variant& variant) {
  return variant.type() == nullptr;
}

// Single-line rendering used by `watch` and by the human `Value:` line.
std::string VariantToString(const opcua::Variant& variant) {
  if (IsNullVariant(variant)) {
    return "null";
  }
  if (variant.isScalar()) {
    return ElementToString(variant, 0);
  }
  std::string out = "[";
  const std::size_t count = variant.arrayLength();
  for (std::size_t i = 0; i < count; ++i) {
    if (i != 0) {
      out += ", ";
    }
    out += ElementToString(variant, i);
  }
  return out + "]";
}

// Per-element rendering for the human `read` output. Only arrays get one; a
// scalar is fully described by VariantToString.
std::optional<std::vector<std::string>> VariantToElements(
    const opcua::Variant& variant) {
  if (IsNullVariant(variant) || variant.isScalar()) {
    return std::nullopt;
  }
  std::vector<std::string> elements;
  elements.reserve(variant.arrayLength());
  for (std::size_t i = 0; i < variant.arrayLength(); ++i) {
    elements.push_back(ElementToString(variant, i));
  }
  return elements;
}

// Typed rendering for --json: a scalar stays a JSON scalar, an array becomes a
// real JSON array (never the string "<array>").
boost::json::value VariantToJson(const opcua::Variant& variant) {
  if (IsNullVariant(variant)) {
    return boost::json::value();
  }
  if (variant.isScalar()) {
    return ElementToJson(variant, 0);
  }
  boost::json::array out;
  out.reserve(variant.arrayLength());
  for (std::size_t i = 0; i < variant.arrayLength(); ++i) {
    out.push_back(ElementToJson(variant, i));
  }
  return out;
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

// BadAttributeIdInvalid is the server correctly saying "this node has no such
// attribute" — most often `read --attribute=Value` aimed at an Object, which
// is a folder-like node with no value at all. Reported bare it reads like the
// CLI broke, so name the node class and point at what to do instead. Only
// reached on the error path, so the extra read costs nothing in the good case.
std::string ExplainAttributeIdInvalid(opcua::Client& client,
                                      const opcua::NodeId& node_id,
                                      const std::string& attribute) {
  std::string node_class = "This";
  auto read_class = opcua::services::readAttribute(
      client, node_id, opcua::AttributeId::NodeClass,
      opcua::TimestampsToReturn::Neither);
  if (read_class && read_class->hasValue()) {
    node_class = NodeClassName(static_cast<opcua::NodeClass>(
                     read_class->value().scalar<int32_t>())) +
                 " node";
  }
  std::string hint = node_class + " has no " + attribute +
                     " attribute — the server rejected the request, the "
                     "connection is fine.";
  if (attribute == "Value") {
    hint +=
        " Only Variable and VariableType nodes carry a Value; try "
        "--attribute=DisplayName, or browse this node for child variables.";
  }
  return hint;
}

// Rejects transports this client cannot speak, before open62541 answers with a
// bare BadTcpEndpointUrlInvalid that reads like a typo in the URL. See the
// "Transport support" section of README.md for the full reasoning; the short
// version is that open62541 1.4 ships no WebSocket transport at all, and a
// server's WebSocket endpoint typically carries the UA-JSON mapping rather
// than the binary protocol this client encodes.
void ValidateEndpointScheme(const std::string& endpoint) {
  static constexpr std::string_view kWebSocketSchemes[] = {
      "opc.wss://", "opc.ws://", "wss://", "ws://", "https://", "http://"};
  for (const auto scheme : kWebSocketSchemes) {
    if (endpoint.rfind(scheme, 0) != 0) {
      continue;
    }
    throw std::runtime_error(
        std::string("unsupported endpoint scheme '") + std::string(scheme) +
        "': opcua-cli speaks OPC UA binary over TCP (opc.tcp://) only.\n"
        "  WebSocket endpoints are usually the UA-JSON mapping (subprotocol "
        "opcua+uajson), a different\n"
        "  wire encoding, and open62541 provides no WebSocket transport to "
        "build on either.\n"
        "  Use the server's opc.tcp:// endpoint, tunnelling if it is not "
        "routable: ssh -L 4840:localhost:4840 HOST\n"
        "  then: opcua-cli read opc.tcp://localhost:4840 i=2255");
  }
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
    entry.node_id = FormatNodeId(ref.nodeId().nodeId());
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
    // One publish request in flight instead of open62541's default ten. On
    // teardown the session is closed with deleteSubscriptions set, so every
    // queued publish request comes back BadNoSubscription and the client logs
    // a warning for each — ten lines of noise on the same stderr channel
    // `events` uses to report real problems. Nothing is lost by queueing less:
    // the server holds notifications in the monitored item queue until a
    // publish request is available and can answer a whole burst in one
    // response, so the only cost is a round trip of latency per publish cycle.
    config->outStandingPublishRequests = 1;
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
  ValidateEndpointScheme(endpoint);
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
  opcua::StatusCode status = data_value.code();
  if (data_value) {
    const opcua::DataValue& value = data_value.value();
    status = value.status();
    result.status = StatusToString(status);
    if (value.hasValue()) {
      result.value = VariantToString(value.value());
      result.elements = VariantToElements(value.value());
      result.json_value = VariantToJson(value.value());
      result.type = VariantTypeName(value.value());
    }
    if (value.hasSourceTimestamp()) {
      result.source_timestamp = DateTimeToString(value.sourceTimestamp());
    }
    if (value.hasServerTimestamp()) {
      result.server_timestamp = DateTimeToString(value.serverTimestamp());
    }
  }
  if (status == UA_STATUSCODE_BADATTRIBUTEIDINVALID) {
    result.hint = ExplainAttributeIdInvalid(*impl_->client, node_id, attribute);
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
  ValidateEndpointScheme(endpoint);
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
    for (const auto& url : endpoint_description.server().discoveryUrls()) {
      info.discovery_urls.push_back(ToString(url));
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

namespace {

// Parses one --select entry into a browse path below BaseEventType.
// "Message" -> {0:Message}; "2:Vendor/Code" -> {2:Vendor, 0:Code}. The
// namespace prefix defaults to 0 because the fields worth selecting by default
// are the BaseEventType ones, which all live in the OPC UA namespace.
std::vector<opcua::QualifiedName> ParseEventFieldPath(
    const std::string& field) {
  std::vector<opcua::QualifiedName> path;
  std::size_t start = 0;
  while (true) {
    const auto slash = field.find('/', start);
    std::string part = field.substr(
        start, slash == std::string::npos ? std::string::npos : slash - start);
    if (part.empty()) {
      throw std::runtime_error("Invalid event field '" + field +
                               "' (expected NAME, NS:NAME or A/B)");
    }
    opcua::NamespaceIndex ns = 0;
    const auto colon = part.find(':');
    if (colon != std::string::npos) {
      const std::string prefix = part.substr(0, colon);
      if (prefix.empty() ||
          prefix.find_first_not_of("0123456789") != std::string::npos) {
        throw std::runtime_error("Invalid namespace prefix in event field '" +
                                 field +
                                 "' (expected NS:NAME with NS numeric)");
      }
      ns = static_cast<opcua::NamespaceIndex>(std::stoul(prefix));
      part = part.substr(colon + 1);
    }
    path.emplace_back(ns, part);
    if (slash == std::string::npos) {
      return path;
    }
    start = slash + 1;
  }
}

// Renders one event field. Kept apart from VariantToString so that an absent
// value reads "<null>" rather than the lowercase "null" a read prints: on an
// event line every field is a name=value pair, and the whole point of this
// command is that a missing EventId stands out.
EventField MakeEventField(const std::string& name,
                          const opcua::Variant& value) {
  EventField field;
  field.name = name;
  field.type = VariantTypeName(value);
  if (IsNullVariant(value)) {
    field.value = "<null>";
    field.json_value = boost::json::value();
    return field;
  }
  field.value = VariantToString(value);
  field.json_value = VariantToJson(value);
  return field;
}

// Per-select-clause results from the server's EventFilterResult (OPC UA Part 4
// §7.22.4.2). A clause the server answers Bad for still occupies its slot in
// every notification, filled with a null — indistinguishable from the server
// reporting a genuinely null field unless the rejection is reported here.
std::vector<std::string> CollectRejectedSelectClauses(
    const opcua::ExtensionObject& filter_result,
    const std::vector<std::string>& fields) {
  std::vector<std::string> rejected;
  // Compared against the raw UA type rather than decodedData<T>() because
  // UA_EventFilterResult has no open62541pp wrapper to look up.
  if (filter_result.decodedType() != &UA_TYPES[UA_TYPES_EVENTFILTERRESULT]) {
    return rejected;
  }
  const auto* result =
      static_cast<const UA_EventFilterResult*>(filter_result.decodedData());
  for (std::size_t i = 0; i < result->selectClauseResultsSize; ++i) {
    const opcua::StatusCode status(result->selectClauseResults[i]);
    if (!status.isBad()) {
      continue;
    }
    const std::string name =
        i < fields.size() ? fields[i] : ("clause " + std::to_string(i));
    rejected.push_back(name + ": " + StatusToString(status));
  }
  return rejected;
}

// Best-effort pre-flight. A node whose EventNotifier attribute is missing, or
// has the SubscribeToEvents bit clear (OPC UA Part 3 §5.6.2), will never send
// an event; without this the user watches an idle terminal and cannot tell
// that apart from a quiet server. Returns a warning, not an error, because the
// authoritative answer is the CreateMonitoredItems status below and a server
// that misreports its own attribute may still deliver.
std::string CheckEventNotifier(opcua::Client& client,
                               const opcua::NodeId& node,
                               const std::string& node_id_text) {
  auto value = opcua::services::readAttribute(
      client, node, opcua::AttributeId::EventNotifier,
      opcua::TimestampsToReturn::Neither);
  if (!value || !value->hasValue() || !value->value().isType<UA_Byte>()) {
    // Deliberately weaker than the SubscribeToEvents case below: servers that
    // do not expose the attribute at all still accept an event subscription
    // and deliver — an aggregating proxy answering BadAttributeIdInvalid for
    // EventNotifier on i=2253 was observed doing exactly that. Saying more
    // than "watch out" here would be wrong.
    return node_id_text +
           " does not expose a readable EventNotifier attribute. Some servers "
           "omit it and still deliver events, so this is not conclusive — but "
           "if nothing arrives, that is the first thing to suspect.";
  }
  const auto bits = value->value().scalar<UA_Byte>();
  if ((bits & static_cast<UA_Byte>(opcua::EventNotifier::SubscribeToEvents)) ==
      0) {
    return node_id_text + " has EventNotifier=" + std::to_string(bits) +
           " with the SubscribeToEvents bit clear (OPC UA Part 3 §5.6.2): the "
           "server does not offer event subscriptions on this node.";
  }
  return {};
}

// A rejected event monitored item reads like a broken tool, but it is usually
// the server correctly refusing: either the node raises no events, or it will
// not serve the requested EventFilter. Say which, in the style of
// ExplainAttributeIdInvalid.
std::string ExplainEventSubscribeFailure(
    const std::string& node_id_text,
    opcua::StatusCode status,
    const std::vector<std::string>& rejected) {
  std::string message = "Event subscription on " + node_id_text +
                        " rejected: " + StatusToString(status);
  switch (status.get()) {
    case UA_STATUSCODE_BADATTRIBUTEIDINVALID:
    case UA_STATUSCODE_BADNODEATTRIBUTESINVALID:
    case UA_STATUSCODE_BADNOTSUPPORTED:
      message +=
          "\n  Hint: this node has no EventNotifier attribute, so it raises no "
          "events — the server rejected the request, the connection is fine.\n"
          "  Every server exposes the Server object i=2253 as an event "
          "notifier; try that, or browse for a node whose EventNotifier has "
          "the SubscribeToEvents bit set.";
      break;
    case UA_STATUSCODE_BADEVENTFILTERINVALID:
    case UA_STATUSCODE_BADMONITOREDITEMFILTERINVALID:
    case UA_STATUSCODE_BADMONITOREDITEMFILTERUNSUPPORTED:
    case UA_STATUSCODE_BADFILTERNOTALLOWED:
      message +=
          "\n  Hint: the server rejected the EventFilter, not the connection. "
          "Restrict --select to fields this\n"
          "  server's event types define; the BaseEventType set (OPC UA Part 5 "
          "§6.4.2) is the safe default.";
      break;
    default:
      break;
  }
  for (const auto& field : rejected) {
    message += "\n  Rejected select clause " + field;
  }
  return message;
}

std::optional<std::string> ExtractValuePreview(const opcua::DataValue& dv) {
  if (!dv.hasValue()) {
    return std::nullopt;
  }
  std::string text = VariantToString(dv.value());
  constexpr std::size_t kMaxPreview = 120;
  if (text.size() > kMaxPreview) {
    text = text.substr(0, kMaxPreview) + "…";
  }
  return text;
}

// The structural attributes read for every node, in this fixed order. Value
// is appended only when the caller opts in (it can block on device I/O), so
// it always occupies the last slot when present.
constexpr opcua::AttributeId kStructuralAttributes[] = {
    opcua::AttributeId::NodeClass,   opcua::AttributeId::BrowseName,
    opcua::AttributeId::DisplayName, opcua::AttributeId::Description,
    opcua::AttributeId::DataType,    opcua::AttributeId::ValueRank,
    opcua::AttributeId::IsAbstract,  opcua::AttributeId::Symmetric,
    opcua::AttributeId::InverseName,
};
constexpr std::size_t kValueSlot = std::size(kStructuralAttributes);

// Reads, in a single Read request, the structural attributes that any node
// class might carry (plus Value when include_value is set). Attributes that do
// not apply to the node return a Bad status and are left unset by the caller.
std::vector<opcua::DataValue> ReadNodeAttributes(opcua::Client& client,
                                                 const opcua::NodeId& id,
                                                 bool include_value) {
  std::vector<opcua::ReadValueId> to_read;
  to_read.reserve(std::size(kStructuralAttributes) + 1);
  for (const auto attribute : kStructuralAttributes) {
    to_read.emplace_back(id, attribute);
  }
  if (include_value) {
    to_read.emplace_back(id, opcua::AttributeId::Value);
  }
  opcua::ReadResponse response = opcua::services::read(
      client, to_read, opcua::TimestampsToReturn::Neither);
  std::vector<opcua::DataValue> results;
  for (auto& value : response.results()) {
    results.emplace_back(value);
  }
  return results;
}

}  // namespace

std::vector<std::string> DefaultEventFields() {
  return {"EventId", "EventType",   "SourceNode", "SourceName",
          "Time",    "ReceiveTime", "Message",    "Severity"};
}

void OpcuaClient::SubscribeEvents(
    const std::string& node_id_text,
    const std::vector<std::string>& fields,
    std::optional<double> duration_seconds,
    std::optional<std::uint64_t> count,
    const std::function<void(const EventSubscriptionInfo&)>& on_ready,
    const std::function<void(const EventNotification&)>& on_event) {
  opcua::Client& client = *impl_->client;
  const opcua::NodeId node = ParseNodeId(node_id_text);
  if (fields.empty()) {
    throw std::runtime_error(
        "Event subscription needs at least one select "
        "clause (--select=EventId,Message,...)");
  }

  std::vector<opcua::SimpleAttributeOperand> select_clauses;
  select_clauses.reserve(fields.size());
  for (const auto& field : fields) {
    // Every clause is rooted at BaseEventType: a select clause names the type
    // that *defines* the field, and BaseEventType is the supertype of every
    // event type, so its fields resolve on any concrete event a server sends
    // (OPC UA Part 4 §7.7.4.5).
    select_clauses.emplace_back(
        opcua::NodeId(opcua::ObjectTypeId::BaseEventType),
        ParseEventFieldPath(field), opcua::AttributeId::Value);
  }
  // An empty where clause means "no filtering": report every event the node
  // raises. Narrowing by event type or severity is the server's job to support
  // and not what this command is for.
  const opcua::EventFilter event_filter(select_clauses, opcua::ContentFilter{});

  EventSubscriptionInfo info;
  info.warning = CheckEventNotifier(client, node, node_id_text);

  const auto subscription = opcua::services::createSubscription(
      client, opcua::SubscriptionParameters{}, true, {}, {});
  const opcua::StatusCode subscription_status =
      subscription.responseHeader().serviceResult();
  if (subscription_status.isBad()) {
    throw std::runtime_error("CreateSubscription failed: " +
                             StatusToString(subscription_status));
  }
  const opcua::IntegerId subscription_id = subscription.subscriptionId();

  // Delete the subscription on every exit path, including the throw below: a
  // server keeps it alive for lifetimeCount publishing cycles otherwise, and a
  // diagnostic tool run in a loop should not leave a trail of them.
  struct SubscriptionGuard {
    ~SubscriptionGuard() {
      static_cast<void>(opcua::services::deleteSubscription(client, id));
    }
    opcua::Client& client;
    opcua::IntegerId id;
  } guard{client, subscription_id};

  opcua::MonitoringParametersEx parameters;
  parameters.filter = opcua::ExtensionObject(event_filter);
  // Sampling does not apply to event items — the server queues events as they
  // are raised (OPC UA Part 4 §5.12.1.3) — so ask for the fastest practical
  // rate rather than leaving the data-change default of 250 ms.
  parameters.samplingInterval = 0.0;
  // The default queue size of 1 discards every event but the newest between
  // publishes, which would hide exactly the bursts worth diagnosing.
  parameters.queueSize = 1000;

  std::uint64_t received = 0;
  std::string callback_error;
  const auto result = opcua::services::createMonitoredItemEvent(
      client, subscription_id, {node, opcua::AttributeId::EventNotifier},
      opcua::MonitoringMode::Reporting, parameters,
      [&](opcua::IntegerId, opcua::IntegerId,
          opcua::Span<const opcua::Variant> event_fields) {
        // This runs inside open62541's C callback: an exception escaping here
        // would unwind through C frames, so failures are recorded and the run
        // loop below ends on them instead.
        try {
          EventNotification notification;
          notification.sequence = ++received;
          notification.fields.reserve(fields.size());
          for (std::size_t i = 0; i < fields.size(); ++i) {
            // A server may return fewer fields than clauses; the missing ones
            // are null, exactly as a field it declined to fill would be.
            static const opcua::Variant kAbsent;
            notification.fields.push_back(MakeEventField(
                fields[i],
                i < event_fields.size() ? event_fields[i] : kAbsent));
          }
          on_event(notification);
        } catch (const std::exception& error) {
          callback_error = error.what();
        }
      });

  info.rejected_fields =
      CollectRejectedSelectClauses(result.filterResult(), fields);
  const opcua::StatusCode item_status = result.statusCode();
  if (item_status.isBad()) {
    // The pre-flight finding is reported here too: on this path on_ready never
    // runs, and "the filter was refused" plus "this node raises no events
    // anyway" are different problems the user should not have to rediscover.
    std::string message = ExplainEventSubscribeFailure(
        node_id_text, item_status, info.rejected_fields);
    if (!info.warning.empty()) {
      message += "\n  Note: " + info.warning;
    }
    throw std::runtime_error(message);
  }

  info.subscription_id = subscription_id;
  info.monitored_item_id = result.monitoredItemId();
  info.publishing_interval_ms = subscription.revisedPublishingInterval();
  on_ready(info);

  // Same self-termination contract as `watch`: --duration is wall-clock
  // seconds, --count is an event count, whichever is reached first ends the
  // run, and without either it runs until interrupted. Neither is --timeout,
  // which is the connect/request timeout.
  using Clock = std::chrono::steady_clock;
  std::optional<Clock::time_point> deadline;
  if (duration_seconds) {
    deadline =
        Clock::now() + std::chrono::duration_cast<Clock::duration>(
                           std::chrono::duration<double>(*duration_seconds));
  }
  // Iterate in short slices rather than one long network wait, so --duration
  // and --count take effect promptly instead of only when a publish arrives.
  constexpr std::int64_t kSliceMs = 100;
  while (callback_error.empty() && (!count || received < *count)) {
    std::int64_t slice = kSliceMs;
    if (deadline) {
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(*deadline -
                                                                Clock::now())
              .count();
      if (remaining <= 0) {
        break;
      }
      slice = std::min(remaining, kSliceMs);
    }
    client.runIterate(static_cast<std::uint16_t>(slice));
  }

  if (!callback_error.empty()) {
    throw std::runtime_error(callback_error);
  }
}

NodesetDump OpcuaClient::DumpNodeset(const std::string& root_node,
                                     std::optional<int> namespace_filter,
                                     std::size_t max_nodes,
                                     bool include_values) {
  opcua::Client& client = *impl_->client;
  NodesetDump dump;

  // Server namespace array (i=2255): index 0 is the OPC UA namespace, so the
  // emitted list starts at index 1.
  auto ns_value = opcua::services::readAttribute(
      client, opcua::NodeId(0, 2255), opcua::AttributeId::Value,
      opcua::TimestampsToReturn::Neither);
  if (ns_value && ns_value->hasValue() && ns_value->value().isArray()) {
    const auto uris = ns_value->value().array<opcua::String>();
    for (std::size_t i = 1; i < uris.size(); ++i) {
      dump.namespace_uris.emplace_back(ToString(uris[i]));
    }
  }

  std::deque<opcua::NodeId> queue;
  std::unordered_set<std::string> visited;
  const opcua::NodeId root = ParseNodeId(root_node);
  queue.push_back(root);
  visited.insert(FormatNodeId(root));

  while (!queue.empty() && dump.nodes.size() < max_nodes) {
    const opcua::NodeId id = queue.front();
    queue.pop_front();

    // Forward references from this node (all reference types, subtypes
    // included): these define the node's place in the hierarchy and feed the
    // crawl frontier.
    opcua::BrowseDescription description(
        id, opcua::BrowseDirection::Forward, opcua::ReferenceTypeId::References,
        true, opcua::NodeClass::Unspecified, opcua::BrowseResultMask::All);
    auto refs = opcua::services::browseAll(client, description);

    const auto attributes = ReadNodeAttributes(client, id, include_values);
    if (attributes.size() < kValueSlot || !attributes[0].hasValue()) {
      // Could not read even the NodeClass: skip (e.g. a dangling reference
      // target that is not a real node).
      continue;
    }

    NodesetNode node;
    node.node_id = FormatNodeId(id);
    node.node_class = NodeClassName(
        static_cast<opcua::NodeClass>(attributes[0].value().scalar<int32_t>()));
    if (attributes[1].hasValue()) {
      const auto browse = attributes[1].value().scalar<opcua::QualifiedName>();
      node.browse_ns = browse.namespaceIndex();
      node.browse_name = ToString(browse.name());
    }
    if (attributes[2].hasValue()) {
      node.display_name =
          ToString(attributes[2].value().scalar<opcua::LocalizedText>().text());
    }
    if (attributes[3].hasValue()) {
      node.description =
          ToString(attributes[3].value().scalar<opcua::LocalizedText>().text());
    }
    if (attributes[4].hasValue()) {
      node.data_type =
          FormatNodeId(attributes[4].value().scalar<opcua::NodeId>());
    }
    if (attributes[5].hasValue()) {
      node.value_rank = attributes[5].value().scalar<int32_t>();
    }
    if (attributes[6].hasValue()) {
      node.is_abstract = attributes[6].value().scalar<bool>();
    }
    if (attributes[7].hasValue()) {
      node.symmetric = attributes[7].value().scalar<bool>();
    }
    if (attributes[8].hasValue()) {
      node.inverse_name =
          ToString(attributes[8].value().scalar<opcua::LocalizedText>().text());
    }
    if (include_values && attributes.size() > kValueSlot) {
      node.value_preview = ExtractValuePreview(attributes[kValueSlot]);
    }

    for (const auto& ref : refs.value()) {
      if (!ref.isForward()) {
        continue;
      }
      NodesetReference reference;
      reference.reference_type = FormatNodeId(ref.referenceTypeId());
      reference.target = FormatNodeId(ref.nodeId().nodeId());
      node.references.push_back(std::move(reference));

      // Enqueue local, not-yet-seen targets. A null target (i=0) is never a
      // real node, so browsing/reading it is pointless — and it can fail-stop
      // servers that assert on a null routed NodeId, so never crawl into it.
      // GUID identifiers are skipped (BrowseNode does the same) because they
      // are not addressable here.
      if (ref.nodeId().isLocal() && !ref.nodeId().nodeId().isNull() &&
          ref.nodeId().nodeId().identifierType() != opcua::NodeIdType::Guid) {
        const std::string target = FormatNodeId(ref.nodeId().nodeId());
        if (visited.insert(target).second) {
          queue.push_back(ref.nodeId().nodeId());
        }
      }
    }

    const bool in_scope =
        !namespace_filter ||
        id.namespaceIndex() ==
            static_cast<opcua::NamespaceIndex>(*namespace_filter);
    if (in_scope) {
      dump.nodes.push_back(std::move(node));
    }
  }

  return dump;
}

std::string StatusToString(std::uint32_t status_code) {
  return StatusToString(opcua::StatusCode(status_code));
}
