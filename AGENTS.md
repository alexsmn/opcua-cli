# AGENTS.md

Guidance for coding agents working in this repository.

## What this is

`opcua-cli` is a C++23 OPC UA **diagnostic client** — a command-line tool for
poking at a live OPC UA server: browse the address space, read and write
attributes, watch a value change, subscribe to a node's events, dump the
address space to a UANodeSet XML.

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
  stderr and show warnings and errors only. Any one of `--debug`,
  `--debug-stderr` and `--debug-file=PATH` turns on full logging and picks
  where it goes (stdout, stderr, a file) — see the debug-flag entry below.
- `--json` must emit machine-usable types: array values are real JSON arrays
  with typed elements, not strings. Never reintroduce a placeholder like the
  old `"<array>"`.
- `watch` prints exactly **one line per sample** and flushes each tick, so it
  stays useful through a pipe. `events` does the same per event, and puts
  *everything* about the subscription itself — the confirmation line, the
  EventNotifier warning, rejected select clauses — on stderr, so stdout stays a
  clean stream of events.
- **A bad EventId must stay visible, and it has three shapes.** A field the
  server sent with no value renders `<null>` (JSON `null`), a zero-length
  ByteString `<empty>`, and an all-zero one renders as its true hex. All three
  stay distinct; never blank, normalise or pretty-print any of them away.

  The third is the one that actually occurs, and the one a "simplification"
  will drop. The real phantom event carries `EncodeEventIdByteString(0)` —
  **eight zero bytes, a well-formed ByteString of the correct length**, not an
  absent field. Anything screening only for null/empty passes it as a valid id.
  That is why the ByteString rendering must stay faithful hex rather than
  gaining a "looks empty, print nothing" shortcut: the zeros *are* the signal.
  OPC UA Part 5 §6.4.2 makes EventId mandatory on every event, so all three are
  defects worth surfacing.
- **Never deduplicate events.** `events` prints what arrives, repeats included.
  The same §6.4.2 makes EventId unique per event, so a repeat is a server
  defect, and a client-side dedupe would bury it exactly as normalising a null
  EventId would. This is not hypothetical: the SCADA aggregating proxy delivers
  ~0.6 duplicated events per monitored-item creation — same EventId, same
  Time, same Message, twice within one subscription, confined to the first four
  notifications (Seq 1–4, never 5+, while runs reach Seq 60). Confirmed on the
  wire, in ordinary PublishResponses with no Republish involved, so it is the
  server emitting twice and not open62541 redelivering. Anyone seeing repeats
  in the output is seeing the server, not a bug here.
- NodeIds are rendered in canonical form (`i=2253`, `ns=2;i=1001`) everywhere,
  never with the quotes `opcua::toString` adds. Use `FormatNodeId`.
- `write` and `browse` exit nonzero when the server answers with a Bad status.
  **An empty result must never stand in for an error.** `browse` used to return
  a bare empty vector when the Browse service answered Bad, so a refused node
  and a childless one printed identically — the exact class of misreport this
  tool exists to avoid. The status is now carried out of `BrowseNode` and
  surfaced with a hint; failed sub-browses in a `--recursive` crawl are marked
  per-node rather than rendering as leaves. Use `browseAll`, not `browse`: it
  follows continuation points, so large nodes are not silently truncated.
- Each debug flag enables full logging on its own; they select a destination,
  they do not modify `--debug`. Keying the log level off `--debug` alone made
  `--debug-stderr` a no-op (it chose the destination logs already went to).
  Note also that boolean long flags must strip exactly the two leading dashes:
  an old `substr(rfind('-') + 1)` stored `--debug-stderr` as `"stderr"`, so it
  never reached `SecurityOptions` — any hyphenated flag would break the same way.
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

To exercise `events`, give that server something to raise — drive it with
`runIterate()` in a loop instead of `run()` and trigger on a timer:

```cpp
opcua::Event event(server);
event.writeSourceName("TestSource").writeTime(opcua::DateTime::now())
     .writeSeverity(100).writeMessage({"en-US", "test event"});
event.trigger();  // origin defaults to ObjectId::Server, i.e. i=2253
```

Two things that will otherwise cost an hour:

- **You cannot make open62541 emit a null EventId.** `triggerEvent` always
  generates one and writes it over whatever you set, so the `<null>` path
  cannot be reproduced from the server side. Exercise it through `LocalTime`
  instead: it is optional on BaseEventType, so the select clause resolves but
  the instance has no value and the field arrives null. `<empty>` is reachable
  through a plain ByteString variable holding a zero-length value. The
  rendering is field-name agnostic, so both prove the EventId path.
- **Do not raise `outStandingPublishRequests` back to open62541's default of
  ten.** Closing the session deletes subscriptions, so every queued publish
  request returns BadNoSubscription and the client logs a warning for each —
  ten lines of noise on the stderr channel `events` uses for real problems.
  One request in flight loses nothing: the server queues notifications and
  answers a whole burst in one response.

Useful checks against any live server, since they exist on every one:
`i=2255` (NamespaceArray — an array read), `i=2254` (ServerArray), `i=85`
(Objects folder — an Object node, so `--attribute=Value` correctly fails, and
a non-notifier, so `events` on it correctly fails too).

Servers disagree about the EventNotifier attribute, so the pre-flight check in
`CheckEventNotifier` **warns and continues rather than failing**. The SCADA
aggregating proxy answers `BadAttributeIdInvalid` for EventNotifier on `i=2253`
and then delivers events perfectly well; hard-failing on that reading would
block a working subscription. The authoritative answer is the server's
CreateMonitoredItems status.

macOS has **no `timeout(1)`**; do not lean on it to bound a run. `watch` and
`events` have `--duration` and `--count` for exactly this reason.

### Telling a tool bug from a server bug

When output looks wrong, the question is whether these bytes arrived or whether
we invented them — and the answer is cheap. Against a tunnelled server the
tunnel's local end is **plaintext OPC UA**, so a small logging TCP relay
between `opcua-cli` and the tunnel records exactly what the server sent. No
root, no `tcpdump`, nothing installed on the far host, and it beats a capture
at the server: that one carries every other client's traffic to the same nodes,
while the loopback stream is only ours. Run each invocation as its own TCP
connection so run N pairs with capture N, then search the stream for the raw
bytes in question (an EventId is its 8 bytes, `bytes.fromhex`).

Two things that decide whether the answer is worth anything:

- **Carry a control group through the same measurement.** Comparing "suspect"
  counts against "known-good" counts *from the same capture* cancels out
  whatever else is on the link. Proving a duplicated EventId appears twice
  means nothing until singly-delivered ones are shown to appear exactly once.
- **A control that reads impossibly is the finding, not a nuisance.** The first
  run of this said "client-side duplication" from a comparison of `0 == 0`: a
  `nc -z` readiness probe had opened a zero-byte connection the relay numbered
  first, offsetting every run against its capture by one, so every search
  missed. Events we had certainly received cannot occur zero times in the
  stream that delivered them. Had the offset hit only the suspect group, the
  control would have looked fine and the wrong answer would have shipped.

Client-side observation stops at "the proxy's socket emitted it twice"; it
cannot say *which* hop upstream doubled it. That needs server-side tracing, and
against the GCP demo the tracing is **already live** — do not go building a
capture rig before checking it:

- The test target is the **GCP** demo VM (`scada-demo`, `us-west1-b`), reached
  over an IAP tunnel. Not a local cluster. So telemetry goes to the managed
  Google Cloud backends, not to anything self-hosted.
- All seven tiers carry a `metrics` block with `traces.enabled = true` and
  `sampling_ratio = 1.0`, exporting OTLP to `host.docker.internal:4317`. The
  receiver is the Ops Agent's `otelopscol` on the VM host, forwarding to Cloud
  Trace; spans are confirmed arriving. Container logs already go to Cloud
  Logging via the `gcplogs` driver, which is what the `gcloud logging read`
  queries throughout this work were reading.
- The only gap for a which-hop question is **a span on notification emission**.
  Cross-tier W3C `traceparent` propagation already works (gRPC `trace_id`
  northbound, an OPC UA `RequestHeader.AdditionalHeader` entry between tiers),
  so one span would join the existing waterfall. One change, not a project.

Two traps that made this look absent when it is not, both worth avoiding next
time: the exporter config lives in each tier's `configs/*.json`, **not** in
container env vars, so `docker inspect | grep -i otel` finds nothing; and the
collector is a **host-level agent**, not a container, so `docker ps | grep
collector` finds nothing either. Check `ss -lntp | grep 4317` and the tier
config. Details are documented on the SCADA side in `docs/ops/deployment.md`.

## Docs

Update `README.md` in the same change whenever a flag, a command, or an output
convention changes. It is the only user-facing documentation.
