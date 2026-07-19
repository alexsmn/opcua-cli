#include "nodeset.h"

#include <cctype>
#include <filesystem>
#include <fstream>
#include <regex>
#include <stdexcept>
#include <string_view>

namespace {

std::string SanitizeIdentifier(std::string value) {
  for (char& ch : value) {
    if (!std::isalnum(static_cast<unsigned char>(ch))) {
      ch = '_';
    }
  }
  if (value.empty() ||
      std::isdigit(static_cast<unsigned char>(value.front()))) {
    value = "_" + value;
  }
  return value;
}

std::string NamespaceOpen(const std::string& ns) {
  std::string out;
  std::size_t start = 0;
  while (start < ns.size()) {
    std::size_t pos = ns.find("::", start);
    std::string part = ns.substr(
        start, pos == std::string::npos ? std::string::npos : pos - start);
    if (!part.empty()) {
      out += "namespace " + part + " {\n";
    }
    if (pos == std::string::npos)
      break;
    start = pos + 2;
  }
  return out;
}

std::string NamespaceClose(const std::string& ns) {
  int count = 1;
  for (std::size_t pos = ns.find("::"); pos != std::string::npos;
       pos = ns.find("::", pos + 2)) {
    ++count;
  }
  std::string out;
  for (int i = 0; i < count; ++i) {
    out += "}  // namespace\n";
  }
  return out;
}

}  // namespace

void GenerateNodeset(const std::string& input,
                     const std::string& output_dir,
                     const std::string& cpp_namespace) {
  std::ifstream in(input);
  if (!in) {
    throw std::runtime_error("Cannot open NodeSet file: " + input);
  }
  std::string xml((std::istreambuf_iterator<char>(in)),
                  std::istreambuf_iterator<char>());
  std::filesystem::create_directories(output_dir);

  std::ofstream header(std::filesystem::path(output_dir) / "NodeIds.h");
  header << "#ifndef GENERATED_NODEIDS_H_\n";
  header << "#define GENERATED_NODEIDS_H_\n\n";
  header << "#include <string_view>\n\n";
  header << NamespaceOpen(cpp_namespace) << "\nstruct NodeIds {\n";

  std::regex node_regex(
      R"(<UA(?:Object|Variable|Method|ObjectType|VariableType|DataType|ReferenceType|View)\s+[^>]*>)");
  std::regex node_id_regex(R"(NodeId=\"([^\"]+)\")");
  std::regex browse_name_regex(R"(BrowseName=\"([^\"]+)\")");
  int anonymous_index = 0;
  for (std::sregex_iterator it(xml.begin(), xml.end(), node_regex), end;
       it != end; ++it) {
    std::string tag = (*it)[0].str();
    std::smatch node_id_match;
    if (!std::regex_search(tag, node_id_match, node_id_regex)) {
      continue;
    }
    std::string node_id = node_id_match[1].str();
    std::string browse_name;
    std::smatch browse_name_match;
    if (std::regex_search(tag, browse_name_match, browse_name_regex)) {
      browse_name = browse_name_match[1].str();
    }
    auto colon = browse_name.find(':');
    if (colon != std::string::npos) {
      browse_name = browse_name.substr(colon + 1);
    }
    if (browse_name.empty()) {
      browse_name = "Node_" + std::to_string(anonymous_index++);
    }
    header << "  static constexpr std::string_view "
           << SanitizeIdentifier(browse_name) << " = \"" << node_id << "\";\n";
  }

  header << "};\n\n"
         << NamespaceClose(cpp_namespace)
         << "\n#endif  // GENERATED_NODEIDS_H_\n";
}

namespace {

std::string XmlEscape(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (char ch : value) {
    switch (ch) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      default:
        out += ch;
    }
  }
  return out;
}

// UANodeSet element name for a NodeClass string.
std::string ElementName(const std::string& node_class) {
  return "UA" + node_class;
}

}  // namespace

void WriteNodeset(const std::string& output_file, const NodesetDump& dump) {
  std::ofstream out(output_file);
  if (!out) {
    throw std::runtime_error("Cannot write NodeSet file: " + output_file);
  }

  out << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
  out << "<UANodeSet "
         "xmlns=\"http://opcfoundation.org/UA/2011/03/UANodeSet.xsd\" "
         "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\n";

  // NamespaceUris lists the server's namespaces from index 1 onward; index 0
  // (the OPC UA namespace) is implicit and omitted, per the UANodeSet schema.
  if (!dump.namespace_uris.empty()) {
    out << "  <NamespaceUris>\n";
    for (const auto& uri : dump.namespace_uris) {
      out << "    <Uri>" << XmlEscape(uri) << "</Uri>\n";
    }
    out << "  </NamespaceUris>\n";
  }

  for (const auto& node : dump.nodes) {
    const std::string element = ElementName(node.node_class);
    out << "  <" << element << " NodeId=\"" << XmlEscape(node.node_id)
        << "\" BrowseName=\"" << node.browse_ns << ":"
        << XmlEscape(node.browse_name) << "\"";
    if (node.data_type)
      out << " DataType=\"" << XmlEscape(*node.data_type) << "\"";
    if (node.value_rank)
      out << " ValueRank=\"" << *node.value_rank << "\"";
    if (node.is_abstract && *node.is_abstract)
      out << " IsAbstract=\"true\"";
    if (node.symmetric && *node.symmetric)
      out << " Symmetric=\"true\"";
    out << ">\n";

    out << "    <DisplayName>" << XmlEscape(node.display_name)
        << "</DisplayName>\n";
    if (!node.description.empty()) {
      out << "    <Description>" << XmlEscape(node.description)
          << "</Description>\n";
    }
    if (node.inverse_name && !node.inverse_name->empty()) {
      out << "    <InverseName>" << XmlEscape(*node.inverse_name)
          << "</InverseName>\n";
    }
    // The live value is informational only: emitting a typed <Value> would
    // require full DataValue encoding and could not round-trip every type, so
    // it is preserved as a comment instead of an element.
    if (node.value_preview && !node.value_preview->empty()) {
      out << "    <!-- Value: " << XmlEscape(*node.value_preview) << " -->\n";
    }

    if (!node.references.empty()) {
      out << "    <References>\n";
      for (const auto& ref : node.references) {
        out << "      <Reference ReferenceType=\""
            << XmlEscape(ref.reference_type) << "\">" << XmlEscape(ref.target)
            << "</Reference>\n";
      }
      out << "    </References>\n";
    }

    out << "  </" << element << ">\n";
  }

  out << "</UANodeSet>\n";
}
