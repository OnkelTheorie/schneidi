#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
"""Size of what the MCP server hands an AI before it does anything: the tools/list answer (per tool and in total)
and the instructions of initialize. Tokens are estimated as characters / 4 of the compact JSON.

    tools/mcp_tool_sizes.py [path/to/schneidi-cli] [--json]

The test test_mcp_tool_size keeps the total under its budget; this script is for looking at the numbers.
"""
import json
import os
import subprocess
import sys


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    exe = args[0] if args else os.path.join(os.path.dirname(__file__), "..", "build", "schneidi-cli")
    msgs = [
        {"jsonrpc": "2.0", "id": 1, "method": "initialize",
         "params": {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "size", "version": "1"}}},
        {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
    ]
    stdin = "".join(json.dumps(m) + "\n" for m in msgs)
    env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
    out = subprocess.run([exe, "mcp"], input=stdin, capture_output=True, text=True, env=env, timeout=60).stdout
    answers = {}
    for line in out.splitlines():
        try:
            m = json.loads(line)
        except ValueError:
            continue
        if "id" in m:
            answers[m["id"]] = m.get("result", {})
    tools = answers[2]["tools"]
    instructions = answers[1].get("instructions", "")

    def size(v):
        return len(json.dumps(v, ensure_ascii=False, separators=(",", ":")))

    rows = []
    for t in tools:
        props = t.get("inputSchema", {}).get("properties", {})
        params = sum(len(p.get("description", "")) for p in props.values())
        rows.append((t["name"], size(t), len(t.get("description", "")), params, len(props)))
    total = size({"tools": tools})
    if "--json" in sys.argv:
        print(json.dumps({"total": total, "instructions": len(instructions),
                          "tools": {r[0]: r[1] for r in rows}}, indent=1))
        return
    print(f"{'tool':18} {'chars':>6} {'~tok':>5} {'desc':>5} {'ptext':>6} {'params':>6}")
    for name, n, d, p, k in sorted(rows, key=lambda r: -r[1]):
        print(f"{name:18} {n:6} {n // 4:5} {d:5} {p:6} {k:6}")
    print(f"{'tools/list total':18} {total:6} {total // 4:5}   ({len(tools)} tools)")
    print(f"{'instructions':18} {len(instructions):6} {len(instructions) // 4:5}")


if __name__ == "__main__":
    main()
