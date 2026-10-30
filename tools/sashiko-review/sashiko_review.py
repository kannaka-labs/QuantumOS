#!/usr/bin/env python3
"""Sashiko review as a service the person pays for with their own model key (ADR-0028).

Point an agent at this server and it can get an AI review of a commit in one of the person's own
repositories, by Sashiko (sashiko-dev/sashiko, Apache-2.0) or the Kannaka Labs fork
(kannaka-labs/sashiko-kannaka), on the person's machine, with the person's model provider and key:

  review_setup    what is configured: repositories, provider, model, the token cap, whether the key is present
  review_quote    the commit, the size of its diff, the cap and the worst case in dollars, before anything runs
  review_start    queue a review (it spends the person's tokens, so the hub asks first by default)
  review_status   one job: queued, running, done, failed, cancelled or interrupted
  review_result   the findings of a finished job (severity, problem, file and line), and the tokens it used
  review_cancel   stop a queued or running job
  review_jobs     the most recent jobs

Rules, all of which hold before Sashiko runs:
  * The key is read from the environment variable or owner-only file the config names, and handed only to the
    Sashiko process, under the variable its provider expects. It is never in the config, argv, the job's files,
    a log or a result; any output that contains it is redacted.
  * Every review has a hard cap: `max_total_tokens` is required (there is no default), and reaches Sashiko's own
    budget, which aborts the review when uncached input plus output passes it.
  * Repositories are named in the config; the agent refers to them by name. A revision must resolve to a commit
    with `git rev-parse --verify --end-of-options`, and only the resolved ids reach Sashiko. Sashiko's
    --custom-prompt is not exposed.
  * One review at a time. A job that was running when the server stopped is marked `interrupted` and not re-run,
    because running it again would spend again.
  * Nothing is posted anywhere. The findings go back to the caller.

  qos-review example > review.json      (edit it: binary, provider, model, key, cap, repositories)
  qos-review check --config review.json
  qos-review serve --config review.json (MCP over stdio)
  qos-review quote|start|status|result|cancel|jobs --config review.json ...   (the same tools, from a shell)
  Dep: mcp>=1.10,<2 for `serve`; the rest is stdlib. Linux or macOS (Sashiko does not build on Windows).
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import secrets
import signal
import subprocess
import sys
import threading
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJECTS = ("kannaka", "sashiko")
# The variable each API provider reads its key from (Sashiko's src/ai/*.rs). CLI providers and a local Ollama
# use their own login or none, so they take no key here.
KEY_VARS = {"claude": "ANTHROPIC_API_KEY", "openai": "OPENAI_API_KEY", "openai-compatible": "OPENAI_API_KEY",
            "gemini": "GEMINI_API_KEY", "vllm": "VLLM_API_KEY"}
KEYLESS = {"claude-cli", "codex-cli", "ollama"}
PROVIDER_TABLES = {"claude", "gemini", "openai_compat", "ollama", "vllm", "claude_cli", "codex_cli"}
# What the Sashiko process inherits from this one, besides its key. Everything else, other providers' keys
# included, is left behind.
BASE_ENV = ("PATH", "HOME", "USER", "LOGNAME", "LANG", "LC_ALL", "TMPDIR", "XDG_CONFIG_HOME", "XDG_DATA_HOME",
            "XDG_CACHE_HOME")
REV = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._/~^@{}-]{0,99}$")
NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
JOB = re.compile(r"^r-[0-9a-f]{12}$")
FINAL = {"done", "failed", "cancelled", "interrupted"}
MAX_FINDINGS = 50


class Refused(Exception):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code, self.message = code, message

    def as_dict(self) -> dict:
        return {"ok": False, "error": self.code, "message": self.message}


def _now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds")


def _toml(v) -> str:
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return repr(v)
    if isinstance(v, str):
        return json.dumps(v)          # a JSON string is a valid TOML basic string
    raise Refused("bad_config", f"provider settings take strings, numbers and booleans only, not {type(v).__name__}")


def _write_private(path: Path, text: str) -> None:
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write(text)


def load_config(path: str) -> dict:
    try:
        cfg = json.loads(Path(os.path.expanduser(path)).read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise Refused("bad_config", f"cannot read {path}: {e}") from None
    return validate_config(cfg)


def validate_config(cfg: dict) -> dict:
    def need(cond, msg):
        if not cond:
            raise Refused("bad_config", msg)

    need(isinstance(cfg.get("binary"), str) and cfg["binary"], "binary: the path of your sashiko or sashiko-kannaka")
    prov = cfg.get("provider")
    need(prov in KEY_VARS or prov in KEYLESS, f"provider must be one of {sorted(set(KEY_VARS) | KEYLESS)}")
    need(isinstance(cfg.get("model"), str) and cfg["model"], "model: the provider's model name")
    cap = cfg.get("max_total_tokens")
    need(isinstance(cap, int) and not isinstance(cap, bool) and cap > 0,
         "max_total_tokens is required: the most uncached input plus output tokens one review may use")
    out_cap = cfg.get("max_total_output_tokens", min(cap, 500_000))
    need(isinstance(out_cap, int) and 0 < out_cap <= cap, "max_total_output_tokens must be between 1 and the cap")
    if prov in KEY_VARS:
        need(bool(cfg.get("key_env")) != bool(cfg.get("key_file")),
             f"{prov} needs exactly one of key_env (a variable holding your key) or key_file (an owner-only file)")
    else:
        need(not cfg.get("key_env") and not cfg.get("key_file"), f"{prov} takes no key here (it uses its own login)")
    repos = cfg.get("repos")
    need(isinstance(repos, dict) and repos, "repos: at least one {\"name\": \"/path/to/checkout\"}")
    for name in repos:
        need(bool(NAME.match(name)), f"repository name {name!r}: letters, digits, '.', '_' and '-' only")
    for table, vals in (cfg.get("provider_settings") or {}).items():
        need(table in PROVIDER_TABLES, f"provider_settings: {table!r} is not one of {sorted(PROVIDER_TABLES)}")
        need(isinstance(vals, dict) and all(NAME.match(k) for k in vals), f"provider_settings.{table}: simple keys")
        for k in vals:
            need(not re.search(r"key|token|secret", k, re.I),
                 f"provider_settings.{table}.{k}: keys go in key_env or key_file, never in the config")
    price = cfg.get("price") or {}
    for k, v in price.items():
        need(k in ("usd_per_mtok_in", "usd_per_mtok_out", "usd_per_mtok_cached") and isinstance(v, (int, float))
             and v >= 0, f"price.{k}: one of usd_per_mtok_in / _out / _cached, a number >= 0")
    projects = cfg.get("projects", list(PROJECTS))
    need(isinstance(projects, list) and projects and set(projects) <= set(PROJECTS), f"projects: a subset of {PROJECTS}")
    need(all(isinstance(n, str) and n not in BASE_ENV and NAME.match(n) and not re.search(r"KEY|TOKEN|SECRET", n)
             for n in cfg.get("pass_env", [])), "pass_env: names of non-secret variables only")
    return {**cfg, "max_total_output_tokens": out_cap, "projects": projects,
            "timeout_s": int(cfg.get("timeout_s", 3600)), "max_input_tokens": int(cfg.get("max_input_tokens", 190000)),
            "max_interactions": int(cfg.get("max_interactions", 150))}


class ReviewService:
    def __init__(self, cfg: dict, jobs_dir: str | None = None, environ: dict | None = None):
        self.cfg = cfg
        self.env = dict(os.environ if environ is None else environ)
        self.dir = Path(os.path.expanduser(jobs_dir or cfg.get("jobs_dir") or "~/.qos-review/jobs"))
        self.dir.mkdir(mode=0o700, parents=True, exist_ok=True)
        os.chmod(self.dir, 0o700)
        self._lock = threading.Lock()
        self._procs: dict[str, subprocess.Popen] = {}
        self._worker: threading.Thread | None = None
        for job in self._all():                         # a job left by a stopped server is not run again
            if job["state"] == "running":
                self._update(job["id"], state="interrupted", finished=_now(),
                             message="the review server stopped while this review ran; it was not re-run, because "
                                     "that would spend again. Tokens it used are spent")
            elif job["state"] == "queued":
                self._update(job["id"], state="interrupted", finished=_now(),
                             message="the review server stopped before this review started; nothing was spent. "
                                     "Start it again if you still want it")

    # ---- key ----
    def _key(self) -> str | None:
        prov = self.cfg["provider"]
        if prov not in KEY_VARS:
            return None
        if self.cfg.get("key_env"):
            key = self.env.get(self.cfg["key_env"], "")
        else:
            p = Path(os.path.expanduser(self.cfg["key_file"]))
            try:
                if p.stat().st_mode & 0o077:
                    raise Refused("key_file_open", f"{p} can be read by others; chmod 600 it")
                key = p.read_text(encoding="utf-8").strip()
            except OSError:
                key = ""
        return key or None

    def _redact(self, text: str) -> str:
        key = None
        try:
            key = self._key()
        except Refused:
            pass
        return text.replace(key, "<key>") if key and len(key) >= 8 else text

    # ---- repositories and revisions ----
    def _repo(self, name: str) -> Path:
        if not isinstance(name, str) or name not in self.cfg["repos"]:
            raise Refused("unknown_repo", f"no repository {name!r}; configured: {sorted(self.cfg['repos'])}")
        p = Path(os.path.expanduser(self.cfg["repos"][name]))
        if not (p / ".git").exists():
            raise Refused("not_a_checkout", f"repository {name!r} is not a git checkout")
        return p

    def _git(self, repo: Path, *args) -> str:
        r = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True, timeout=60,
                           env={k: v for k, v in self.env.items() if k in BASE_ENV})
        if r.returncode:
            raise Refused("bad_revision", (r.stderr or "git failed").strip()[-300:])
        return r.stdout

    def _resolve(self, repo: Path, rev: str) -> tuple[str, str | None]:
        """(commit, base or None). A range is two revisions joined by '..'."""
        if not isinstance(rev, str):
            raise Refused("bad_revision", "rev must be a string")
        parts = rev.split("..")
        if len(parts) > 2 or not all(REV.match(p) for p in parts):
            raise Refused("bad_revision", "rev must be a commit, a branch or tag name, or base..head")
        shas = [self._git(repo, "rev-parse", "--verify", "--end-of-options", p + "^{commit}").strip() for p in parts]
        return (shas[1], shas[0]) if len(shas) == 2 else (shas[0], None)

    def _project(self, project: str) -> str:
        if project not in self.cfg["projects"]:
            raise Refused("bad_project", f"project must be one of {self.cfg['projects']}")
        return project

    # ---- tools ----
    def setup(self) -> dict:
        b = Path(os.path.expanduser(self.cfg["binary"]))
        try:
            key_ok = self._key() is not None if self.cfg["provider"] in KEY_VARS else None
            key_note = None
        except Refused as e:
            key_ok, key_note = False, e.message
        sha = hashlib.sha256(b.read_bytes()).hexdigest() if b.is_file() else None
        return {"ok": True, "binary": str(b), "binary_found": sha is not None, "binary_sha256": sha,
                "provider": self.cfg["provider"], "model": self.cfg["model"],
                "key": ("not needed (the provider's own login)" if key_ok is None else
                        "present" if key_ok else (key_note or "MISSING")),
                "max_total_tokens": self.cfg["max_total_tokens"],
                "max_total_output_tokens": self.cfg["max_total_output_tokens"],
                "price": self.cfg.get("price") or None, "projects": self.cfg["projects"],
                "repos": sorted(self.cfg["repos"]), "posts_anywhere": False}

    def _worst_case(self) -> float | None:
        p = self.cfg.get("price") or {}
        if "usd_per_mtok_in" not in p or "usd_per_mtok_out" not in p:
            return None
        cap, out_cap = self.cfg["max_total_tokens"], self.cfg["max_total_output_tokens"]
        return round((out_cap * p["usd_per_mtok_out"] + (cap - out_cap) * p["usd_per_mtok_in"]) / 1e6, 2)

    def quote(self, repo: str, rev: str, project: str) -> dict:
        path = self._repo(repo)
        project = self._project(project)
        head, base = self._resolve(path, rev)
        rng = [f"{base}..{head}"] if base else [f"{head}^!"]
        if base:
            stat = self._git(path, "diff", "--shortstat", base, head).strip()
        elif self._has_parent(path, head):
            stat = self._git(path, "diff", "--shortstat", f"{head}^", head).strip()
        else:
            stat = "root commit"
        subject = self._git(path, "log", "-1", "--format=%s", head).strip()
        commits = int(self._git(path, "rev-list", "--count", *rng).strip() or 0)
        worst = self._worst_case()
        return {"ok": True, "repo": repo, "project": project, "commit": head, "base": base, "commits": commits,
                "subject": subject, "diff": stat, "max_total_tokens": self.cfg["max_total_tokens"],
                "max_total_output_tokens": self.cfg["max_total_output_tokens"],
                "worst_case_usd": worst,
                "notes": ("The cap is enforced by Sashiko: the review aborts when uncached input plus output passes it, "
                          "and what was used until then is spent. Cached input is not counted by the cap; it is "
                          "reported afterwards. "
                          + ("Dollar figures are estimates from the prices in your config."
                             if worst is not None else "No prices are configured, so no dollar figure is given.")
                          + " For scale: on QuantumOS pull requests one review used about 1.1M input and 60k output "
                            "tokens.")}

    def _has_parent(self, repo: Path, sha: str) -> bool:
        return len(self._git(repo, "rev-list", "--parents", "-n", "1", sha).split()) > 1

    def start(self, repo: str, rev: str, project: str) -> dict:
        q = self.quote(repo, rev, project)
        if self.cfg["provider"] in KEY_VARS and not self._key():
            raise Refused("no_key", f"no key: set {self.cfg.get('key_env') or self.cfg.get('key_file')} first")
        if not Path(os.path.expanduser(self.cfg["binary"])).is_file():
            raise Refused("no_binary", f"{self.cfg['binary']} does not exist; install sashiko first")
        jid = "r-" + secrets.token_hex(6)
        jd = self.dir / jid
        jd.mkdir(mode=0o700)
        job = {"id": jid, "state": "queued", "repo": repo, "project": q["project"], "commit": q["commit"],
               "base": q["base"], "subject": q["subject"], "queued": _now(),
               "max_total_tokens": self.cfg["max_total_tokens"], "provider": self.cfg["provider"],
               "model": self.cfg["model"]}
        self._save(job)
        self._kick()
        return {"ok": True, "job": jid, "state": "queued", "commit": q["commit"], "subject": q["subject"],
                "max_total_tokens": job["max_total_tokens"], "worst_case_usd": q["worst_case_usd"],
                "next": "review_status (a review takes minutes; poll every minute or two), then review_result"}

    def status(self, job: str) -> dict:
        j = self._load(job)
        return {"ok": True, **{k: j.get(k) for k in ("id", "state", "repo", "project", "commit", "subject", "queued",
                                                     "started", "finished", "exit_code", "message", "tokens")}}

    def result(self, job: str) -> dict:
        j = self._load(job)
        if j["state"] != "done":
            return {"ok": False, "error": "not_done", "state": j["state"], "message": j.get("message")}
        rep = json.loads((self.dir / job / "report.json").read_text(encoding="utf-8"))
        rv = rep.get("review") or {}
        findings = []
        for f in (rv.get("findings") or [])[:MAX_FINDINGS]:
            locs = [{"file": loc.get("file"), "line": loc.get("line")} for loc in (f.get("locations") or [])[:3]
                    if isinstance(loc, dict)]
            findings.append({"severity": f.get("severity"), "problem": self._redact(str(f.get("problem") or "")),
                             "why": self._redact(str(f.get("severity_explanation") or ""))[:600],
                             "stage": f.get("stage"), "locations": locs})
        return {"ok": True, "job": job, "commit": j["commit"], "subject": j["subject"],
                "summary": self._redact(str(rv.get("summary") or ""))[:4000], "findings": findings,
                "findings_total": len(rv.get("findings") or []), "tokens": j.get("tokens"),
                "full_report": str(self.dir / job / "report.json"),
                "note": "These are an AI's findings, not verified; check each before acting on it. Nothing was posted."}

    def cancel(self, job: str) -> dict:
        j = self._load(job)
        if j["state"] in FINAL:
            return {"ok": True, "job": job, "state": j["state"]}
        with self._lock:
            proc = self._procs.get(job)
            self._update(job, state="cancelled", finished=_now(), message="cancelled")
        if proc and proc.poll() is None:
            try:
                os.killpg(proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        return {"ok": True, "job": job, "state": "cancelled",
                "note": "tokens used before the cancel are spent"}

    def jobs(self, limit: int = 20) -> dict:
        js = sorted(self._all(), key=lambda j: j["queued"], reverse=True)[:max(1, min(int(limit), 100))]
        return {"ok": True, "jobs": [{k: j.get(k) for k in ("id", "state", "repo", "commit", "subject", "queued",
                                                           "finished")} for j in js]}

    # ---- jobs on disk ----
    def _path(self, job: str) -> Path:
        if not isinstance(job, str) or not JOB.match(job):
            raise Refused("unknown_job", "no such job")
        p = self.dir / job / "job.json"
        if not p.exists():
            raise Refused("unknown_job", "no such job")
        return p

    def _load(self, job: str) -> dict:
        return json.loads(self._path(job).read_text(encoding="utf-8"))

    def _save(self, job: dict) -> None:
        p = self.dir / job["id"] / "job.json"
        tmp = p.with_suffix(".tmp")
        _write_private(tmp, json.dumps(job, indent=1, sort_keys=True))
        os.replace(tmp, p)

    def _update(self, job: str, **kw) -> dict:
        j = self._load(job)
        j.update(kw)
        self._save(j)
        return j

    def _all(self) -> list[dict]:
        out = []
        for p in self.dir.glob("r-*/job.json"):
            try:
                out.append(json.loads(p.read_text(encoding="utf-8")))
            except (OSError, ValueError):
                pass
        return out

    # ---- the runner: one review at a time ----
    def _kick(self) -> None:
        with self._lock:
            if self._worker and self._worker.is_alive():
                return
            self._worker = threading.Thread(target=self._drain, daemon=True)
            self._worker.start()

    def _drain(self) -> None:
        while True:
            with self._lock:
                queued = sorted((j for j in self._all() if j["state"] == "queued"), key=lambda j: j["queued"])
                if not queued:
                    self._worker = None
                    return
                job = self._update(queued[0]["id"], state="running", started=_now())
            try:
                self._run(job)
            except Exception as e:  # noqa: BLE001  (a job's failure must not stop the queue)
                with self._lock:
                    if self._load(job["id"])["state"] == "running":
                        self._update(job["id"], state="failed", finished=_now(), message=self._redact(str(e))[:500])

    def settings_toml(self, project: str) -> str:
        c = self.cfg
        lines = ['log_level = "warn"', "", "[project]", f"kind = {_toml(project)}", "", "[ai]",
                 f"provider = {_toml(c['provider'])}", f"model = {_toml(c['model'])}",
                 f"max_input_tokens = {c['max_input_tokens']}", f"max_interactions = {c['max_interactions']}"]
        for table, vals in (c.get("provider_settings") or {}).items():
            lines += ["", f"[ai.{table}]"] + [f"{k} = {_toml(v)}" for k, v in vals.items()]
        lines += ["", "[review]", "concurrency = 1", f"timeout_seconds = {c['timeout_s']}",
                  f"max_total_tokens = {c['max_total_tokens']}",
                  f"max_total_output_tokens = {c['max_total_output_tokens']}", ""]
        return "\n".join(lines)

    def child_env(self) -> dict:
        env = {k: v for k, v in self.env.items() if k in BASE_ENV or k in self.cfg.get("pass_env", [])}
        key = self._key()
        if key:
            env[KEY_VARS[self.cfg["provider"]]] = key
        return env

    def _run(self, job: dict) -> None:
        jd = self.dir / job["id"]
        settings = jd / "Settings.toml"
        _write_private(settings, self.settings_toml(job["project"]))
        target = f"{job['base']}..{job['commit']}" if job.get("base") else job["commit"]
        argv = [os.path.expanduser(self.cfg["binary"]), "review", "--settings", str(settings),
                "--project", job["project"], "--format", "json", target]
        repo = self._repo(job["repo"])
        with open(jd / "stdout.raw", "wb") as out, open(jd / "stderr.raw", "wb") as err:
            os.chmod(jd / "stdout.raw", 0o600)
            os.chmod(jd / "stderr.raw", 0o600)
            with self._lock:
                if self._load(job["id"])["state"] != "running":       # cancelled while it waited
                    return
                proc = subprocess.Popen(argv, cwd=repo, env=self.child_env(), stdout=out, stderr=err,
                                        stdin=subprocess.DEVNULL, start_new_session=True)
                self._procs[job["id"]] = proc
            try:
                rc = proc.wait(timeout=self.cfg["timeout_s"] + 300)
            except subprocess.TimeoutExpired:
                os.killpg(proc.pid, signal.SIGKILL)
                rc = proc.wait()
            finally:
                with self._lock:
                    self._procs.pop(job["id"], None)
        raw_out = (jd / "stdout.raw").read_bytes().decode("utf-8", "replace")
        raw_err = (jd / "stderr.raw").read_bytes().decode("utf-8", "replace")
        (jd / "stdout.raw").unlink()
        (jd / "stderr.raw").unlink()
        _write_private(jd / "stderr.log", self._redact(raw_err))
        tokens, report = None, None
        try:
            report = json.loads(raw_out)
            tokens = {k: report.get(k) for k in ("tokens_in", "tokens_out", "tokens_cached")}
            tokens["usd_estimate"] = self._cost(tokens)
        except ValueError:
            pass
        with self._lock:
            if self._load(job["id"])["state"] != "running":            # cancelled while it ran
                if report is not None:
                    _write_private(jd / "report.json", self._redact(raw_out))
                self._update(job["id"], exit_code=rc, tokens=tokens)
                return
            # Sashiko exits 0 with no high-severity findings and 1 when it has them; both are a finished review.
            if rc in (0, 1) and report is not None:
                _write_private(jd / "report.json", self._redact(raw_out))
                self._update(job["id"], state="done", finished=_now(), exit_code=rc, tokens=tokens)
            else:
                tail = self._redact(raw_err.strip())[-400:]
                budget = "Token budget exceeded" in raw_err
                self._update(job["id"], state="failed", finished=_now(), exit_code=rc, tokens=tokens,
                             message=("the review passed max_total_tokens and Sashiko stopped it; the tokens used "
                                      "are spent" if budget else f"sashiko exited {rc}: {tail}"))

    def _cost(self, t: dict) -> float | None:
        p = self.cfg.get("price") or {}
        if not p or t.get("tokens_in") is None:
            return None
        cached = int(t.get("tokens_cached") or 0)
        uncached = max(0, int(t["tokens_in"]) - cached)
        usd = (uncached * p.get("usd_per_mtok_in", 0) + int(t.get("tokens_out") or 0) * p.get("usd_per_mtok_out", 0)
               + cached * p.get("usd_per_mtok_cached", p.get("usd_per_mtok_in", 0))) / 1e6
        return round(usd, 4)

    def wait_idle(self, timeout: float = 30) -> None:
        """For tests and the CLI: wait for the queue to drain."""
        w = self._worker
        if w:
            w.join(timeout)


def _call(fn, *a, **kw) -> dict:
    try:
        return fn(*a, **kw)
    except Refused as e:
        return e.as_dict()


def build(svc: ReviewService):
    from mcp.server.fastmcp import FastMCP

    mcp = FastMCP("quantumos-review")

    @mcp.tool()
    def review_setup() -> dict:
        """What this review server is set up with: the repositories you can name, the profiles (kannaka, sashiko),
        the model provider and model, the token cap per review, any prices, and whether the key is present (never
        the key itself). Nothing is posted anywhere by any tool here."""
        return _call(svc.setup)

    @mcp.tool()
    def review_quote(repo: str, rev: str, project: str = "sashiko") -> dict:
        """Before a review: resolve rev (a commit, a branch or tag, or base..head) in the named repository, and
        return the commit, its subject, the diff size, the token cap and the worst case in dollars (when prices
        are configured). Runs nothing and spends nothing."""
        return _call(svc.quote, repo, rev, project)

    @mcp.tool()
    def review_start(repo: str, rev: str, project: str = "sashiko") -> dict:
        """Queue a Sashiko review of rev in the named repository, with the person's own model and key. It spends
        their tokens, up to the cap review_quote shows, so show them the quote first. Returns a job id; a review
        takes minutes. Then call review_status, and review_result when it is done."""
        return _call(svc.start, repo, rev, project)

    @mcp.tool()
    def review_status(job: str) -> dict:
        """One review job: queued, running, done, failed, cancelled or interrupted, with times, the exit code, a
        message on failure, and the tokens used once it has finished."""
        return _call(svc.status, job)

    @mcp.tool()
    def review_result(job: str) -> dict:
        """The findings of a finished review: severity, problem, why, stage, and file and line, plus Sashiko's
        summary and the tokens used. They are an AI's findings, not verified: check each before acting on it."""
        return _call(svc.result, job)

    @mcp.tool()
    def review_cancel(job: str) -> dict:
        """Stop a queued or running review. Tokens it used before the cancel are spent."""
        return _call(svc.cancel, job)

    @mcp.tool()
    def review_jobs(limit: int = 20) -> dict:
        """The most recent review jobs, newest first."""
        return _call(svc.jobs, limit)

    return mcp


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("example", help="print the example config (edit it, then save it as review.json)")
    HELP = {
        "check": "validate the config: the model key works, the budget parses, Sashiko answers",
        "serve": "expose the review as MCP tools over stdio (review_quote, review_start, ...)",
        "jobs": "list the reviews this config has started",
        "quote": "price a review of <repo> at <rev> without starting it",
        "start": "start a review of <repo> at <rev> (spends within the budget you set)",
        "status": "where a review job is",
        "result": "the finished review's findings",
        "cancel": "stop a running review",
    }
    for name in ("check", "serve", "jobs"):
        sub.add_parser(name, help=HELP[name]).add_argument(
            "--config", required=True, help="review.json (see `qos-review example`)")
    for name in ("quote", "start"):
        p = sub.add_parser(name, help=HELP[name])
        p.add_argument("--config", required=True, help="review.json (see `qos-review example`)")
        p.add_argument("repo", help="repository URL or local path")
        p.add_argument("rev", help="commit, branch or tag to review")
        p.add_argument("--project", default="sashiko", choices=PROJECTS, help="Sashiko project profile (default sashiko)")
        if name == "start":
            p.add_argument("--wait", action="store_true", help="stay until the review finishes")
    for name in ("status", "result", "cancel"):
        p = sub.add_parser(name, help=HELP[name])
        p.add_argument("--config", required=True, help="review.json (see `qos-review example`)")
        p.add_argument("job", help="the job id `start` printed")
    a = ap.parse_args(argv)
    if a.cmd == "example":
        print((HERE / "review.example.json").read_text(encoding="utf-8"), end="")
        return 0
    try:
        svc = ReviewService(load_config(a.config))
    except Refused as e:
        print(json.dumps(e.as_dict(), indent=1))
        return 2
    if a.cmd == "serve":
        build(svc).run()
        return 0
    if a.cmd == "check":
        out = svc.setup()
    elif a.cmd == "jobs":
        out = _call(svc.jobs)
    elif a.cmd in ("quote", "start"):
        out = _call(getattr(svc, a.cmd), a.repo, a.rev, a.project)
        if a.cmd == "start" and a.wait and out.get("ok"):
            svc.wait_idle(timeout=None)
            out = _call(svc.status, out["job"])
    else:
        out = _call(getattr(svc, a.cmd), a.job)
    print(json.dumps(out, indent=1))
    return 0 if out.get("ok", True) else 1


if __name__ == "__main__":
    sys.exit(main())
