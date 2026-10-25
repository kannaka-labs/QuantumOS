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

## CLIs without an MCP server (`cli_connector.py`)

A server entry can be `{"cli": {...}}` instead of a command: a CLI whose **allowlisted argv
templates** become hub tools, under the same allow/ask/deny policy.

```json
"scada": {"cli": {"binary": ["node", "~/Source/0xSCADA/cli/dist/index.js"], "timeout_s": 60, "tools": {
  "alarms_by_severity": {"description": "List alarms of one severity.",
                         "argv": ["alarms", "list", "--severity", "{severity}", "--json"],
                         "params": {"severity": {"type": "string",
                                                 "enum": ["critical", "high", "medium", "low", "info"]}}}}}}
```

- **Only listed commands exist.** The connector never probes a CLI with `--help` (on a host
  with a live Kannaka store that is not harmless).
- **A placeholder fills a whole argv slot only:** no shell, never inside a larger string.
- **Every value is checked:** string (`max_len`, `enum`, a whole-value `pattern`), integer
  (`min`/`max`) or boolean. A string may not start with `-`, so it can never become an option.
- **Bounds:** unknown parameters are refused, and each run has a timeout and a 64 KiB output cap.
- **Options:** `binary` may be a command prefix (`["node", ".../index.js"]`), and `cwd` sets
  the working directory.

The example config wires up:
- `kcli`: the `kannaka` CLI, with status, recall and swarm status;
- `scada`: 0xscada status, assets and alarms are allowed; `alarm_ack` asks, because it changes
  state at a site;
- `steward`: `govern` and `trade`, limited to the offline warrant fixtures, never signing or
  submitting.

## MCP servers over HTTP

```json
"city": {"url": "https://kax.ninja-portal.com/api/mcp",
         "auth": {"bearer_file": "~/.kannaka/kax-token-kannaka.txt"}}
```

- **Transport:** streamable HTTP. Plain JSON-RPC replies, as Agent-kax sends, work too.
- **The token:** read at start from `bearer_file`, or from the variable named by `bearer_env`,
  and sent as `Authorization: Bearer …`. It is never logged and never stored in the config.
- **Failures:** a server that refuses to connect is logged and skipped. A server that dies
  later answers with an error result. Each server runs in its own task, so neither takes the
  hub down.
- **The example config** adds KAX City (Agent-kax). Looking around and the Joinery catalog are
  allowed; entering, speaking, moving and trading ask.

**Known gap:** KAX identity tokens live 15 minutes, and the hub reads its token once, at
start. A refresh hook (`POST /auth/token/refresh` on a timer, or a `bearer_cmd`) is next in
#297, along with the Command Center (OAuth) and PFORK_MCP.

## Trust boundary

The policy holds for an agent that reaches tools only through the hub. Downstream servers run
with the hub's own permissions and credentials, so run the hub as a user whose credentials
you are happy for allowed tools to use.

## Tests

`python3 -m pytest -q tools/qos-hub`: 30 tests (8 hub, 19 CLI connector, 3 HTTP). They cover the policy matching, allow, deny
never reaching the server, ask holding the exact call and releasing it only with the key and
only once, a tampered or expired approval refused, unknown tools asking, a broken downstream
server not hiding the others, the approval CLI, and a full run over real MCP stdio with a
fake downstream server.
