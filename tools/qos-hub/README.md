# QuantumOS hub (#297, ADR-0027)

Installed: `pip install quantumos-host-tools` (add `[oci]` for the OCI plugin) gives the `qos-broker`, `qos-broker-mcp` and `qos-hub` commands; from a checkout, run the scripts in this directory directly.

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

Or install it as a Claude Code plugin from the constellation marketplace, which registers the
same server from the manifests at the repository root (`.claude-plugin/plugin.json`, `.mcp.json`):

```
pip install quantumos-host-tools                       # qos-hub on PATH
mkdir -p ~/.qos-hub && cp hub.example.json ~/.qos-hub/hub.json   # fix the paths; adjust the policies
qos-broker envelope --db ~/.qos-hub/broker.db --per-day 2 --per-action 1 --classes storage,compute,qpu --max-lease 24h
claude plugin marketplace add kannaka-labs/kannaka-constellation-marketplace
claude plugin install quantumos@kannaka-constellation
```

The plugin reads `~/.qos-hub/hub.json` and `~/.qos-hub/broker.db`; nothing in it carries a key.

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
| `cannon` | [kannaka-cannon](https://github.com/kannaka-labs/kannaka-cannon) `clipcannon` (51 tools, stdio) | project list/status, config and credits reads, disk status, transcript / scene map / narrative flow / editing context, search, edit history, branches, voice profiles, HRM recall | everything else (ingest, edits, renders, previews, and the generate / speak / compose tools that call paid providers) | project_delete, disk_cleanup |

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
  submitting;
- `hdl`: [kannaka-hdl](https://github.com/kannaka-labs/kannaka-hdl) `check` and `grow` (JSON plan,
  unresolved components stubbed) over the repository's `examples/*.khdl`, both reads;
- `apps`: the [kannaka-apps](https://github.com/kannaka-labs/kannaka-apps) store: `list` and
  `plan` (`--dry-run`, executes nothing) are allowed; `run` asks, because a provision, schedule or
  work app is a one-way operation on live memory;
- `buzz`: the Hive's [`buzz` CLI](https://github.com/kannaka-labs/kannaka-buzz) (`cargo install
  --path crates/buzz-cli` in the fork): channels, messages and search are allowed; `send` asks,
  because other members read it. The identity (`BUZZ_PRIVATE_KEY`) comes from the hub process's
  environment, never from this file;
- `lens`: [kannaka-lens](https://github.com/kannaka-labs/kannaka-lens) `show --json` of a project
  record is allowed; `run` asks, because it spends minutes of CPU on a clip and writes what it saw
  into the HRM;
- `crystal`: [kannaka-crystal](https://github.com/kannaka-labs/kannaka-crystal) `materials` and the
  `primitives` registry reads (by class and persistence floor, or one `CRY-######`), all allowed;
  evolving, promoting, pruning and publishing are not listed;
- `dna`: [kannaka-dna](https://github.com/kannaka-labs/kannaka-dna) `verify`, `stats` and `lineage`
  over the committed genomes are allowed (stdlib Python, reads only); `express` asks, because it
  writes a birth kit. Cloning, mating and `--hive` identity minting are not listed.

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

**Short-lived tokens.** KAX identity tokens last 15 minutes. Add `"refresh_url"` and
`"refresh_every_s"` to `auth`:
- the hub POSTs the current token to that URL on a timer and uses the returned token from the
  next request on, on the same open session (the token is attached per request);
- it writes the new token back to `bearer_file` (0600);
- a failed refresh is logged without the token, and the current token is kept.

KAX refuses to refresh an expired token, so the hub has to start with a fresh one.

**The Command Center** ([nats.ninja-portal.com](https://nats.ninja-portal.com), 25 tools over the
NATS bus and the GhostSignals markets) authenticates headless agents with the OAuth 2.1
`client_credentials` grant rather than a token file:

```json
"cc": {"url": "https://nats.ninja-portal.com/mcp",
       "auth": {"token_url": "https://nats.ninja-portal.com/api/public/auth/agent/token",
                "client_id": "…", "client_secret_file": "~/.kannaka/command-center-client-secret.txt",
                "scope": "mcp:read", "refresh_every_s": 600}}
```

- Create the client on the Command Center's Integrations page with the scopes you want; the
  secret is shown once. Put it alone in a 0600 file (or name a variable with
  `client_secret_env`). The client id is public and may sit in the config.
- The hub posts the grant at start and every `refresh_every_s` (tokens last 900 s), and uses
  the new `access_token` from the next request on. A refused grant is logged as the server's
  error code (`401 invalid_client`), never with the secret.
- The example policy allows the reads (bus, presence, dreams, Nostr and Hive traffic, recall,
  market reads, compute roster) and asks for `ask_kannaka` (needs `mcp:recall`) and for
  everything metered: `place_bet`, `propose_prediction`, `buy_credits`, `redeem_credits`,
  `wake_machine`.

PFORK_MCP has no live endpoint (measured 2026-10-30, see #297); it would also need an
`x-api-key` header option here. Still open in #297: `KAX.qos.*` over NATS.

## Trust boundary

The policy holds for an agent that reaches tools only through the hub. Downstream servers run
with the hub's own permissions and credentials, so run the hub as a user whose credentials
you are happy for allowed tools to use.

## Tests

`python3 -m pytest -q tools/qos-hub`: 43 tests (8 hub, 19 CLI connector, 6 HTTP, 10 installed defaults). They cover the policy matching, allow, deny
never reaching the server, ask holding the exact call and releasing it only with the key and
only once, a tampered or expired approval refused, unknown tools asking, a broken downstream
server not hiding the others, the approval CLI, and a full run over real MCP stdio with a
fake downstream server.
