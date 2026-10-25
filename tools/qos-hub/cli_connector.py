"""CLI connector for the QuantumOS hub (#297 slice 3): a CLI with no MCP server, exposed as
hub tools through allowlisted argv templates.

Config, as one entry under "servers":
  "kcli": {"cli": {
      "binary": "kannaka",              (or a command prefix: ["node", "~/0xSCADA/cli/dist/index.js"])
      "cwd": "~/somewhere",             (optional working directory)
      "env": {"KANNAKA_QUIET": "1"},
      "timeout_s": 60,
      "tools": {
        "observe": {"description": "Consciousness observation as JSON.", "argv": ["observe", "--json"]},
        "recall":  {"description": "Search memories.", "argv": ["recall", "{query}", "--top-k", "{k}"],
                    "params": {"query": {"type": "string", "max_len": 200},
                               "k": {"type": "integer", "min": 1, "max": 20, "default": 5}}}}}}

Rules, all of which hold before anything runs:
  * Only the configured tools exist. The connector never runs a command it was not given;
    in particular it never probes a CLI with --help (on a host with a live Kannaka store,
    `kannaka --help` is not harmless).
  * A "{name}" placeholder must be an ENTIRE argv element; it is replaced by one validated
    value and never split, joined or passed through a shell. No placeholder may appear
    inside a larger string.
  * Every parameter has a type: string (max_len, optional regex `pattern` that must match the
    whole value, optional `enum`), integer (min, max) or boolean (rendered as "true"/"false").
    A string value may not start with "-", so it can never be read as an option.
  * Unknown parameters are refused. Missing ones take their `default`, or are refused.
  * A wall-clock timeout (timeout_s, default 60) and an output cap (64 KiB) bound every run.

The result is {"exit_code", "stdout", "stderr"} (stdout parsed as JSON when it is JSON). A
non-zero exit is reported as an error result, not raised.
"""
from __future__ import annotations

import asyncio
import json
import os
import re
from types import SimpleNamespace as NS

PLACEHOLDER = re.compile(r"^\{([a-z_][a-z0-9_]{0,31})\}$")
MAX_OUT = 64 * 1024
# Colour and cursor escapes a CLI prints for a terminal; an agent reads them as noise.
ANSI = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")
# Variables a Python launcher sets for ITSELF: uv's venv launcher on Windows sets PYTHONHOME to its own
# interpreter, and a different Python started with it inherited loads the wrong standard library and
# packages ("No module named ..."). A server entry that wants one sets it explicitly in its env.
LAUNCHER_VARS = ("PYTHONHOME", "UV_INTERNAL__PYTHONHOME", "__PYVENV_LAUNCHER__")


def child_env(extra: dict | None) -> dict:
    """The environment for a program the hub starts: its own, minus the launcher variables, plus `extra`."""
    env = {k: v for k, v in os.environ.items() if k not in LAUNCHER_VARS}
    env.update({k: os.path.expanduser(v) for k, v in (extra or {}).items()})
    return env


class ConfigError(ValueError):
    pass


def _schema(params: dict) -> dict:
    props, required = {}, []
    for name, p in params.items():
        t = p.get("type")
        if t == "string":
            s = {"type": "string", "maxLength": int(p.get("max_len", 200))}
            if "enum" in p:
                s["enum"] = list(p["enum"])
            if "pattern" in p:
                s["pattern"] = p["pattern"]
        elif t == "integer":
            s = {"type": "integer", "minimum": int(p["min"]), "maximum": int(p["max"])}
        elif t == "boolean":
            s = {"type": "boolean"}
        else:
            raise ConfigError(f"parameter {name!r}: type must be string, integer or boolean")
        if "description" in p:
            s["description"] = p["description"]
        if "default" in p:
            s["default"] = p["default"]
        else:
            required.append(name)
        props[name] = s
    out = {"type": "object", "properties": props, "additionalProperties": False}
    if required:
        out["required"] = required
    return out


class CliSession:
    """Looks like an MCP ClientSession to the hub: list_tools() and call_tool()."""

    def __init__(self, spec: dict):
        b = spec["binary"]
        self.prefix = [os.path.expanduser(x) for x in (b if isinstance(b, list) else [b])]
        if not self.prefix or not all(isinstance(x, str) and x for x in self.prefix):
            raise ConfigError("binary must be a program or a non-empty command prefix list")
        self.binary = self.prefix[0]
        self.cwd = os.path.expanduser(spec["cwd"]) if spec.get("cwd") else None
        self.env = child_env(spec.get("env"))
        self.timeout = float(spec.get("timeout_s", 60))
        self.tools: dict[str, dict] = {}
        for name, t in (spec.get("tools") or {}).items():
            params = t.get("params") or {}
            argv = t.get("argv")
            if not isinstance(argv, list) or not all(isinstance(a, str) for a in argv):
                raise ConfigError(f"tool {name!r}: argv must be a list of strings")
            used = set()
            for a in argv:
                m = PLACEHOLDER.match(a)
                if m:
                    if m.group(1) not in params:
                        raise ConfigError(f"tool {name!r}: placeholder {a} has no parameter")
                    used.add(m.group(1))
                elif "{" in a or "}" in a:
                    raise ConfigError(f"tool {name!r}: {a!r}: a placeholder must be a whole argv element")
            unused = set(params) - used
            if unused:
                raise ConfigError(f"tool {name!r}: parameters never used in argv: {sorted(unused)}")
            self.tools[name] = {"argv": argv, "params": params, "schema": _schema(params),
                                "description": t.get("description", "")}

    async def list_tools(self):
        return NS(tools=[NS(name=n, description=t["description"], inputSchema=t["schema"])
                         for n, t in self.tools.items()])

    @staticmethod
    def _value(name: str, p: dict, v) -> str:
        t = p["type"]
        if t == "string":
            if not isinstance(v, str):
                raise ValueError(f"{name} must be a string")
            if len(v) > int(p.get("max_len", 200)) or "\x00" in v:
                raise ValueError(f"{name} is too long or has a NUL byte")
            if v.startswith("-"):
                raise ValueError(f"{name} may not start with '-'")
            if "enum" in p and v not in p["enum"]:
                raise ValueError(f"{name} must be one of {p['enum']}")
            if "pattern" in p and not re.fullmatch(p["pattern"], v):
                raise ValueError(f"{name} does not match {p['pattern']}")
            return v
        if t == "integer":
            if not isinstance(v, int) or isinstance(v, bool) or not int(p["min"]) <= v <= int(p["max"]):
                raise ValueError(f"{name} must be an integer from {p['min']} to {p['max']}")
            return str(v)
        if not isinstance(v, bool):
            raise ValueError(f"{name} must be true or false")
        return "true" if v else "false"

    def render(self, tool: str, arguments: dict) -> list[str]:
        t = self.tools[tool]
        extra = set(arguments) - set(t["params"])
        if extra:
            raise ValueError(f"unknown parameters: {sorted(extra)}")
        vals = {}
        for name, p in t["params"].items():
            if name in arguments:
                vals[name] = self._value(name, p, arguments[name])
            elif "default" in p:
                vals[name] = self._value(name, p, p["default"])
            else:
                raise ValueError(f"missing parameter {name}")
        out = list(self.prefix)
        for a in t["argv"]:
            m = PLACEHOLDER.match(a)
            out.append(vals[m.group(1)] if m else a)
        return out

    def validate(self, name: str, arguments: dict) -> None:
        """Raise ValueError if the call could not run, without running it (the hub checks a call
        before asking the person to approve it)."""
        if name not in self.tools:
            raise ValueError(f"no tool {name}")
        self.render(name, arguments or {})

    async def call_tool(self, name: str, arguments: dict):
        def result(obj, error=False):
            return NS(content=[NS(type="text", text=json.dumps(obj))], isError=error)
        if name not in self.tools:
            return result({"error": f"no tool {name}"}, True)
        try:
            argv = self.render(name, arguments or {})
        except ValueError as e:
            return result({"error": "bad_arguments", "message": str(e)}, True)
        try:
            proc = await asyncio.create_subprocess_exec(*argv, stdin=asyncio.subprocess.DEVNULL,
                                                        stdout=asyncio.subprocess.PIPE,
                                                        stderr=asyncio.subprocess.PIPE, env=self.env,
                                                        cwd=self.cwd)
        except OSError as e:
            return result({"error": "cannot_start", "message": f"{self.binary}: {e}"}, True)
        try:
            out, err = await asyncio.wait_for(proc.communicate(), self.timeout)
        except asyncio.TimeoutError:
            proc.kill()
            await proc.wait()
            return result({"error": "timeout", "message": f"no result within {self.timeout:g} s"}, True)
        text = ANSI.sub("", out[:MAX_OUT].decode(errors="replace"))
        try:
            parsed = json.loads(text)
        except ValueError:
            parsed = text + ("\n[truncated]" if len(out) > MAX_OUT else "")
        body = {"exit_code": proc.returncode, "stdout": parsed,
                "stderr": ANSI.sub("", err[:4096].decode(errors="replace"))}
        return result(body, proc.returncode != 0)
