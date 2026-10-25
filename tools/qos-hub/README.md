# QuantumOS hub (#297, ADR-0027)

One MCP endpoint for the whole ecosystem. Instead of configuring your AI with Kannaka's
MCP server, kannaka-quantum's, Kannaktopus's and the provider broker's one by one, you point
it at the hub. The hub starts them, re-exports their tools under a namespace
(`kannaka.kannaka_recall`, `quantum.run_circuit`, `broker.broker_quote`), and puts a
**policy** on every tool:

| policy | what happens |
|---|---|
| `allow` | the call runs at once |
| `ask` | the call does **not** run. The hub stores it exactly (tool + arguments) and returns an approval id. You approve it with your approver key; the agent then runs it with `hub_call_approved`. An approval is used once, expires after an hour, and cannot be applied to different arguments. |
| `deny` | refused, and not even listed to the agent |

A tool your config doesn't mention gets the `default` policy, which is `ask` unless you
change it, so a new tool appearing downstream is never silently callable. Every call, its
decision and its outcome are written to the ledger.

The ledger and the approver key are the provider broker's (`tools/provider-broker`):
one database and one key for approving both tool calls and paid resources.

## Use it

```
cp hub.example.json hub.json          # fix the paths; adjust the policies
python3 ../provider-broker/provider_broker.py envelope --db broker.db --per-day 2 --per-action 1 \
        --classes storage,compute,qpu --max-lease 24h     # once: shows your approver key
claude mcp add quantumos -- python3 $PWD/qos_hub.py serve --config $PWD/hub.json --db $PWD/broker.db
```

When the agent hits an `ask` tool it gets `tell_your_human`. You then:

```
python3 qos_hub.py pending --db broker.db         # what is waiting, with its exact arguments
python3 qos_hub.py approve --db broker.db h1a2b3c4d5      # shows the call, asks for your key
```

The hub's own tools are `hub_tools` (every tool and its policy), `hub_call_approved`
and `hub_ledger`. There is no tool to approve or to change the policy.

## What the example config wires up

| server | from | allowed | asks first | denied |
|---|---|---|---|---|
| `kannaka` | the Kannaka plugin's `kannaka-mcp.mjs` | status, recall, swarm status/tail, KAX compute roster/status/events | remember, dream, **swarm_send** (public bus), **compute_wake** (spends KAX credits) | compute_grant |
| `quantum` | kannaka-quantum (`python -m kannaka_quantum mcp`) | devices, quantum_random, resonance_recall | run_circuit | |
| `octo` | Kannaktopus `mcp-server` | status, skills, Kannaka/HRM reads, swarm status/tail | swarm_send, absorb, dream, the octopus workflows (they call paid LLMs) | |
| `broker` | the provider broker's `broker_mcp.py` | all (the broker enforces its own envelope and approvals) | | |

Next (#297): HTTP MCP servers (Agent-kax's KAX City tools, the Command Center), and a CLI
connector that exposes allowlisted commands of CLIs without an MCP server (0xscada, kannaka,
steward) as tools.

## Trust boundary

The policy holds for an agent that reaches tools only through the hub. Downstream servers run
with the hub's own permissions and credentials, so run the hub as a user whose credentials
you are happy for allowed tools to use.

## Tests

`python3 -m pytest -q tools/qos-hub`: 8 tests. They cover the policy matching, allow, deny
never reaching the server, ask holding the exact call and releasing it only with the key and
only once, a tampered or expired approval refused, unknown tools asking, a broken downstream
server not hiding the others, the approval CLI, and a full run over real MCP stdio with a
fake downstream server.
