#!/usr/bin/env python3
"""A stand-in for `sashiko review` in the tests: no model, no tokens. It records what it was given (argv, working
directory, environment, the settings file) to calls.jsonl beside itself, and behaves as the file `mode` beside
itself says: ok (a report with findings, exit 1 as Sashiko does when it has them), clean (exit 0), budget (Sashiko's
token-budget abort), garbage (not JSON), sleep (runs until killed)."""
import json
import os
import sys
import time
from pathlib import Path

here = Path(__file__).resolve().parent
mode = (here / "mode").read_text().strip() if (here / "mode").exists() else "ok"
argv = sys.argv[1:]
settings = argv[argv.index("--settings") + 1] if "--settings" in argv else None
with open(here / "calls.jsonl", "a") as f:
    f.write(json.dumps({"argv": argv, "cwd": os.getcwd(), "env": dict(os.environ),
                        "settings": Path(settings).read_text() if settings else None,
                        "settings_mode": oct(os.stat(settings).st_mode & 0o777) if settings else None}) + "\n")
key = os.environ.get("ANTHROPIC_API_KEY", "")
if mode == "sleep":
    time.sleep(120)
if mode == "budget":
    sys.stderr.write("Error: Token budget exceeded: 3000001 uncached input + output tokens used > 3000000 limit\n")
    sys.exit(3)
if mode == "garbage":
    print("this is not json")
    sys.exit(3)
sys.stderr.write(f"debug: talking to the provider with {key}\n")      # a leaky log line, to prove the redaction
findings = [] if mode == "clean" else [
    {"severity": "High", "problem": f"the handler echoes the key {key} into a log", "severity_explanation": "leak",
     "stage": "security", "locations": [{"file": "src/a.c", "line": 12, "code_snippet": "x"}]},
    {"severity": "Low", "problem": "an off-by-one in the loop bound", "severity_explanation": "minor",
     "stage": "implementation", "locations": [{"file": "src/b.c", "line": 40}]}]
print(json.dumps({"tokens_in": 1_000_000, "tokens_out": 50_000, "tokens_cached": 200_000,
                  "review": {"summary": "two concerns", "findings": findings}}))
sys.exit(1 if findings else 0)
