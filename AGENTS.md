# AGENTS.md

Guidance for coding agents working in this repository.

## What this is

`opcua-cli` is a C++23 OPC UA **diagnostic client** — a command-line tool for
poking at a live OPC UA server: browse the address space, read and write
attributes, watch a value change, dump the address space to a UANodeSet XML.

It is a port of the `php-opcua/opcua-cli` v4.4.x command surface (upstream docs:
https://www.php-opcua.com/documentation/opcua-cli/v4.4.x), built on
[`open62541pp`](https://github.com/open62541pp/open62541pp), the C++ wrapper
over `open62541`.

This is a **standalone repository**. It is a sibling of the `scada` tree, not a
part of it and not a submodule — do not reach into `../scada` for build inputs,
and do not assume anything in this repo is shared with it.

## Layout

| Path | Contents |
|---|---|
| `src/cli.cpp` | Argument parsing, command dispatch, all human and `--json` output formatting. |
| `src/opcua_client.{h,cpp}` | The `open62541pp` client wrapper: connect, browse, read, write, endpoints, address-space crawl. All OPC UA value rendering lives here. |
| `src/nodeset.{h,cpp}` | UANodeSet XML reading (`generate:nodeset`) and writing (`dump:nodeset`). |

The whole tool is one executable target; there is no library split and no test
suite yet.

## Build

The default preset uses the **sibling vcpkg checkout at `../vcpkg`** in manifest
mode:

```sh
cmake --preset vcpkg
cmake --build --preset vcpkg
```

The binary lands at `build/vcpkg-make/opcua-cli`.

`cmake` is not on `PATH` in every shell on macOS here — it lives at
`/Applications/CMake.app/Contents/bin/cmake`. Prepend it rather than assuming
the bare name resolves, and never pipe a build through `tail`/`grep` without
checking the exit status: a pipeline reports the *last* command's status, so a
`command not found` on `cmake` silently looks like a successful build.

## Conventions

- Follow the surrounding style: Google-ish C++ (`.clang-format` is `BasedOnStyle: Chromium`),
  `PascalCase` functions, `snake_case` locals and struct members, two-space indent.
- Comments explain **why**, not what. The existing ones cite OPC UA part and
  section numbers where behaviour is spec-driven (e.g. status-code severity
  bits, Part 4 §7.38) — keep that up when touching protocol logic.
- Prefer fixing the tool over working around it in a caller. It is a diagnostic
  instrument; if it misreports what a server holds, that is the bug.

## Output contract

These are load-bearing, and scripts depend on them:

- **stdout carries only command output**, including `--json`. Client logs go to
  stderr and show warnings and errors only. `--debug` opts into full logging;
  `--debug-stderr` and `--debug-file=PATH` redirect it.
- `--json` must emit machine-usable types: array values are real JSON arrays
  with typed elements, not strings. Never reintroduce a placeholder like the
  old `"<array>"`.
- `watch` prints exactly **one line per sample** and flushes each tick, so it
  stays useful through a pipe.
- NodeIds are rendered in canonical form (`i=2253`, `ns=2;i=1001`) everywhere,
  never with the quotes `opcua::toString` adds. Use `FormatNodeId`.
- `write` exits nonzero when the server answers with a Bad status.
- A status code that is the server's *correct* answer but reads like a tool
  failure should carry a `Hint` explaining it (see `ExplainAttributeIdInvalid`).

## Transport

`opc.tcp://` only. `open62541` 1.4 ships no WebSocket transport, so `opc.wss://`
cannot be supported without writing a second client stack. Endpoints with a
WebSocket or HTTP scheme are rejected up front with an explanatory error — see
the "Transport support" section of `README.md` before revisiting this.

## Verifying a change

There are no unit tests; **verify against a real server**. A disposable one is a
few lines against the already-installed `open62541pp`:

```cpp
opcua::ServerConfig config(4855);
opcua::Server server(std::move(config));
// add variables under opcua::ObjectId::ObjectsFolder, then:
server.run();
```

Compile it directly against the vcpkg tree the build already produced:

```sh
V=build/vcpkg-make/vcpkg_installed/arm64-osx
c++ -std=c++23 -I$V/include server.cpp -o server $V/lib/libopen62541pp.a $V/lib/libopen62541.a
```

Useful checks against any live server, since they exist on every one:
`i=2255` (NamespaceArray — an array read), `i=2254` (ServerArray), `i=85`
(Objects folder — an Object node, so `--attribute=Value` correctly fails).

macOS has **no `timeout(1)`**; do not lean on it to bound a run. `watch` has
`--duration` and `--count` for exactly this reason.

## Docs

Update `README.md` in the same change whenever a flag, a command, or an output
convention changes. It is the only user-facing documentation.
