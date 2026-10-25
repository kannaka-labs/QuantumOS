"""Tests for the review server (ADR-0028) against a stand-in Sashiko (fake_sashiko.py): the key reaches only the
Sashiko process and is redacted everywhere else, the cap reaches Sashiko's settings, revisions and repositories are
refused unless they resolve, one review runs at a time, and a cancelled or interrupted job is never run again."""
from __future__ import annotations

import asyncio
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import sashiko_review as sr  # noqa: E402

KEY = "sk-ant-test-0123456789abcdef"
OTHER = "sk-other-provider-secret-99"


def git(repo, *a):
    return subprocess.run(["git", "-C", str(repo), *a], check=True, capture_output=True, text=True).stdout.strip()


@pytest.fixture
def world(tmp_path):
    repo = tmp_path / "proj"
    repo.mkdir()
    git(repo, "init", "-q")
    git(repo, "config", "user.email", "t@example.com")
    git(repo, "config", "user.name", "T")
    (repo / "a.c").write_text("int a;\n")
    git(repo, "add", "a.c")
    git(repo, "commit", "-qm", "first")
    (repo / "a.c").write_text("int a;\nint b;\n")
    git(repo, "commit", "-qam", "add b")
    fake = tmp_path / "bin"
    fake.mkdir()
    shutil.copy(HERE / "fake_sashiko.py", fake / "fake_sashiko.py")
    exe = fake / "sashiko"
    exe.write_text(f'#!/bin/sh\nexec "{sys.executable}" "{fake / "fake_sashiko.py"}" "$@"\n')
    exe.chmod(0o755)
    cfg = {"binary": str(exe), "provider": "claude", "model": "claude-test", "key_env": "MY_KEY",
           "max_total_tokens": 3_000_000, "max_total_output_tokens": 300_000,
           "price": {"usd_per_mtok_in": 3.0, "usd_per_mtok_out": 15.0, "usd_per_mtok_cached": 0.3},
           "repos": {"proj": str(repo)}}
    env = {"PATH": os.environ["PATH"], "HOME": str(tmp_path), "MY_KEY": KEY, "OPENAI_API_KEY": OTHER,
           "SASHIKO_CONFIG": "/etc/elsewhere.toml"}

    def make(**over):
        c = sr.validate_config({**cfg, **over})
        return sr.ReviewService(c, jobs_dir=str(tmp_path / "jobs"), environ=env)

    def mode(m):
        (fake / "mode").write_text(m)

    def calls():
        p = fake / "calls.jsonl"
        return [json.loads(x) for x in p.read_text().splitlines()] if p.exists() else []

    return {"repo": repo, "make": make, "mode": mode, "calls": calls, "tmp": tmp_path, "cfg": cfg}


def wait_state(svc, job, states, timeout=20):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        st = svc.status(job)["state"]
        if st in states:
            return st
        time.sleep(0.05)
    raise AssertionError(f"{job} still {svc.status(job)['state']}")


def test_config_requires_a_cap_and_one_key_source(world):
    base = world["cfg"]
    for over, word in (({"max_total_tokens": None}, "max_total_tokens is required"),
                       ({"max_total_tokens": 0}, "max_total_tokens is required"),
                       ({"key_file": "~/k"}, "exactly one of key_env"),
                       ({"key_env": None}, "exactly one of key_env"),
                       ({"provider": "claude-cli"}, "takes no key"),
                       ({"provider": "something"}, "provider must be"),
                       ({"provider_settings": {"claude": {"api_key": "x"}}}, "never in the config"),
                       ({"provider_settings": {"bogus": {}}}, "not one of"),
                       ({"projects": ["linux"]}, "projects"),
                       ({"pass_env": ["GITHUB_TOKEN"]}, "non-secret"),
                       ({"repos": {"../up": "/tmp"}}, "repository name")):
        with pytest.raises(sr.Refused) as e:
            sr.validate_config({k: v for k, v in {**base, **over}.items() if v is not None})
        assert word in e.value.message, (over, e.value.message)
    ok = sr.validate_config({**base, "provider": "claude-cli", "key_env": None})
    assert ok["provider"] == "claude-cli"


def test_quote_resolves_the_commit_and_states_the_worst_case(world):
    svc = world["make"]()
    head = git(world["repo"], "rev-parse", "HEAD")
    q = svc.quote("proj", "HEAD", "sashiko")
    assert q["ok"] and q["commit"] == head and q["subject"] == "add b" and q["commits"] == 1
    assert "1 file changed" in q["diff"] and q["max_total_tokens"] == 3_000_000
    assert q["worst_case_usd"] == pytest.approx(0.3 * 15 + 2.7 * 3)            # output cap at $15, the rest at $3
    r = svc.quote("proj", "HEAD~1..HEAD", "kannaka")
    assert r["base"] == git(world["repo"], "rev-parse", "HEAD~1") and r["commits"] == 1
    root = svc.quote("proj", "HEAD~1", "sashiko")
    assert root["diff"] == "root commit"
    assert world["make"](price={})._worst_case() is None
    assert world["calls"]() == [], "a quote runs nothing"


@pytest.mark.parametrize("rev", ["--output=/tmp/x", "-x", "HEAD;rm -rf /", "a..b..c", "nope", "HEAD $(id)", "",
                                 "x" * 101])
def test_revisions_that_do_not_resolve_cleanly_are_refused(world, rev):
    svc = world["make"]()
    with pytest.raises(sr.Refused) as e:
        svc.quote("proj", rev, "sashiko")
    assert e.value.code == "bad_revision"


def test_unknown_repositories_jobs_and_profiles_are_refused(world):
    svc = world["make"]()
    for fn, args, code in ((svc.quote, ("other", "HEAD", "sashiko"), "unknown_repo"),
                           (svc.quote, ("proj", "HEAD", "linux"), "bad_project"),
                           (svc.status, ("r-000000000000",), "unknown_job"),
                           (svc.status, ("../../etc/passwd",), "unknown_job")):
        with pytest.raises(sr.Refused) as e:
            fn(*args)
        assert e.value.code == code


def test_a_review_runs_with_the_key_only_in_sashiko_and_redacted_everywhere_else(world):
    world["mode"]("ok")
    svc = world["make"]()
    st = svc.start("proj", "HEAD", "kannaka")
    assert st["ok"] and st["state"] == "queued" and st["worst_case_usd"] == pytest.approx(12.6)
    assert wait_state(svc, st["job"], sr.FINAL) == "done"
    (call,) = world["calls"]()
    head = git(world["repo"], "rev-parse", "HEAD")
    assert call["argv"][-1] == head and call["argv"][:1] == ["review"]
    assert call["argv"][call["argv"].index("--project") + 1] == "kannaka" and "--format" in call["argv"]
    assert "--custom-prompt" not in call["argv"] and call["cwd"] == str(world["repo"])
    env = call["env"]
    assert env["ANTHROPIC_API_KEY"] == KEY, "the key reaches Sashiko under its provider's variable"
    assert "MY_KEY" not in env and "OPENAI_API_KEY" not in env and "SASHIKO_CONFIG" not in env
    s = call["settings"]
    assert "max_total_tokens = 3000000" in s and "max_total_output_tokens = 300000" in s and 'kind = "kannaka"' in s
    assert KEY not in s and call["settings_mode"] == "0o600"
    jd = world["tmp"] / "jobs" / st["job"]
    assert oct(jd.stat().st_mode & 0o777) == "0o700"
    for f in jd.iterdir():
        assert KEY not in f.read_text(), f"{f.name} holds the key"
        assert oct(f.stat().st_mode & 0o777) == "0o600", f.name
    assert "<key>" in (jd / "stderr.log").read_text()
    res = svc.result(st["job"])
    assert res["ok"] and res["findings_total"] == 2 and res["findings"][0]["severity"] == "High"
    assert "<key>" in res["findings"][0]["problem"] and KEY not in json.dumps(res)
    assert res["findings"][0]["locations"] == [{"file": "src/a.c", "line": 12}]
    assert res["tokens"]["usd_estimate"] == pytest.approx(0.8 * 3 + 0.05 * 15 + 0.2 * 0.3)
    assert "Nothing was posted" in res["note"]


def test_the_budget_abort_and_a_broken_report_are_failures(world):
    svc = world["make"]()
    world["mode"]("budget")
    j = svc.start("proj", "HEAD", "sashiko")["job"]
    assert wait_state(svc, j, sr.FINAL) == "failed"
    assert "max_total_tokens" in svc.status(j)["message"]
    assert svc.result(j)["error"] == "not_done"
    world["mode"]("garbage")
    g = svc.start("proj", "HEAD", "sashiko")["job"]
    assert wait_state(svc, g, sr.FINAL) == "failed" and "exited 3" in svc.status(g)["message"]


def test_no_key_and_an_open_key_file_refuse_to_start(world):
    svc = world["make"](key_env="ABSENT")
    with pytest.raises(sr.Refused) as e:
        svc.start("proj", "HEAD", "sashiko")
    assert e.value.code == "no_key"
    kf = world["tmp"] / "key"
    kf.write_text(KEY + "\n")
    kf.chmod(0o644)
    svc2 = world["make"](key_env=None, key_file=str(kf))
    with pytest.raises(sr.Refused) as e:
        svc2.start("proj", "HEAD", "sashiko")
    assert e.value.code == "key_file_open"
    assert "chmod 600" in svc2.setup()["key"]
    kf.chmod(0o600)
    world["mode"]("clean")
    j = svc2.start("proj", "HEAD", "sashiko")["job"]
    assert wait_state(svc2, j, sr.FINAL) == "done"
    assert world["calls"]()[-1]["env"]["ANTHROPIC_API_KEY"] == KEY and svc2.result(j)["findings"] == []


def test_one_review_at_a_time_and_cancel_stops_it(world):
    world["mode"]("sleep")
    svc = world["make"]()
    a = svc.start("proj", "HEAD", "sashiko")["job"]
    b = svc.start("proj", "HEAD", "sashiko")["job"]
    assert wait_state(svc, a, {"running"}) == "running"
    time.sleep(0.3)
    assert svc.status(b)["state"] == "queued" and len(world["calls"]()) == 1
    assert svc.cancel(b)["state"] == "cancelled"
    t0 = time.monotonic()
    assert svc.cancel(a)["state"] == "cancelled"
    svc.wait_idle(timeout=10)
    assert time.monotonic() - t0 < 10, "the running review was killed, not waited for"
    assert svc.status(a)["state"] == "cancelled" and svc.status(b)["state"] == "cancelled"
    assert len(world["calls"]()) == 1, "the cancelled queued job never ran"
    assert {j["id"] for j in svc.jobs()["jobs"]} == {a, b}


def test_a_restarted_server_never_reruns_a_job(world):
    world["mode"]("clean")
    svc = world["make"]()
    j = svc.start("proj", "HEAD", "sashiko")["job"]
    wait_state(svc, j, sr.FINAL)
    jobs = world["tmp"] / "jobs"
    for jid, state in (("r-aaaaaaaaaaaa", "running"), ("r-bbbbbbbbbbbb", "queued")):
        (jobs / jid).mkdir(mode=0o700)
        (jobs / jid / "job.json").write_text(json.dumps({"id": jid, "state": state, "queued": sr._now(),
                                                         "repo": "proj", "commit": "x" * 40, "project": "sashiko"}))
    svc2 = world["make"]()
    assert svc2.status("r-aaaaaaaaaaaa")["state"] == "interrupted"
    assert "not re-run" in svc2.status("r-aaaaaaaaaaaa")["message"]
    assert svc2.status("r-bbbbbbbbbbbb")["state"] == "interrupted"
    assert "nothing was spent" in svc2.status("r-bbbbbbbbbbbb")["message"]
    assert svc2.status(j)["state"] == "done"
    time.sleep(0.3)
    assert len(world["calls"]()) == 1


def test_over_mcp_stdio(world):
    pytest.importorskip("mcp")
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client

    cfg_path = world["tmp"] / "review.json"
    cfg_path.write_text(json.dumps({**world["cfg"], "jobs_dir": str(world["tmp"] / "jobs")}))
    world["mode"]("ok")
    params = StdioServerParameters(command=sys.executable, args=[str(HERE / "sashiko_review.py"), "serve", "--config",
                                                                 str(cfg_path)],
                                   env={"PATH": os.environ["PATH"], "HOME": str(world["tmp"]), "MY_KEY": KEY})

    async def main():
        async with stdio_client(params) as (r, w), ClientSession(r, w) as s:
            await s.initialize()
            names = sorted(t.name for t in (await s.list_tools()).tools)
            setup = json.loads((await s.call_tool("review_setup", {})).content[0].text)
            started = json.loads((await s.call_tool("review_start", {"repo": "proj", "rev": "HEAD"})).content[0].text)
            for _ in range(200):
                st = json.loads((await s.call_tool("review_status", {"job": started["job"]})).content[0].text)
                if st["state"] in sr.FINAL:
                    break
                await asyncio.sleep(0.05)
            res = json.loads((await s.call_tool("review_result", {"job": started["job"]})).content[0].text)
            return names, setup, res

    names, setup, res = asyncio.run(main())
    assert names == ["review_cancel", "review_jobs", "review_quote", "review_result", "review_setup", "review_start",
                     "review_status"]
    assert setup["key"] == "present" and setup["posts_anywhere"] is False and KEY not in json.dumps(setup)
    assert res["ok"] and res["findings_total"] == 2 and KEY not in json.dumps(res)


def test_example_config_validates_and_cli_check(world, capsys):
    ex = json.loads((HERE / "review.example.json").read_text())
    sr.validate_config(ex)
    cfg_path = world["tmp"] / "review.json"
    cfg_path.write_text(json.dumps({**world["cfg"], "jobs_dir": str(world["tmp"] / "jobs")}))
    os.environ["MY_KEY"] = KEY
    try:
        assert sr.main(["check", "--config", str(cfg_path)]) == 0
    finally:
        del os.environ["MY_KEY"]
    out = json.loads(capsys.readouterr().out)
    assert out["key"] == "present" and out["binary_found"] and KEY not in json.dumps(out)
