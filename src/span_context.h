#ifndef SRC_SPAN_CONTEXT_H_
#define SRC_SPAN_CONTEXT_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// NodeId identifier, in namespace 0, of
// AdditionalParametersType_Encoding_DefaultBinary — the type of the
// ExtensionObject RequestHeader.additionalHeader carries. Exposed because a
// caller handing the encoded body to a stack's own ExtensionObject has to
// stamp the same typeId on it. OPC UA Part 4 §7.33,
// https://reference.opcfoundation.org/Core/Part4/v105/docs/7.33
inline constexpr std::uint16_t kAdditionalParametersEncodingId = 17537;

// Trace context to hang off a request's RequestHeader.additionalHeader.
// Either entry may be set on its own, or both together — OPC UA Part 4 §7.33
// makes the extension slot a list, so carrying two keys is legal and a peer
// must ignore whichever it does not understand.
struct TraceContext {
  // The OPC UA Part 26 §5.6.4 "SpanContext" entry. `span_trace_id` is 32 hex
  // digits read as the Guid's canonical 8-4-4-4-12 text form (dashes
  // optional), `span_id` is 16 hex digits read as a big-endian UInt64.
  //
  // Both are taken verbatim, all-zero values included: a null TraceId and a
  // zero SpanId are exactly the inputs worth sending, because Part 26 gives
  // them no meaning and a server has to drop them without failing the request.
  std::optional<std::string> span_trace_id;
  std::optional<std::string> span_id;
  // The W3C "traceparent" entry, sent verbatim as a String.
  std::optional<std::string> traceparent;

  bool empty() const {
    return !span_trace_id.has_value() && !span_id.has_value() &&
           !traceparent.has_value();
  }
};

// An encoded AdditionalParametersType extension in the two forms a caller
// needs. `body` is the ByteString that goes *inside* the outer ExtensionObject
// — open62541 writes that envelope itself from the UA_ExtensionObject fields —
// while `full` is the complete octet string including the envelope, which is
// what a packet capture shows and what `--dump-header` prints.
struct EncodedAdditionalHeader {
  std::vector<std::uint8_t> body;
  std::vector<std::uint8_t> full;
};

// Encodes `trace` as an OPC UA Part 26 §5.6.4 AdditionalParametersType
// key/value list, by hand, straight from the OPC UA Part 6 binary encoding
// rules. Deliberately hand-rolled and dependency-free: open62541 has no
// Part 26 DataTypes, and an encoder written independently of the server's is
// the entire point of this path.
//
// Throws std::runtime_error when a hex field is not the right length or holds
// a non-hex digit.
EncodedAdditionalHeader EncodeAdditionalHeader(const TraceContext& trace);

// Lowercase hex, no separators.
std::string ToHex(const std::vector<std::uint8_t>& bytes);

#endif  // SRC_SPAN_CONTEXT_H_
