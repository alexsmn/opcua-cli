#ifndef SRC_NODESET_H_
#define SRC_NODESET_H_

#include <optional>
#include <string>

void GenerateNodeset(const std::string& input,
                     const std::string& output_dir,
                     const std::string& cpp_namespace);
void DumpNodesetPlaceholder(const std::string& output_file,
                            std::optional<int> namespace_index);

#endif  // SRC_NODESET_H_
