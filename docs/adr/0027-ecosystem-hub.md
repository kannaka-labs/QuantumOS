# 27. One Policy-Gated MCP Endpoint for the Ecosystem

Date: 2026-10-24
Status: Accepted (2026-10-25, Nick; shipped in v0.6.0)

## Context

Nick (2026-10-24): adapt or port the Kannaka CLI, the 0xSCADA CLI, and any other ecosystem
CLI, MCP server or communication channel that helps users, so that QuantumOS is where a
person points their AI.

An inventory of `~/Source` found the ecosystem already has several MCP servers and more CLIs:
- **MCP over stdio:** Kannaka (the plugin), kannaka-quantum and Kannaktopus.
- **MCP over HTTP:** Agent-kax (KAX City), the Command Center (the NATS connector, metered) and
  PFORK_MCP.
- **CLIs without MCP:** 0xscada, kannaka (30+ subcommands), kannaka-quantum lab-\*, steward.
- **Comms:** NATS (`KANNAKA.*`, `KAX.*`, `KAX.qos.*`, `scada.*`), several HTTP APIs, and the
  Nostr and hive relays.

Each has its own credentials, and some publish to a shared public bus or spend money. A
person who wires all of them into their AI today gives the AI every one of those powers,
unconditionally.

ADR-0026 gave paid resources a broker with budgets, approvals and a ledger. Tool calls need the
same discipline, but most are not leased resources: they are single calls.

## Decision

1. **A hub: one MCP server in front of the others** (`tools/qos-hub`). It starts the downstream
   servers named in its config, re-exports their tools under a namespace, and decides each call
   by a per-tool policy: `allow`, `ask` or `deny` (fnmatch patterns; the longest match wins). An
   unlisted tool falls to `default`, which is `ask`.
2. **`ask` stores the call, not just a yes/no.** The hub records the exact tool and arguments,
   with a digest, and returns an approval id. The person approves with the approver key, and the
   agent runs the stored call with `hub_call_approved`. An approval is used once, expires after
   an hour, and cannot be applied to other arguments.
3. **One ledger, one key.** Hub approvals and events live in the provider broker's database and
   use its approver key (ADR-0026), so the person has one place to see and approve everything.
4. **Adapt, don't port, for now.** Tools keep running where they run today and are reached
   through their existing MCP servers. Running Kannaka *inside* the guest remains #263's longer
   path (a Rust `no_std` toolchain first).
5. **Configuration, not code, is how it grows.** Adding an MCP server is a config entry. Next come
   HTTP transport (for Agent-kax and the Command Center) and a CLI connector, which exposes
   allowlisted command templates of a CLI with no MCP server (0xscada, kannaka, steward) as tools.

## Consequences

- Least privilege becomes the default for an AI: reads flow, while shared-bus writes, spending and
  store changes wait for the person.
- The hub is a single point of failure for the agent's tools. A downstream server that fails to
  start or list is logged and skipped, not fatal.
- Approval adds a round trip through the person. For tools they trust, they set `allow`.
- Downstream servers run with the hub's credentials. The policy only holds for an agent that has
  no other path to those credentials (the same trust boundary as ADR-0026).
- Results are passed through as text, at most 64 KiB, and tools' structured output schemas are
  not re-exported yet.

## Evidence

- `tools/qos-hub/qos_hub.py` and 8 tests (`test_qos_hub.py`), including a full run over MCP stdio
  with a fake downstream server (denied tools never reach it).
- A real run on the desktop, with the hub in front of the Kannaka plugin's MCP server (1.4.7,
  12 tools):
  - status and recall were allowed and returned live data;
  - `swarm_send` was held as `approval_required` and then **denied**, so nothing was published;
  - `compute_grant` was denied and not listed;
  - the ledger recorded each decision.
