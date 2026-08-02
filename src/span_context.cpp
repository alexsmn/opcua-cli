#include "span_context.h"

#include <cstddef>
#include <stdexcept>
#include <string_view>

namespace {

// NodeId of the DefaultBinary encoding of SpanContextDataType. OPC UA Part 26
// §5.6.2 Table 11,
// https://reference.opcfoundation.org/Core/Part26/v105/docs/5.6.2
constexpr std::uint16_t kSpanContextEncodingId = 19754;

// The key Part 26 §5.6.4 assigns to the SpanContextDataType parameter,
// https://reference.opcfoundation.org/Core/Part26/v105/docs/5.6.4
constexpr std::string_view kSpanContextKey = "SpanContext";

// The W3C key this stack has carried since before Part 26 alignment.
constexpr std::string_view kTraceParentKey = "traceparent";

// Variant encoding-mask values are the built-in type ids of OPC UA Part 6
// §5.1.2 Table 1. Only the low six bits are the type; the array bits stay
// clear because both values here are scalars.
constexpr std::uint8_t kVariantTypeString = 12;
constexpr std::uint8_t kVariantTypeExtensionObject = 22;

// ExtensionObject body encoding of OPC UA Part 6 §5.2.2.15: 0x01 means the
// body is a ByteString.
constexpr std::uint8_t kExtensionObjectByteStringBody = 0x01;

// NodeId encoding of OPC UA Part 6 §5.2.2.9 Table 6: 0x01 is the FourByte
// form, a Byte namespace index and a UInt16 identifier.
constexpr std::uint8_t kNodeIdFourByte = 0x01;

void AppendUInt8(std::vector<std::uint8_t>& out, std::uint8_t value) {
  out.push_back(value);
}

// OPC UA Part 6 §5.2.2.1: integers are little endian on the wire.
void AppendUInt16(std::vector<std::uint8_t>& out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value));
  out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void AppendUInt32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8)
    out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void AppendInt32(std::vector<std::uint8_t>& out, std::int32_t value) {
  AppendUInt32(out, static_cast<std::uint32_t>(value));
}

void AppendUInt64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8)
    out.push_back(static_cast<std::uint8_t>(value >> shift));
}

// OPC UA Part 6 §5.2.2.4: an Int32 byte count followed by the UTF-8 bytes.
// Only non-null strings are written here, so the -1 null length never occurs.
void AppendString(std::vector<std::uint8_t>& out, std::string_view text) {
  AppendInt32(out, static_cast<std::int32_t>(text.size()));
  out.insert(out.end(), text.begin(), text.end());
}

// OPC UA Part 6 §5.2.2.9 Table 6, FourByte form. Both encoding ids used here
// are numeric, in namespace 0, and below 65536, so the compact form applies.
void AppendNumericNodeId(std::vector<std::uint8_t>& out,
                         std::uint16_t identifier) {
  AppendUInt8(out, kNodeIdFourByte);
  AppendUInt8(out, 0);  // namespace index
  AppendUInt16(out, identifier);
}

// A QualifiedName is a UInt16 namespace index then a String (Part 6 §5.2.2.13).
void AppendQualifiedName(std::vector<std::uint8_t>& out,
                         std::string_view name) {
  AppendUInt16(out, 0);
  AppendString(out, name);
}

int HexDigit(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

// Strips the optional dashes of a canonical Guid and checks what is left is
// `expected_digits` hex digits.
std::string NormalizeHex(std::string_view text,
                         std::size_t expected_digits,
                         std::string_view what) {
  std::string digits;
  digits.reserve(text.size());
  for (char c : text) {
    if (c == '-')
      continue;
    if (HexDigit(c) < 0) {
      throw std::runtime_error(std::string(what) +
                               ": not a hex digit: " + std::string(1, c));
    }
    digits.push_back(c);
  }
  if (digits.size() != expected_digits) {
    throw std::runtime_error(
        std::string(what) + ": expected " + std::to_string(expected_digits) +
        " hex digits, got " + std::to_string(digits.size()));
  }
  return digits;
}

std::uint64_t HexToUInt64(std::string_view digits) {
  std::uint64_t value = 0;
  for (char c : digits)
    value = (value << 4) | static_cast<std::uint64_t>(HexDigit(c));
  return value;
}

// Encodes a Guid per OPC UA Part 6 §5.2.2.6: it is NOT an opaque 16-byte
// array. Part 3 §8.14 gives it four fields, and the first three are integers
// written little endian, so their bytes come out reversed relative to the
// canonical text. Only Data4 travels verbatim.
//
// This is the convention under test. The 32 hex digits are read as the
// canonical 8-4-4-4-12 text form, so `4bf92f35-...` puts 0x35 on the wire
// first. Reading the same digits as raw bytes would put 0x4b first and
// transpose the leading eight bytes with no error anywhere.
void AppendGuid(std::vector<std::uint8_t>& out, std::string_view hex32) {
  const std::uint32_t data1 =
      static_cast<std::uint32_t>(HexToUInt64(hex32.substr(0, 8)));
  const std::uint16_t data2 =
      static_cast<std::uint16_t>(HexToUInt64(hex32.substr(8, 4)));
  const std::uint16_t data3 =
      static_cast<std::uint16_t>(HexToUInt64(hex32.substr(12, 4)));

  AppendUInt32(out, data1);
  AppendUInt16(out, data2);
  AppendUInt16(out, data3);
  for (std::size_t i = 16; i < 32; i += 2) {
    out.push_back(static_cast<std::uint8_t>(HexDigit(hex32[i]) * 16 +
                                            HexDigit(hex32[i + 1])));
  }
}

// The SpanContextDataType body: Guid TraceId then UInt64 SpanId, the two
// fields of Part 26 §5.6.2 Table 11, in declaration order and with no
// structure header of their own (Part 6 §5.2.6 encodes a Structure as its
// fields back to back).
std::vector<std::uint8_t> EncodeSpanContextBody(std::string_view trace_id_hex,
                                                std::string_view span_id_hex) {
  std::vector<std::uint8_t> body;
  AppendGuid(body, trace_id_hex);
  AppendUInt64(body, HexToUInt64(span_id_hex));
  return body;
}

// An ExtensionObject carrying an already-encoded body as a ByteString
// (Part 6 §5.2.2.15): NodeId of the encoding, the 0x01 encoding byte, then an
// Int32-prefixed ByteString.
void AppendExtensionObject(std::vector<std::uint8_t>& out,
                           std::uint16_t encoding_id,
                           const std::vector<std::uint8_t>& body) {
  AppendNumericNodeId(out, encoding_id);
  AppendUInt8(out, kExtensionObjectByteStringBody);
  AppendInt32(out, static_cast<std::int32_t>(body.size()));
  out.insert(out.end(), body.begin(), body.end());
}

}  // namespace

EncodedAdditionalHeader EncodeAdditionalHeader(const TraceContext& trace) {
  // AdditionalParametersType is a Structure whose single field is an array of
  // KeyValuePair, so its body is an Int32 element count followed by the
  // elements (Part 6 §5.2.5 for the array, §5.2.6 for the structure).
  std::vector<std::uint8_t> parameters;
  std::int32_t count = 0;

  if (trace.traceparent.has_value()) {
    AppendQualifiedName(parameters, kTraceParentKey);
    AppendUInt8(parameters, kVariantTypeString);
    AppendString(parameters, *trace.traceparent);
    ++count;
  }

  if (trace.span_trace_id.has_value() || trace.span_id.has_value()) {
    // Either half alone is a legitimate thing to send — a caller testing what
    // a server does with a null TraceId should not have to supply a SpanId —
    // so the missing half defaults to the all-zero value Part 26 leaves
    // meaningless rather than to an error.
    const std::string trace_id_hex = NormalizeHex(
        trace.span_trace_id.value_or(std::string(32, '0')), 32, "trace id");
    const std::string span_id_hex = NormalizeHex(
        trace.span_id.value_or(std::string(16, '0')), 16, "span id");

    AppendQualifiedName(parameters, kSpanContextKey);
    AppendUInt8(parameters, kVariantTypeExtensionObject);
    AppendExtensionObject(parameters, kSpanContextEncodingId,
                          EncodeSpanContextBody(trace_id_hex, span_id_hex));
    ++count;
  }

  EncodedAdditionalHeader encoded;
  AppendInt32(encoded.body, count);
  encoded.body.insert(encoded.body.end(), parameters.begin(), parameters.end());

  AppendExtensionObject(encoded.full, kAdditionalParametersEncodingId,
                        encoded.body);
  return encoded;
}

std::string ToHex(const std::vector<std::uint8_t>& bytes) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(bytes.size() * 2);
  for (std::uint8_t byte : bytes) {
    hex.push_back(kDigits[byte >> 4]);
    hex.push_back(kDigits[byte & 0x0f]);
  }
  return hex;
}
