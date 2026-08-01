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

On Windows with Visual Studio 2022 and a sibling vcpkg checkout:

```sh
cmake --preset vcpkg-windows
cmake --build --preset vcpkg-windows --config Debug
```

If your vcpkg checkout lives elsewhere, pass
`-DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake`.

## Releases

GitHub Actions builds release archives for Linux x64, macOS arm64, and
Windows x64 when a version tag is pushed:

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
opcua-cli watch opc.tcp://localhost:4840 NODEID [--interval=250] [--duration=SECONDS] [--count=N] [--json]
opcua-cli generate:nodeset path/to/NodeSet2.xml [--output=generated] [--namespace=Generated::OpcUa]
opcua-cli dump:nodeset opc.tcp://localhost:4840 --output=Server.NodeSet2.xml [--namespace=N] [--root=NODEID] [--max-nodes=N] [--values]
```

`watch` polls a node's Value and prints one sample per line, flushing each
tick. It runs until interrupted unless you bound it: `--duration=SECONDS`
stops after that much wall-clock time, `--count=N` after N samples, and with
both it stops at whichever comes first. `--interval` is the gap between
samples in milliseconds (default 1000). None of these are `--timeout`, which
is the connect/request timeout and is unrelated.

```sh
# ten samples, quarter-second apart
opcua-cli watch opc.tcp://localhost:4840 ns=2;i=41 --interval=250 --count=10
# whatever arrives in the next 30 seconds, as JSON lines
opcua-cli watch opc.tcp://localhost:4840 ns=2;i=41 --duration=30 --json
```

`dump:nodeset` crawls the server's address space breadth-first from `--root`
(default `i=84`, the Root node), following forward references, and writes a
UANodeSet XML with each node's class, browse name, display name, description,
per-class attributes, and forward references, plus the server's namespace URIs.
`--namespace=N` restricts the emitted nodes to namespace index `N` (the crawl
still traverses other namespaces to reach them); `--max-nodes` bounds the
crawl. Node values are not read by default because reading device-backed
variables can block on device I/O — pass `--values` to include each variable's
current value as a comment.

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

## Transport support

**Only `opc.tcp://` works.** `opc.wss://`, `opc.ws://`, `wss://` and `https://`
endpoints are rejected up front with an explanatory error rather than a bare
`BadTcpEndpointUrlInvalid`. This is a hard limitation, not an oversight:

- `open62541` 1.4 ships connection managers for POSIX TCP, UDP, Ethernet and
  MQTT only — there is no WebSocket transport to configure. (The 1.3-era
  libwebsockets support was server-side and was dropped in 1.4.) `open62541pp`
  is a thin wrapper and adds no transport of its own.
- The client also filters discovered endpoints on
  `transportProfileUri == …/uatcp-uasc-uabinary`, so a WebSocket endpoint would
  be rejected during endpoint selection even if a transport existed.
- This vcpkg build has no TLS backend at all (`UA_ENABLE_ENCRYPTION_MBEDTLS`,
  `_OPENSSL` and `_LIBRESSL` are all off), so there is nothing to layer TLS on.

For a server whose WebSocket endpoint carries **UA Binary** (subprotocol
`opcua+uacp`), a byte-level WSS↔TCP shim would be enough, because the framed
payload is the same UACP byte stream open62541 already speaks. That is not what
our deployments serve: the SCADA server's `opc.wss://` endpoint negotiates
`opcua+uajson` only and rejects `opcua+uacp` outright, because it is aimed at
the browser client. UA-JSON is a different wire encoding, and that endpoint
deliberately implements neither UA discovery (`GetEndpoints`, `FindServers`)
nor UA SecureChannel. Supporting it would mean writing a second OPC UA client
stack — TLS, WebSocket, UA-JSON envelopes and session establishment — beside
`open62541`, not adding a flag to this one.

Until then, reach the server's `opc.tcp://` endpoint directly. When it is not
routable — for instance when only an HTTPS reverse-proxy path is published —
tunnel it:

```sh
ssh -L 4840:localhost:4840 HOST
opcua-cli read opc.tcp://localhost:4840 i=2255
```

## Output conventions

- Array values are rendered element by element. `read` prints a
  `Type[count]` header and then one indexed line per element, because for the
  arrays worth reading — `i=2255` NamespaceArray above all — element N *is*
  index N:

  ```
  $ opcua-cli read opc.tcp://localhost:4840 i=2255
  Value:      String[3]
    [0] http://opcfoundation.org/UA/
    [1] urn:host:Telecontrol:Server
    [2] http://telecontrol.ru/opcua/filesystem/FileType
  ```

  Under `--json` the same value is a real JSON array with typed elements
  (numbers stay numbers), plus a `Count` field — never the string `<array>`.
  `watch` keeps one line per sample and renders arrays inline as `[a, b, c]`.
  A typed empty array reads as `String[0]` / `[]`, distinct from a `null`
  value. Floating-point values use the shortest round-trip form, so a `1e-09`
  reading is reported as `1e-09` and not rounded to `0.000000`.
- NodeIds are printed in canonical form (`i=2253`, `ns=2;i=1001`) in every
  command and in `--json`, so they can be pasted straight into another
  invocation.
- `read` explains status codes that look like tool failures but are the
  server's correct answer. `--attribute=Value` on an Object node returns
  `BadAttributeIdInvalid`; the CLI adds a `Hint` line naming the node class and
  pointing at `--attribute=DisplayName` or `browse`.
- stdout carries only command output (including `--json`); client logs go to
  stderr and show warnings and errors only. `--debug` enables full logging
  (on stdout, or stderr with `--debug-stderr`, or a file with
  `--debug-file=PATH`).
- `write` exits with code 1 when the server answers with a Bad status, so
  scripts can detect rejected writes without parsing output.
- Status codes missing from open62541's name table (vendor-specific server
  codes) are reported by severity, e.g. `Bad (vendor-specific) (0x80300000)`.

## License

MIT. See [LICENSE](LICENSE).
