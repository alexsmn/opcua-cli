# opcua-cli C++ port

This is a C++23 port of the `php-opcua/opcua-cli` v4.4.x command surface,
ported from the upstream documentation at
https://www.php-opcua.com/documentation/opcua-cli/v4.4.x.

It uses the `open62541pp` C++ OPC UA client wrapper from vcpkg.

## Build

The default preset uses the sibling vcpkg checkout at `../vcpkg` and vcpkg
manifest mode to install `open62541pp` plus required host tooling:

```sh
cmake --preset vcpkg
cmake --build --preset vcpkg
```

If your vcpkg checkout lives elsewhere, pass
`-DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake`.

## Releases

GitHub Actions builds release archives for Linux x64 and macOS arm64 when a
version tag is pushed:

```sh
git tag v0.1.0
git push origin v0.1.0
```

The workflow publishes a GitHub Release with packaged `opcua-cli` binaries.

## Commands

```sh
opcua-cli browse opc.tcp://localhost:4840 [/Objects|NODEID] [--recursive] [--depth=N] [--json]
opcua-cli read opc.tcp://localhost:4840 NODEID [--attribute=Value] [--json]
opcua-cli write opc.tcp://localhost:4840 NODEID VALUE [--type=Int32] [--json]
opcua-cli endpoints opc.tcp://localhost:4840 [--json]
opcua-cli watch opc.tcp://localhost:4840 NODEID [--interval=250] [--json]
opcua-cli generate:nodeset path/to/NodeSet2.xml [--output=generated] [--namespace=Generated::OpcUa]
opcua-cli dump:nodeset opc.tcp://localhost:4840 --output=Server.NodeSet2.xml [--namespace=N]
```

Global security and output flags mirror the PHP CLI where `open62541pp` exposes
the feature through its stock client configuration:

```sh
--security-policy=POLICY|-s POLICY
--security-mode=MODE|-m MODE
--cert=PATH
--key=PATH
--ca=PATH
--username=USER|-u USER
--password=PASS|-p PASS
--timeout=SECONDS|-t SECONDS
--json|-j
--debug|-d
--debug-stderr
--debug-file=PATH
--help|-h
--version|-v
```

Certificate loading and advanced security policy selection are represented in the
CLI contract and validated, but require an `open62541pp`/`open62541` build with
encryption plugins enabled.

## License

MIT. See [LICENSE](LICENSE).
