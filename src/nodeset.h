#ifndef SRC_NODESET_H_
#define SRC_NODESET_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Generates a C++ NodeIds header from a NodeSet2 XML file (offline).
void GenerateNodeset(const std::string& input,
                     const std::string& output_dir,
                     const std::string& cpp_namespace);

// A single forward reference emitted on a node. reference_type and target are
// NodeId strings (e.g. "i=47", "ns=1;i=5"); NodeId form is always valid for
// the ReferenceType/target of a UANodeSet reference.
struct NodesetReference {
  std::string reference_type;
  std::string target;
};

// One node captured from a live server, with the attributes relevant to its
// NodeClass. Attribute fields that do not apply to the class (or that the
// server did not return) stay empty/nullopt.
struct NodesetNode {
  std::string node_id;      // NodeId string
  std::string node_class;   // "Object", "Variable", "Method", ...
  std::uint16_t browse_ns = 0;
  std::string browse_name;  // local part of the BrowseName
  std::string display_name;
  std::string description;
  // Variable / VariableType.
  std::optional<std::string> data_type;   // NodeId string
  std::optional<std::int32_t> value_rank;
  std::optional<std::string> value_preview;  // current value, emitted as a comment
  // ObjectType / VariableType / DataType / ReferenceType.
  std::optional<bool> is_abstract;
  // ReferenceType.
  std::optional<bool> symmetric;
  std::optional<std::string> inverse_name;
  std::vector<NodesetReference> references;  // forward references
};

// A crawled address space: the server's namespace URIs (wire index order,
// index 1 first — index 0 is the OPC UA namespace and is implicit) plus the
// captured nodes.
struct NodesetDump {
  std::vector<std::string> namespace_uris;
  std::vector<NodesetNode> nodes;
};

// Serializes a crawled address space to a UANodeSet XML file
// (http://opcfoundation.org/UA/2011/03/UANodeSet.xsd).
void WriteNodeset(const std::string& output_file, const NodesetDump& dump);

#endif  // SRC_NODESET_H_
