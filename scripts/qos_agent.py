#!/usr/bin/env python3
"""Bring-your-own-model ↔ QuantumOS: a BYO-key agent that drives a live
QuantumOS VM through its FROZEN MCP tool surface (ADR-0020 lane B, contracts/
mcp/v1-tools.json). An external model is exactly the consumer that freeze was
built for — so this script REUSES scripts/qos_mcp.py verbatim rather than
duplicating a single tool definition. One source of truth: the same 22 tools
CI diffs against the golden are the tools your model sees.

Any model, your choice of provider:

    --provider anthropic   (default) Claude, via the Anthropic SDK's
                           tool_runner + MCP conversion helpers
    --provider openai      ANY OpenAI-compatible /chat/completions endpoint:
                           OpenAI itself, OpenRouter, Groq, DeepSeek, Mistral,
                           LM Studio, llama.cpp server — or a keyless local
                           Ollama (--base-url http://localhost:11434/v1)

Architecture (all local, no hosted anything):

    your model (your key — or no key at all for a local server)
        │  provider-specific agentic loop
        ▼
    MCP tool conversion (anthropic helpers / plain function-calling)
        │  stdio
        ▼
    scripts/qos_mcp.py  (FastMCP, unmodified)
        │  thin delegation
        ▼
    qos_bridge.QosVM / QosSociety  →  QEMU  →  QuantumOS

The key never leaves your machine; the VM never leaves your machine. The model
plans and runs field-society / associative-memory experiments over the wire,
reads the results, and iterates.

HONESTY (mirrors qos_mcp.py's module docstring): a tool result's `verified`
means the boot attestation is self-consistent (frames + Lamport signature +
qseed), NOT that the VM is live-now — a captured attestation replays. Only the
key-admitted STATUS reply is per-request nonce+HMAC fresh. The system prompt
tells the model this so it never overstates what a result proves.

Run:
    # 1. build the guest once (produces build/x86_64/kernel.elf32)
    make kernel
    # 2. install deps
    pip install -r requirements-agent.txt

    # Claude (default) — bring Anthropic credentials, either works:
    export ANTHROPIC_API_KEY=sk-ant-...        # or: ant auth login
    python3 scripts/qos_agent.py "Boot a VM and imprint three phrases, then recall a corrupted probe of the first."

    # any OpenAI-compatible provider:
    export OPENAI_API_KEY=...
    python3 scripts/qos_agent.py --provider openai --model gpt-5.1 --experiment society

    # a local model, no key at all:
    python3 scripts/qos_agent.py --provider openai --model llama3.3 \
        --base-url http://localhost:11434/v1

    # prove the MCP handshake without spending a token / needing a key:
    python3 scripts/qos_agent.py --list-tools

Deps: requirements-agent.txt (anthropic[mcp], openai, mcp) — each provider SDK
is imported only when that provider is selected. Python >= 3.10.
"""

import argparse
import asyncio
import json
import os
import re
import shlex
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

PROVIDERS = ("anthropic", "openai")

# Curated on-ramp experiments (--experiment NAME). Each is a goal, not a
# recipe: the model designs the actual tool sequence. "recall" is the default.
EXPERIMENTS = {
    "recall": (
        "Design and run a small associative-memory experiment: boot a VM, "
        "imprint three distinct short phrases into the kernel Hopfield-Kuramoto "
        "field, then recall a corrupted probe of one of them and report whether "
        "the field relaxed to the right basin and with what order parameter R. "
        "Draw one quantum-seeded number to salt the run. Shut the VM down when "
        "finished and give a plain-language readout of what the field did."
    ),
    "society": (
        "Boot a 3-node field society, wait for the members to mean-field "
        "synchronize, then read each node's status and the aggregates it "
        "received from its peers. Report whether all three synchronized (and to "
        "what R_x), and whether each node saw the OTHER two nodes' distinct "
        "qseed-salted aggregates. Shut the society down when finished."
    ),
    "quantum": (
        "Draw several quantum-seeded random numbers from the guest and submit a "
        "small quantum circuit (e.g. a Bell pair) to the in-OS QPU broker. "
        "Report the results and explain, honestly, what is and isn't attested "
        "about them. Shut the VM down when finished."
    ),
    "explore": (
        "You have a live QuantumOS instance. Explore it: read a machine "
        "snapshot, inspect the holographic field read-only, look at the "
        "capability authority ledger, and form your own small hypothesis about "
        "how the field behaves — then run one experiment to test it. Shut down "
        "cleanly and tell me what you found."
    ),
}
DEFAULT_TASK = EXPERIMENTS["recall"]

# Idempotent power-off tools called on exit so a booted VM/society is never
# orphaned, however the run ends (the system prompt asks the model to shut
# down; this guarantees it even on error, Ctrl-C, or the turn cap).
CLEANUP_TOOLS = ("qos_shutdown", "qos_society_shutdown")

SYSTEM = """You are an experimentalist driving a live QuantumOS instance — a \
capability-secured microkernel whose kernel hosts a Hopfield–Kuramoto \
associative-memory field ("ghostd"), quantum-seeded entropy, and multi-node \
"field societies" that couple over the wire. You act only through the provided \
QuantumOS tools; there is no shell.

What you can do: boot and shut down VMs; imprint and recall associative \
memories at the kernel field and read that field's live state; form N-node \
societies (3–4 members) that mean-field-synchronize and exchange qseed-salted \
aggregates; draw quantum-seeded entropy; run citizens off the initrd; read \
machine snapshots; and bridge memories to/from the host Kannaka HRM.

Method: state your hypothesis, run the smallest experiment that tests it, read \
the actual tool results (do not assume — a boot can fail, a recall can miss), \
and iterate. Prefer one clear result over an exhaustive sweep.

Honesty about trust — this matters: a result's `verified` flag means the boot \
attestation is internally consistent (frames + Lamport signature + qseed), NOT \
that the VM is live at read time; a captured attestation replays. Only a \
key-admitted STATUS reply is per-request authenticated. Never describe an \
ordinary tool result as cryptographically attested data. Report what the field \
did, plainly, and distinguish measured from inferred.

Operational: booting a VM or a society takes tens of seconds to a few minutes \
— that is normal, not a hang. Always shut down what you booted before ending, \
even on failure. When done, give a plain-language readout: what you ran, what \
the field did, and what it demonstrates."""


async def _open_tools(mcp_client_ctx):
    """List the QuantumOS MCP tools from an initialized ClientSession."""
    result = await mcp_client_ctx.list_tools()
    return result.tools


# Both the Anthropic and OpenAI APIs only accept tool names matching this.
_WIRE_NAME_OK = re.compile(r"^[A-Za-z0-9_-]{1,64}$")


def _wire_tools(mcp_tools):
    """Rename tools whose names the model APIs reject. Behind the hub
    (`--server-cmd "qos-hub serve ..."`) tools are namespaced, e.g.
    `qos.qos_boot`, and the dot is not a legal tool name; it becomes
    `qos__qos_boot`. Returns (tools as the model sees them, wire -> MCP name)."""
    tools, mapping = [], {}
    for t in mcp_tools:
        wire = t.name if _WIRE_NAME_OK.match(t.name) else re.sub(r"[^A-Za-z0-9_-]", "__", t.name)[:64]
        mapping[wire] = t.name
        tools.append(t if wire == t.name else t.model_copy(update={"name": wire}))
    return tools, mapping


class _WireSession:
    """A ClientSession that maps wire tool names back to MCP names on call_tool,
    so both drivers stay unaware of the renaming."""

    def __init__(self, session, mapping):
        self._session = session
        self._mapping = mapping

    async def call_tool(self, name, arguments=None, *a, **kw):
        return await self._session.call_tool(self._mapping.get(name, name), arguments, *a, **kw)

    def __getattr__(self, attr):
        return getattr(self._session, attr)


def _kernel_hint():
    """Warn early if no built guest is discoverable — every tool that boots a
    VM would otherwise fail deep inside a tool call with a confusing error."""
    env_kernel = os.environ.get("QOS_KERNEL")
    default = os.path.join(REPO, "build", "x86_64", "kernel.elf32")
    if env_kernel and os.path.exists(env_kernel):
        return None
    if os.path.exists(default):
        return None
    return (
        "No QuantumOS kernel found (looked at $QOS_KERNEL and "
        f"{default}). Build one first with `make kernel`, or point QOS_KERNEL "
        "at a kernel.elf32."
    )


def _server_params(server_cmd=None):
    """stdio parameters that launch scripts/qos_mcp.py as an MCP server.

    Running `python qos_mcp.py` puts scripts/ on sys.path[0], so its bare
    `from qos_bridge import ...` resolves; cwd is the repo root so any
    relative kernel path resolves the same way the CI gates see it. QOS_KERNEL
    (and the rest of the environment) is inherited by the child, so the VM the
    agent boots is the one you built.

    `server_cmd` (a command line) launches a different MCP server instead,
    e.g. the hub: `qos-hub serve --config hub.json --db broker.db`."""
    from mcp import StdioServerParameters

    if server_cmd:
        argv = shlex.split(server_cmd)
        return StdioServerParameters(command=argv[0], args=argv[1:], env=os.environ.copy())
    return StdioServerParameters(
        command=sys.executable,
        args=[os.path.join(HERE, "qos_mcp.py")],
        cwd=REPO,
        env=os.environ.copy(),
    )


async def list_tools_only(server_cmd=None):
    """Prove the model→MCP→qos_bridge plumbing end to end WITHOUT calling any
    API or needing a key: spawn the server, initialize, list the tools."""
    from mcp import ClientSession
    from mcp.client.stdio import stdio_client

    async with stdio_client(_server_params(server_cmd)) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            tools = await _open_tools(session)
            print(f"QuantumOS MCP server exposes {len(tools)} tools:")
            for t in sorted(tools, key=lambda x: x.name):
                summary = (t.description or "").strip().splitlines()[0] if t.description else ""
                print(f"  {t.name:<26} {summary[:90]}")
    return 0


async def _cleanup(session):
    """Power off anything the agent may have booted. Best-effort and idempotent:
    both tools no-op when nothing is running, so calling both is always safe.
    Behind the hub they are namespaced (`qos.qos_shutdown`), so match by suffix."""
    try:
        names = [t.name for t in await _open_tools(session)
                 if t.name.rsplit(".", 1)[-1] in CLEANUP_TOOLS]
    except Exception:  # noqa: BLE001
        names = list(CLEANUP_TOOLS)
    for name in names:
        try:
            await session.call_tool(name, {})
        except Exception:  # noqa: BLE001 — cleanup must never mask the real outcome
            pass


def _fmt_input(obj):
    """Compact one-line rendering of a tool_use input for the trace."""
    if not obj:
        return ""
    return ", ".join(f"{k}={v!r}" for k, v in obj.items())


# --------------------------------------------------------------------------
# provider: anthropic (Claude — the default)
# --------------------------------------------------------------------------

def _prepare_anthropic():
    """Import the SDK and resolve credentials BEFORE any VM is spawned.
    Returns a client, or an int exit code on failure."""
    try:
        from anthropic import AsyncAnthropic
        from anthropic.lib.tools.mcp import async_mcp_tool  # noqa: F401 — fail early if the MCP extra is missing
    except ImportError:
        print(
            "The `anthropic` SDK with MCP extras is required for "
            "--provider anthropic:\n  pip install -r requirements-agent.txt",
            file=sys.stderr,
        )
        return 2
    try:
        return AsyncAnthropic()  # resolves ANTHROPIC_API_KEY or an `ant auth login` profile
    except Exception as exc:  # noqa: BLE001 — surface any credential-resolution failure clearly
        print(
            f"Could not construct the Anthropic client ({exc}).\n"
            "Bring your own key: `export ANTHROPIC_API_KEY=sk-ant-...` or "
            "`ant auth login`.",
            file=sys.stderr,
        )
        return 2


# Sticker $/MTok (input, output) for the spend estimate printed after each run.
# Cache reads bill ~0.1x input; cache writes ~1.25x. Unknown models fall back
# to the Opus row so the estimate errs high rather than silent.
_PRICES = {
    "claude-fable-5": (10.0, 50.0),
    "claude-opus": (5.0, 25.0),
    "claude-sonnet": (3.0, 15.0),
    "claude-haiku": (1.0, 5.0),
}


def _price_for(model):
    for prefix, rates in _PRICES.items():
        if model.startswith(prefix):
            return rates
    return _PRICES["claude-opus"]


def _report_spend(model, totals):
    in_rate, out_rate = _price_for(model)
    est = (
        totals["input"] * in_rate
        + totals["cache_read"] * in_rate * 0.1
        + totals["cache_write"] * in_rate * 1.25
        + totals["output"] * out_rate
    ) / 1_000_000
    print(
        f"\n[usage] {model}: {totals['input']:,} in / {totals['output']:,} out / "
        f"{totals['cache_read']:,} cache-read / {totals['cache_write']:,} cache-write "
        f"tokens — est. ${est:.2f}",
        file=sys.stderr,
    )


async def _drive_anthropic(client, session, mcp_tools, task, model, max_tokens, effort, max_turns,
                           token_budget):
    """The Claude loop: the SDK's MCP conversion helpers + tool_runner drive
    the agentic loop, streaming the trace to stdout."""
    from anthropic.lib.tools.mcp import async_mcp_tool

    tools = [async_mcp_tool(t, session) for t in mcp_tools]
    runner = client.beta.messages.tool_runner(
        model=model,
        max_tokens=max_tokens,
        # Adaptive thinking with a visible summary so the reasoning between
        # tool calls isn't a silent pause; effort is the depth knob (high
        # suits multi-step experiment design).
        thinking={"type": "adaptive", "display": "summarized"},
        output_config={"effort": effort},
        system=SYSTEM,
        messages=[{"role": "user", "content": task}],
        tools=tools,
    )

    # Each yielded message is one assistant turn (before its tools run).
    # The MCP tools execute long VM operations; the runner awaits them.
    turns = 0
    totals = {"input": 0, "output": 0, "cache_read": 0, "cache_write": 0}
    async for message in runner:
        turns += 1
        u = getattr(message, "usage", None)
        if u is not None:
            totals["input"] += getattr(u, "input_tokens", 0) or 0
            totals["output"] += getattr(u, "output_tokens", 0) or 0
            totals["cache_read"] += getattr(u, "cache_read_input_tokens", 0) or 0
            totals["cache_write"] += getattr(u, "cache_creation_input_tokens", 0) or 0
        for block in message.content:
            if block.type == "text" and block.text.strip():
                print(block.text, flush=True)
            elif block.type == "thinking" and getattr(block, "thinking", ""):
                print(f"[thinking] {block.thinking}", flush=True)
            elif block.type == "tool_use":
                print(f"[tool] {block.name}({_fmt_input(block.input)})", flush=True)
        if message.stop_reason == "refusal":
            print("\n[The model declined this request.]", file=sys.stderr)
            break
        spent = sum(totals.values())
        if token_budget and spent >= token_budget:
            print(
                f"\n[Token budget reached — {spent:,} of {token_budget:,} tokens "
                "spent across this run. Stopping so the key can't keep draining. "
                "Raise --token-budget (or pass 0 to disable) to allow more.]",
                file=sys.stderr,
            )
            break
        if turns >= max_turns:
            print(
                f"\n[Reached the {max_turns}-turn cap — stopping so a "
                "runaway loop can't keep spending. Raise --max-turns "
                "to allow more.]",
                file=sys.stderr,
            )
            break
    _report_spend(model, totals)
    return 0


# --------------------------------------------------------------------------
# provider: openai (any OpenAI-compatible /chat/completions endpoint)
# --------------------------------------------------------------------------

def _prepare_openai(base_url, key_env="OPENAI_API_KEY"):
    """Import the SDK and resolve credentials BEFORE any VM is spawned.
    Returns a client, or an int exit code on failure."""
    try:
        from openai import AsyncOpenAI
    except ImportError:
        print(
            "The `openai` SDK is required for --provider openai:\n"
            "  pip install -r requirements-agent.txt",
            file=sys.stderr,
        )
        return 2
    api_key = os.environ.get(key_env)
    if not api_key:
        if base_url:
            # Local/self-hosted servers (Ollama, LM Studio, llama.cpp) ignore
            # the key entirely; the SDK just insists one exists.
            api_key = "byo-no-key"
        else:
            print(
                "Bring your own key: `export OPENAI_API_KEY=...` — or point "
                "--base-url at a keyless local server (e.g. Ollama at "
                "http://localhost:11434/v1).",
                file=sys.stderr,
            )
            return 2
    return AsyncOpenAI(api_key=api_key, base_url=base_url or None)


def _openai_tools(mcp_tools):
    """MCP tool → OpenAI function-calling schema. The inputSchema IS the
    frozen contract (name + JSON Schema), so this is a direct re-labelling,
    not a re-description — still zero tool duplication."""
    return [
        {
            "type": "function",
            "function": {
                "name": t.name,
                "description": (t.description or "").strip()[:1024],
                "parameters": t.inputSchema or {"type": "object", "properties": {}},
            },
        }
        for t in mcp_tools
    ]


async def _call_mcp_tool(session, name, arguments_json):
    """Execute one MCP tool for the openai loop and flatten the result to
    text. Errors are RETURNED as tool output (not raised) so the model can
    read them and adjust — a bad tool call must not kill the run."""
    try:
        args = json.loads(arguments_json) if arguments_json else {}
    except json.JSONDecodeError as exc:
        return f"TOOL-CALL ERROR: arguments were not valid JSON ({exc}); retry with corrected JSON."
    try:
        result = await session.call_tool(name, args)
    except Exception as exc:  # noqa: BLE001 — feed the failure back to the model
        return f"TOOL-CALL ERROR: {exc}"
    parts = []
    for item in getattr(result, "content", None) or []:
        text = getattr(item, "text", None)
        parts.append(text if text is not None else str(item))
    text = "\n".join(parts).strip() or "(empty result)"
    if getattr(result, "isError", False):
        text = f"TOOL ERROR:\n{text}"
    return text


async def _chat(client, mt, **kw):
    """One completion call. Newer OpenAI models only accept
    max_completion_tokens; many compatible servers only know max_tokens —
    try the modern name first and fall back on a 400 that names it."""
    from openai import BadRequestError

    try:
        return await client.chat.completions.create(max_completion_tokens=mt, **kw)
    except BadRequestError as exc:
        if "max_completion_tokens" not in str(exc):
            raise
        return await client.chat.completions.create(max_tokens=mt, **kw)


async def _drive_openai(client, session, mcp_tools, task, model, max_tokens, max_turns):
    """A plain function-calling loop against any OpenAI-compatible endpoint:
    send messages+tools, execute every tool_call against the MCP session,
    append the results, repeat until the model stops calling tools."""
    tools = _openai_tools(mcp_tools)
    messages = [
        {"role": "system", "content": SYSTEM},
        {"role": "user", "content": task},
    ]
    for _turn in range(max_turns):
        rsp = await _chat(client, max_tokens, model=model, messages=messages, tools=tools)
        msg = rsp.choices[0].message
        if getattr(msg, "refusal", None):
            print(f"\n[The model declined this request: {msg.refusal}]", file=sys.stderr)
            return 0
        if msg.content and msg.content.strip():
            print(msg.content, flush=True)
        if not msg.tool_calls:
            return 0
        messages.append(
            {
                "role": "assistant",
                "content": msg.content or None,
                "tool_calls": [
                    {
                        "id": tc.id,
                        "type": "function",
                        "function": {
                            "name": tc.function.name,
                            "arguments": tc.function.arguments,
                        },
                    }
                    for tc in msg.tool_calls
                ],
            }
        )
        for tc in msg.tool_calls:
            print(f"[tool] {tc.function.name}({tc.function.arguments or ''})", flush=True)
            out = await _call_mcp_tool(session, tc.function.name, tc.function.arguments)
            messages.append({"role": "tool", "tool_call_id": tc.id, "content": out})
    print(
        f"\n[Reached the {max_turns}-turn cap — stopping so a runaway loop "
        "can't keep spending. Raise --max-turns to allow more.]",
        file=sys.stderr,
    )
    return 0


# --------------------------------------------------------------------------
# shared driver
# --------------------------------------------------------------------------

async def run_agent(task, provider, model, base_url, max_tokens, effort, max_turns, token_budget,
                    server_cmd=None, key_env="OPENAI_API_KEY"):
    """Drive QuantumOS with the chosen model: resolve provider credentials
    FIRST (no VM is ever spawned for a run that can't talk to a model), then
    spawn the MCP server and hand the session to the provider loop. Guarantees
    the guest is powered off on exit and caps the number of turns so a runaway
    loop cannot silently burn the user's key."""
    prep = (_prepare_anthropic() if provider == "anthropic"
            else _prepare_openai(base_url, key_env))
    if isinstance(prep, int):
        return prep
    client = prep

    # A --server-cmd server finds its own kernel (the hub may run it remotely).
    hint = None if server_cmd else _kernel_hint()
    if hint:
        print(f"WARNING: {hint}\n", file=sys.stderr)

    from mcp import ClientSession
    from mcp.client.stdio import stdio_client

    async with stdio_client(_server_params(server_cmd)) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()
            mcp_tools, mapping = _wire_tools(await _open_tools(session))
            wire = _WireSession(session, mapping)
            print(
                f"Connected to QuantumOS ({len(mcp_tools)} tools). "
                f"Provider: {provider}, model: {model}.\n"
            )
            # cleanup runs in finally so a booted guest is never orphaned — on
            # normal completion, the turn cap, a refusal, or Ctrl-C.
            try:
                if provider == "anthropic":
                    return await _drive_anthropic(
                        client, wire, mcp_tools, task, model, max_tokens, effort, max_turns,
                        token_budget,
                    )
                return await _drive_openai(
                    client, wire, mcp_tools, task, model, max_tokens, max_turns
                )
            finally:
                print("\n[shutting down any VM the agent booted…]", file=sys.stderr)
                await _cleanup(session)


def main():
    p = argparse.ArgumentParser(
        description=(
            "Drive a live QuantumOS VM with the model of your choice over its "
            "frozen MCP tools (BYO key; Claude by default)."
        )
    )
    p.add_argument("task", nargs="?", default=None,
                   help="a free-form experiment for the model to run (overrides --experiment); '-' reads it from stdin")
    p.add_argument("--experiment", choices=sorted(EXPERIMENTS), default="recall",
                   help="a curated experiment when no free-form task is given (default: recall)")
    p.add_argument("--provider", choices=PROVIDERS, default=None,
                   help="anthropic = Claude via the Anthropic SDK (default); openai = any "
                        "OpenAI-compatible endpoint (env QOS_AGENT_PROVIDER)")
    p.add_argument("--model", default=None,
                   help="model id. anthropic default: claude-sonnet-5 (env QOS_CLAUDE_MODEL; "
                        "claude-opus-4-8 / claude-fable-5 for the hardest reasoning). REQUIRED for openai — "
                        "whatever your endpoint serves, e.g. gpt-5.1 or llama3.3")
    p.add_argument("--base-url", default=os.environ.get("QOS_AGENT_BASE_URL"),
                   help="OpenAI-compatible endpoint, e.g. http://localhost:11434/v1 for a local "
                        "Ollama (env QOS_AGENT_BASE_URL); implies --provider openai")
    p.add_argument("--max-tokens", type=int, default=16000)
    p.add_argument("--effort", default="high", choices=["low", "medium", "high", "xhigh", "max"],
                   help="reasoning effort (anthropic only; ignored for openai)")
    p.add_argument("--max-turns", type=int, default=40,
                   help="hard cap on agent turns so a runaway loop can't burn your key (default 40)")
    p.add_argument("--token-budget", type=int,
                   default=int(os.environ.get("QOS_TOKEN_BUDGET", "400000")),
                   help="cumulative token cap (input+output+cache) per run, anthropic only; "
                        "the loop stops when reached and a spend estimate is printed "
                        "(default 400000; env QOS_TOKEN_BUDGET; 0 = unlimited)")
    p.add_argument("--server-cmd", default=os.environ.get("QOS_AGENT_SERVER_CMD"),
                   help="launch this MCP server instead of scripts/qos_mcp.py, e.g. the hub: "
                        "\"qos-hub serve --config hub.json --db broker.db\" (env "
                        "QOS_AGENT_SERVER_CMD; split like a shell, so use forward slashes)")
    p.add_argument("--api-key-env", default=os.environ.get("QOS_AGENT_API_KEY_ENV", "OPENAI_API_KEY"),
                   help="environment variable holding the key for --provider openai, e.g. "
                        "OPENROUTER_API_KEY (default OPENAI_API_KEY)")
    p.add_argument("--list-tools", action="store_true",
                   help="list the QuantumOS MCP tools and exit (no API call, no key needed)")
    args = p.parse_args()

    if args.list_tools:
        return asyncio.run(list_tools_only(args.server_cmd))

    provider = args.provider or os.environ.get("QOS_AGENT_PROVIDER") or (
        "openai" if args.base_url else "anthropic"
    )
    if provider not in PROVIDERS:
        p.error(f"unknown provider {provider!r} (QOS_AGENT_PROVIDER?) — choose from {PROVIDERS}")
    if provider == "anthropic":
        model = (args.model or os.environ.get("QOS_CLAUDE_MODEL")
                 or os.environ.get("QOS_AGENT_MODEL") or "claude-sonnet-5")
    else:
        model = args.model or os.environ.get("QOS_AGENT_MODEL")
        if not model:
            p.error("--model is required with --provider openai — pass whatever your "
                    "endpoint serves (e.g. gpt-5.1, or llama3.3 for a local Ollama)")

    if args.task == "-":
        task = sys.stdin.read()
    else:
        task = args.task if args.task is not None else EXPERIMENTS[args.experiment]
    return asyncio.run(
        run_agent(task, provider, model, args.base_url, args.max_tokens, args.effort, args.max_turns,
                  args.token_budget, args.server_cmd, args.api_key_env)
    )


if __name__ == "__main__":
    sys.exit(main())
