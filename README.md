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
opcua-cli events opc.tcp://localhost:4840 [NODEID] [--duration=SECONDS] [--count=N] [--select=FIELD,...] [--json]
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

`events` subscribes to a node's **events** rather than to a value: it creates a
subscription and a monitored item on the `EventNotifier` attribute with an
`EventFilter`, and prints one line per event, flushed per event. `NODEID`
defaults to `i=2253`, the Server object, which every server exposes as an event
notifier. `--duration` and `--count` bound the run exactly as they do for
`watch`.

The default select clauses are the `BaseEventType` fields every conforming
server defines — `EventId`, `EventType`, `SourceNode`, `SourceName`, `Time`,
`ReceiveTime`, `Message`, `Severity`
([OPC UA Part 5 §6.4.2](https://reference.opcfoundation.org/Core/Part5/v105/docs/6.4.2)).
`--select` replaces that list. A field is a browse path below `BaseEventType`:
`Message`, a namespace-qualified `2:VendorCode`, or a nested `2:Vendor/Code`.

```sh
# the next five events on the Server object
opcua-cli events opc.tcp://localhost:4840 --count=5
# just the identity fields, for 30 seconds, as JSON lines
opcua-cli events opc.tcp://localhost:4840 i=2253 --duration=30 --select=EventId,SourceNode,Time --json
```

Human output is space-separated `Name=Value` pairs in the order requested; a
value containing whitespace or a quote is quoted and escaped so the pairs stay
separable. Under `--json` each event is one object, `{"Seq":N,"Fields":{…}}`,
with typed field values.

```
$ opcua-cli events opc.tcp://localhost:4840 --count=1
EventId=43db88e4d97e57bf53220cac05523184 EventType=i=2041 SourceNode=i=2253 SourceName=Server Time=2026-08-01T16:37:54Z ReceiveTime=2026-08-01T16:37:54Z Message="test event" Severity=500
```

Everything about the subscription itself — the confirmation line, a node that
reports no `EventNotifier`, select clauses the server accepted the item with but
answered Bad for — goes to **stderr**, so stdout stays a clean stream of events.
A rejected subscription is an error with a `Hint` naming the cause, and exits
nonzero:

```
$ opcua-cli events opc.tcp://localhost:4840 i=85
error: Event subscription on i=85 rejected: BadNotSupported (0x803D0000)
  Hint: this node has no EventNotifier attribute, so it raises no events — the server rejected the request, the connection is fine.
  Every server exposes the Server object i=2253 as an event notifier; try that, or browse for a node whose EventNotifier has the SubscribeToEvents bit set.
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
- ByteStrings — an `EventId` above all — are rendered as lowercase hex, cut at
  64 bytes with the true length appended. **A bad one stays visible, in all
  three of its shapes.** A field the server sent with no value at all reads
  `<null>` (JSON `null`), a zero-length ByteString reads `<empty>`, and an
  all-zero one renders as its true hex (`0000000000000000`) — never blanked,
  normalised or abbreviated. OPC UA Part 5 §6.4.2 makes `EventId` mandatory on
  every event, so any of the three is a server or proxy defect, and `events`
  exists in large part to make them observable.

  The third shape is the one that matters in practice and the easiest to miss.
  A phantom event reassembled from a payload-less notification carries
  `EncodeEventIdByteString(0)`: **eight zero bytes — a well-formed ByteString
  of the correct length**, neither null nor empty. A check written only against
  `null` would wave it through as a valid id. Screen for all three:

  ```sh
  opcua-cli events … --json |
    jq 'select(.Fields.EventId == null or .Fields.EventId == "<empty>"
               or (.Fields.EventId | test("^0+$")))'
  ```
- `read` explains status codes that look like tool failures but are the
  server's correct answer. `--attribute=Value` on an Object node returns
  `BadAttributeIdInvalid`; the CLI adds a `Hint` line naming the node class and
  pointing at `--attribute=DisplayName` or `browse`.
- stdout carries only command output (including `--json`); client logs go to
  stderr and show warnings and errors only. Each of `--debug`, `--debug-stderr`
  and `--debug-file=PATH` enables full logging on its own and picks where it
  goes — stdout, stderr, or a file respectively. They are destinations, not
  modifiers: none of them needs to be combined with another to take effect.
- `write` exits with code 1 when the server answers with a Bad status, so
  scripts can detect rejected writes without parsing output.
- `browse` does the same, and never reports a server error as an empty node.
  A refused browse prints the status and a hint on stderr, leaves stdout empty,
  and exits 1; under `--json` it emits `{"nodes": [...], "Status": ..., "Hint":
  ...}` instead of the bare array. In a `--recursive` crawl a node whose own
  children could not be listed is marked `!! browse failed: STATUS` in the tree
  and carries `childStatus` in JSON, so a refusal is never mistaken for a leaf:

  ```
  $ opcua-cli browse opc.tcp://localhost:4840 'ns=99;i=424242'
  error: browse failed: BadNodeIdUnknown (0x80340000)
  hint:  The server has no node with this NodeId. Check the namespace index — ...
  ```
- Status codes missing from open62541's name table (vendor-specific server
  codes) are reported by severity, e.g. `Bad (vendor-specific) (0x80300000)`.

## License

MIT. See [LICENSE](LICENSE).
