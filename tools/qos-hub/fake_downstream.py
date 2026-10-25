#!/usr/bin/env python3
"""A downstream MCP server for the hub's tests: one read tool, one write tool and one tool
that "spends". It appends every call it receives to the file in $FAKE_CALLS, so a test can
prove a call was (or was not) forwarded."""
import json
import os

from mcp.server.fastmcp import FastMCP

mcp = FastMCP("fake")


def _seen(name, **args):
    with open(os.environ["FAKE_CALLS"], "a") as f:
        f.write(json.dumps({"tool": name, **args}) + "\n")


@mcp.tool()
def recall(query: str, top_k: int = 3) -> dict:
    """Search memories."""
    _seen("recall", query=query, top_k=top_k)
    return {"memories": [f"memory about {query}"][:top_k]}


@mcp.tool()
def swarm_send(text: str) -> dict:
    """Publish to the shared swarm bus."""
    _seen("swarm_send", text=text)
    return {"published": text}


@mcp.tool()
def compute_grant(machine: str, credits: int) -> dict:
    """Spend credits."""
    _seen("compute_grant", machine=machine, credits=credits)
    return {"granted": credits}


if __name__ == "__main__":
    mcp.run()
