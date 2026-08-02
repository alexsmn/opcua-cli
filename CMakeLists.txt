cmake_minimum_required(VERSION 3.28)

project(opcua_cli_cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(open62541pp CONFIG REQUIRED)
find_package(Boost CONFIG REQUIRED COMPONENTS json)

add_executable(opcua-cli
  src/cli.cpp
  src/nodeset.cpp
  src/opcua_client.cpp
  src/span_context.cpp
)

target_include_directories(opcua-cli PRIVATE src)
target_compile_features(opcua-cli PRIVATE cxx_std_23)
target_link_libraries(opcua-cli PRIVATE Boost::json open62541pp::open62541pp)

install(TARGETS opcua-cli RUNTIME DESTINATION bin)
