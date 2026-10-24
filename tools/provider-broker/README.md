# QuantumOS provider broker (ADR-0026)

The broker lets an agent get real resources (buckets, VMs, GPUs, QPU jobs) from
providers such as OCI and qBraid, inside a budget the agent cannot raise. This is the
core (#284): the lifecycle, the money rules and the ledger, plus a mock provider. The
OCI (#285) and qBraid (#286) plugins and the MCP tools (#287) build on it.

```
agent (Claude over MCP, #287)
  │  quote → provision → status → teardown, budget, ledger     (Unix socket, JSON lines)
  ▼
provider_broker.py serve
  │  envelope check · approval · lease written first · tagged · reaper · ledger
  ▼
provider plugin (mock today; OCI, qBraid next)  →  the provider's API, with the person's own credentials
```

## The rules

- **A quote comes first.** Every quote gives the unit price, the most the whole lease can
  cost, and whether that figure is exact or an estimate. A quote is valid for 10 minutes
  and can be used once.
- **The person sets the envelope**, and the agent works inside it:
  - USD per UTC day and per action;
  - the resource classes allowed (`storage`, `compute`, `gpu`, `qpu`, `service`);
  - the longest lease.
- **Outside the envelope, the person approves.** `provision` answers `approval_required`
  with an approval id. The person runs `provider_broker.py approve <id>` and types the
  approver key, then the agent retries with the id. An approval is bound to its quote and
  used once. Nothing on the socket can approve or change the envelope.
- **The approver key is shown once**, when the envelope is first set, and stored as an
  scrypt hash. Changing the envelope later needs that key.
- **Real spending is off by default.** Without `--allow-spend`, only quotes whose maximum
  cost is exactly 0 can be provisioned.
- **Every resource has a lease.**
  - The lease is written to the ledger *before* the provider is called.
  - The resource is tagged `qos-<broker id>-<lease id>` at the provider.
  - Expired leases are torn down by the reaper.
  - A crash mid-provision is cleaned up by tag at the next start.
  - Teardown and cleanup only ever touch resources carrying this broker's tag for a lease
    in its own ledger, never anything else in the account.
- **The ledger** records every envelope change, quote, approval request and decision,
  provision and end, with the estimate and the billed cost (when the provider reports one)
  side by side. `ledger` shows it to the agent and the person alike.

## Trust boundary

These rules hold against an agent that reaches the broker only through its socket. An
agent with a shell as the user that owns the ledger and the provider credentials can
bypass them. Run the broker as its own user, and put the agent's user in the socket's
group only.

## Try it with the mock provider

```
python3 provider_broker.py envelope --db b.db --per-day 1 --per-action 0.5 --classes storage,compute --max-lease 24h
python3 provider_broker.py serve --db b.db --socket b.sock            # add --allow-spend to permit paid quotes
printf '%s\n' '{"op":"quote","provider":"mock","kind":"bucket","spec":{},"duration_s":3600}' | nc -U b.sock
```

## Point Claude at it (MCP, #287)

`broker_mcp.py` is a small MCP server with seven tools: `broker_providers`, `broker_quote`,
`broker_provision`, `broker_status`, `broker_teardown`, `broker_budget` and `broker_ledger`.
None of them can approve a request or change the budget. When a provision needs
approval, the result carries `tell_your_human`: the exact command for the person to run,
and an instruction to the agent to wait and retry with the approval id.

```
# the person, once
python3 provider_broker.py envelope --db b.db --per-day 2 --per-action 1 --classes storage,qpu --max-lease 24h
python3 provider_broker.py serve --db b.db --socket b.sock --providers oci,qbraid     --oci-config ~/.oci/broker.conf --oci-compartment ocid1.compartment.oc1..xxxx --allow-spend

# Claude Code
claude mcp add quantumos-broker --env QOS_BROKER_SOCKET=$PWD/b.sock -- python3 $PWD/broker_mcp.py
```

Then ask: "make me a 1 GB bucket for a day". Claude quotes it, provisions it if it fits the
envelope (or tells you the `approve` command to run), and `broker_teardown` removes it.
It is a separate server from `scripts/qos_mcp.py`, so the frozen v1 tool surface
(ADR-0020) is unchanged; `test_broker_mcp.py` pins this server's own tool list.

## Writing a provider plugin

A plugin is an object with:
- `name`;
- `offerings()`;
- `quote(kind, spec, duration_s) -> Quote`;
- `provision(kind, spec, tag) -> resource id`, which must attach `tag` at the provider;
- `status(rid)`;
- `teardown(rid)`, which must be idempotent;
- `billed(rid) -> float | None`;
- `find(tag) -> [rid]`.

Raise `Refused(code, message)` for a request the provider will not take. `plugin_mock.py`
is the reference.

## Tests

`python3 -m pytest -q tools/provider-broker` (the core's 13 tests use the mock provider; the OCI, qBraid and MCP tests are in their own files). They
cover the full lifecycle; spending off; approval with and without the key; daily,
per-action and lease-length caps; no envelope; envelope changes needing the key; quote
expiry; the reaper; failed and half-done provisions; crash recovery; never touching
foreign resources; the socket refusing human-only ops; and the CLI showing the key once.
