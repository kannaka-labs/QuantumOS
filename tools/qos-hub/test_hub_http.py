"""Tests for HTTP MCP servers behind the hub (#297 slice 2). The fake server answers plain
JSON-RPC over POST and checks a bearer token, the way Agent-kax's /api/mcp does."""
from __future__ import annotations

import asyncio
import json
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import qos_hub as hub  # noqa: E402
import provider_broker as pb  # noqa: E402

TOKEN = "test-token-not-a-secret"


class FakeCity(BaseHTTPRequestHandler):
    calls: list = []

    def log_message(self, *a):
        pass

    def _send(self, code, obj=None):
        body = b"" if obj is None else json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if self.headers.get("Authorization") != f"Bearer {TOKEN}":
            return self._send(401, {"jsonrpc": "2.0", "id": req.get("id"), "error": {"code": -32001, "message": "auth"}})
        m, rid = req.get("method"), req.get("id")
        if m == "initialize":
            return self._send(200, {"jsonrpc": "2.0", "id": rid, "result": {
                "protocolVersion": "2025-06-18", "capabilities": {"tools": {}},
                "serverInfo": {"name": "fake-city", "version": "1"}}})
        if m and m.startswith("notifications/"):
            return self._send(202)
        if m == "tools/list":
            return self._send(200, {"jsonrpc": "2.0", "id": rid, "result": {"tools": [
                {"name": "city_look", "description": "Look around.", "inputSchema": {"type": "object"}},
                {"name": "city_say", "description": "Say something in public.",
                 "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}}}}]}})
        if m == "tools/call":
            FakeCity.calls.append(req["params"]["name"])
            return self._send(200, {"jsonrpc": "2.0", "id": rid, "result": {
                "content": [{"type": "text", "text": json.dumps({"you_see": "the plaza", **req["params"].get("arguments", {})})}],
                "isError": False}})
        return self._send(200, {"jsonrpc": "2.0", "id": rid, "error": {"code": -32601, "message": "no"}})


@pytest.fixture
def city():
    FakeCity.calls = []
    srv = ThreadingHTTPServer(("127.0.0.1", 0), FakeCity)
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    yield f"http://127.0.0.1:{srv.server_address[1]}/api/mcp"
    srv.shutdown()
    srv.server_close()


def test_bearer_from_file_or_env(tmp_path, monkeypatch):
    f = tmp_path / "t.txt"
    f.write_text(TOKEN + "\n")
    assert hub.http_headers({"auth": {"bearer_file": str(f)}}) == {"Authorization": f"Bearer {TOKEN}"}
    monkeypatch.setenv("QOS_TEST_TOKEN", TOKEN)
    assert hub.http_headers({"auth": {"bearer_env": "QOS_TEST_TOKEN"}})["Authorization"].endswith(TOKEN)
    assert hub.http_headers({}) == {}
    for bad in ({"auth": {"bearer_file": str(tmp_path / "missing")}}, {"auth": {"bearer_env": "QOS_NOT_SET_X"}},
                {"auth": {"password": "x"}}):
        with pytest.raises(ValueError) as e:
            hub.http_headers(bad)
        assert TOKEN not in str(e.value)


def run_hub(cfg, db, script):
    pytest.importorskip("mcp")
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client

    async def go():
        params = StdioServerParameters(command=sys.executable,
                                       args=[str(HERE / "qos_hub.py"), "serve", "--config", str(cfg), "--db", db],
                                       env={**os.environ})
        async with stdio_client(params) as (r, w), ClientSession(r, w) as s:
            await s.initialize()

            async def c(name, **a):
                return json.loads((await s.call_tool(name, a)).content[0].text)
            return await script(s, c)
    return asyncio.run(go())


def test_http_server_behind_the_hub(city, tmp_path):
    tok = tmp_path / "tok.txt"
    tok.write_text(TOKEN)
    db = str(tmp_path / "pb.db")
    key = hub.HubLedger(db).broker.set_envelope(pb.Envelope(1, 1, ["storage"], 3600))
    cfg = tmp_path / "hub.json"
    cfg.write_text(json.dumps({"servers": {"city": {"url": city, "auth": {"bearer_file": str(tok)}}},
                               "policy": {"city.city_look": "allow", "city.city_say": "ask"}}))

    async def script(s, c):
        names = sorted(t.name for t in (await s.list_tools()).tools)
        assert "city.city_look" in names and "city.city_say" in names
        assert (await c("city.city_look"))["result"]["you_see"] == "the plaza"
        ask = await c("city.city_say", text="hello plaza")
        assert ask["error"] == "approval_required"
        hub.HubLedger(db).decide(ask["approval"], key)
        assert (await c("hub_call_approved", approval=ask["approval"]))["result"]["text"] == "hello plaza"
    run_hub(cfg, db, script)
    assert FakeCity.calls == ["city_look", "city_say"]


def test_a_wrong_token_is_logged_not_fatal(city, tmp_path):
    tok = tmp_path / "tok.txt"
    tok.write_text("wrong-token")
    db = str(tmp_path / "pb.db")
    cfg = tmp_path / "hub.json"
    cfg.write_text(json.dumps({"servers": {"city": {"url": city, "auth": {"bearer_file": str(tok)}},
                                           "cli": {"cli": {"binary": sys.executable, "tools": {
                                               "py": {"description": "py version", "argv": ["--version"]}}}}},
                               "policy": {"cli.py": "allow"}}))

    async def script(s, c):
        names = sorted(t.name for t in (await s.list_tools()).tools)
        assert not any(n.startswith("city.") for n in names) and "cli.py" in names
        led = await c("hub_ledger", limit=10)
        errs = [e for e in led["events"] if e["event"] == "hub_server_error"]
        assert errs and errs[0]["server"] == "city" and "wrong-token" not in json.dumps(led)
    run_hub(cfg, db, script)
    assert FakeCity.calls == []


class RotatingCity(FakeCity):
    """Only the most recently issued token is valid; POST /refresh rotates it."""
    current = TOKEN
    issued = 0

    def do_POST(self):
        if self.path.endswith("/refresh"):
            self.rfile.read(int(self.headers.get("Content-Length") or 0))
            if self.headers.get("Authorization") != f"Bearer {RotatingCity.current}":
                return self._send(401, {"error": "expired"})
            RotatingCity.issued += 1
            RotatingCity.current = f"rotated-{RotatingCity.issued}"
            return self._send(200, {"token": RotatingCity.current, "expiresInSec": 900})
        global TOKEN
        saved, TOKEN = TOKEN, RotatingCity.current
        try:
            return super().do_POST()
        finally:
            TOKEN = saved


def test_a_short_lived_token_is_refreshed_on_the_open_session(tmp_path):
    RotatingCity.current, RotatingCity.issued, FakeCity.calls = TOKEN, 0, []
    srv = ThreadingHTTPServer(("127.0.0.1", 0), RotatingCity)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{srv.server_address[1]}"
    tok = tmp_path / "tok.txt"
    tok.write_text(TOKEN)
    db = str(tmp_path / "pb.db")
    cfg = tmp_path / "hub.json"
    cfg.write_text(json.dumps({"servers": {"city": {"url": base + "/api/mcp", "auth": {
        "bearer_file": str(tok), "refresh_url": base + "/api/auth/token/refresh", "refresh_every_s": 0.5}}},
        "policy": {"city.city_look": "allow"}}))

    async def script(s, c):
        assert (await c("city.city_look"))["ok"]
        await asyncio.sleep(1.6)                    # at least two rotations: the first token is dead now
        assert RotatingCity.issued >= 2
        assert (await c("city.city_look"))["ok"], "the open session must use the refreshed token"
        led = await c("hub_ledger", limit=20)
        refreshes = [e for e in led["events"] if e["event"] == "hub_token_refresh"]
        assert refreshes and all(e["ok"] for e in refreshes)
        assert "rotated-" not in json.dumps(led) and TOKEN not in json.dumps(led), "tokens never in the ledger"
    try:
        run_hub(cfg, db, script)
    finally:
        srv.shutdown()
        srv.server_close()
    assert tok.read_text().startswith("rotated-"), "the new token is written back to the file"
    assert oct(tok.stat().st_mode & 0o777) == "0o600"
    assert FakeCity.calls == ["city_look", "city_look"]


SECRET = "test-client-secret-not-real"


class CredentialCity(FakeCity):
    """A server with the Command Center's shape: POST /api/public/auth/agent/token takes an
    OAuth 2.1 client_credentials form and issues a fresh short-lived token; /api/mcp accepts
    only the most recently issued one."""
    current = None
    issued = 0
    grants: list = []

    def do_POST(self):
        if self.path.endswith("/auth/agent/token"):
            from urllib.parse import parse_qs
            form = {k: v[0] for k, v in parse_qs(self.rfile.read(int(self.headers.get("Content-Length") or 0)).decode()).items()}
            CredentialCity.grants.append({k: v for k, v in form.items() if k != "client_secret"})
            if form.get("grant_type") != "client_credentials":
                return self._send(400, {"error": "unsupported_grant_type"})
            if form.get("client_id") != "hub-test" or form.get("client_secret") != SECRET:
                return self._send(401, {"error": "invalid_client", "error_description": "Bad secret"})
            CredentialCity.issued += 1
            CredentialCity.current = f"issued-{CredentialCity.issued}"
            return self._send(200, {"access_token": CredentialCity.current, "token_type": "Bearer",
                                    "expires_in": 900, "scope": form.get("scope", "mcp:read")})
        global TOKEN
        saved, TOKEN = TOKEN, CredentialCity.current
        try:
            return super().do_POST()
        finally:
            TOKEN = saved


@pytest.fixture
def credential_city():
    CredentialCity.current, CredentialCity.issued, CredentialCity.grants, FakeCity.calls = None, 0, [], []
    srv = ThreadingHTTPServer(("127.0.0.1", 0), CredentialCity)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    yield f"http://127.0.0.1:{srv.server_address[1]}"
    srv.shutdown()
    srv.server_close()


def test_client_credentials_grant_is_run_at_start_and_renewed(credential_city, tmp_path):
    base = credential_city
    sec = tmp_path / "secret.txt"
    sec.write_text(SECRET + "\n")
    db = str(tmp_path / "pb.db")
    cfg = tmp_path / "hub.json"
    cfg.write_text(json.dumps({"servers": {"cc": {"url": base + "/api/mcp", "auth": {
        "token_url": base + "/api/public/auth/agent/token", "client_id": "hub-test",
        "client_secret_file": str(sec), "scope": "mcp:read", "refresh_every_s": 0.5}}},
        "policy": {"cc.city_look": "allow"}}))

    async def script(s, c):
        assert (await c("cc.city_look"))["ok"]
        await asyncio.sleep(1.6)                    # at least two renewals: the first token is dead now
        assert CredentialCity.issued >= 3
        assert (await c("cc.city_look"))["ok"], "the open session must use the renewed token"
        led = await c("hub_ledger", limit=20)
        refreshes = [e for e in led["events"] if e["event"] == "hub_token_refresh"]
        assert refreshes and all(e["ok"] for e in refreshes)
        blob = json.dumps(led)
        assert SECRET not in blob and "issued-" not in blob, "neither the secret nor a token in the ledger"
    run_hub(cfg, db, script)
    assert FakeCity.calls == ["city_look", "city_look"]
    assert all(g == {"grant_type": "client_credentials", "client_id": "hub-test", "scope": "mcp:read"}
               for g in CredentialCity.grants), CredentialCity.grants


def test_a_refused_grant_names_the_error_code_not_the_secret(credential_city, tmp_path, monkeypatch):
    base = credential_city
    monkeypatch.setenv("QOS_TEST_CC_SECRET", "the-wrong-secret")
    with pytest.raises(ValueError) as e:
        hub.http_headers({"auth": {"token_url": base + "/api/public/auth/agent/token", "client_id": "hub-test",
                                   "client_secret_env": "QOS_TEST_CC_SECRET"}})
    assert "401 invalid_client" in str(e.value) and "wrong-secret" not in str(e.value)
    for bad in ({"auth": {"token_url": base + "/x", "client_id": "hub-test"}},
                {"auth": {"token_url": base + "/x", "client_secret_env": "QOS_TEST_CC_SECRET"}},
                {"auth": {"token_url": base + "/x", "client_id": "hub-test", "client_secret_file": str(tmp_path / "missing")}}):
        with pytest.raises(ValueError) as e:
            hub.http_headers(bad)
        assert "wrong-secret" not in str(e.value)
