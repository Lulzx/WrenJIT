#!/usr/bin/env python3
"""Render the JSON from run_benchmarks.py as an HTML report.

    python3 bench/run_benchmarks.py --suite recursion --luajit --json > results.json
    python3 bench/gen_report.py results.json --output docs/index.html

Styling follows https://wren.io/performance.html so the two read as a pair.
"""

import argparse
import html
import json
import platform
import subprocess
import sys
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Bar colours. Wren keeps the wren.io blue; LuaJIT gets a contrasting hue so a
# reader can tell the two implementations apart without reading the labels.
SERIES = [
    ("wren_jit", "wren (jit)", "wren-jit"),
    ("wren_nojit", "wren (no jit)", "wren"),
    ("luajit_off", "luajit (-joff)", "luajit-off"),
    ("luajit_on", "luajit", "luajit"),
]

DESCRIPTIONS = {
    "bench_fib": "Naive binary recursion, <code>fib(35)</code>. No loop in the hot path.",
    "bench_tak": "Takeuchi function, <code>tak(24, 16, 8)</code>. Three-argument recursion, no allocation.",
    "bench_ack": "Ackermann, <code>ack(3, 7)</code>. Deep recursion, little work per frame.",
    "bench_mutual": "Two static methods calling each other 500 deep, 2000 times.",
    "bench_deep": "Linear recursion 1000 frames deep, 2000 times. Isolates call and return cost.",
    "bench_trees": "Recursion plus allocation: builds and walks depth-14 trees 12 times.",
    "bench_method_call": "Three million method calls on a user-defined class. A loop, not recursion.",
    "bench_sum": "Sums 0..999999 in a <code>while</code> loop.",
    "bench_for": "Sums 1..1000000 with <code>for i in 1..n</code>.",
}


def tool_version(command: list[str], fallback: str) -> str:
    try:
        out = subprocess.run(
            command, capture_output=True, text=True, timeout=10
        )
        text = (out.stdout + out.stderr).strip().splitlines()
        return text[0] if text else fallback
    except (OSError, subprocess.SubprocessError):
        return fallback


def collect(entry: dict) -> dict[str, float]:
    """Pull the four comparable timings out of one benchmark entry."""
    values: dict[str, float] = {}
    if "jit" in entry:
        values["wren_jit"] = entry["jit"]["best_seconds"]
    if "no-jit" in entry:
        values["wren_nojit"] = entry["no-jit"]["best_seconds"]
    lua = entry.get("luajit")
    if lua:
        values["luajit_off"] = lua["off"]
        values["luajit_on"] = lua["on"]
    return values


def render_chart(entry: dict) -> str:
    values = collect(entry)
    if not values:
        return ""

    longest = max(values.values())
    rows = []
    for key, label, css in SERIES:
        if key not in values:
            continue
        seconds = values[key]
        width = max(seconds / longest * 100.0, 0.6)
        rows.append(
            '  <tr>\n'
            f'    <th>{html.escape(label)}</th>'
            f'<td><div class="chart-bar {css}" style="width: {width:.1f}%;">'
            f'{seconds * 1000:.1f}ms&nbsp;</div></td>\n'
            '  </tr>'
        )
    return '<table class="chart">\n' + "\n".join(rows) + "\n</table>"


def render_note(entry: dict) -> str:
    jit = entry.get("jit")
    if not jit:
        return ""

    compiled = jit.get("traces_compiled", 0)
    aborted = jit.get("traces_aborted", 0)
    reasons = jit.get("abort_reasons") or []

    if compiled == 0 and aborted == 0:
        body = "The JIT compiled no traces: nothing in the hot path is a loop it can record."
    elif compiled == 0:
        listed = ", ".join(f"<code>{html.escape(r)}</code>" for r in reasons)
        body = (
            f"The JIT compiled no traces and aborted {aborted} recording "
            f"attempt{'s' if aborted != 1 else ''}"
        )
        body += f" ({listed})." if listed else "."
    else:
        body = (
            f"The JIT compiled {compiled} trace{'s' if compiled != 1 else ''}"
        )
        if aborted:
            body += f" and aborted {aborted}"
        body += "."
    return f'<p class="note">{body}</p>'


def ratio_table(entries: list[dict]) -> str:
    rows = []
    for entry in entries:
        values = collect(entry)
        best_wren = min(
            (values[k] for k in ("wren_jit", "wren_nojit") if k in values),
            default=None,
        )
        if best_wren is None or "luajit_on" not in values:
            continue
        label = entry["benchmark"]
        rows.append(
            f"<tr><td><code>{html.escape(label)}</code></td>"
            f"<td>{best_wren * 1000:.1f}</td>"
            f"<td>{values['luajit_off'] * 1000:.1f}</td>"
            f"<td>{values['luajit_on'] * 1000:.1f}</td>"
            f"<td>{best_wren / values['luajit_off']:.1f}&times;</td>"
            f"<td>{best_wren / values['luajit_on']:.1f}&times;</td></tr>"
        )
    if not rows:
        return ""
    return (
        '<table class="summary">\n'
        "<tr><th>benchmark</th><th>wren ms</th><th>luajit -joff ms</th>"
        "<th>luajit ms</th><th>vs -joff</th><th>vs luajit</th></tr>\n"
        + "\n".join(rows)
        + "\n</table>"
    )


STYLE = """
:root {
  --header: "Sanchez", helvetica, arial, sans-serif;
  --body: "Source Sans Pro", georgia, serif;
  --code: "Source Code Pro", Menlo, Monaco, Consolas, monospace;
  --light: hsl(0, 0%, 100%);
  --text: #333333;
  --gray-10: #ebebec;
  --gray-20: #d7d8da;
  --gray-80: #60666a;
  --link: hsl(200, 60%, 50%);
  --link-dark: hsl(210, 60%, 20%);
  --wren: #1d5176;
  --luajit: hsl(28, 75%, 52%);
  --luajit-off: hsl(28, 45%, 72%);
}
* { box-sizing: border-box; }
body {
  background-color: var(--light);
  color: var(--text);
  font: 16px/25px var(--body);
  margin: 0;
  padding: 0 24px 80px 24px;
}
.page { max-width: 720px; margin: 0 auto; }
h1 { padding-top: 40px; font: 500 36px/48px var(--header); color: var(--link); margin: 0 0 4px 0; }
h2 { font-weight: 500; font-size: 24px; font-family: var(--header); margin: 40px 0 0 0; color: var(--link); }
h3 { font: 20px var(--body); margin: 28px 0 0 0; color: var(--link); }
p { margin: 12px 0 0 0; }
code { font: 14px var(--code); background: #f7f7f8; padding: 1px 4px; border-radius: 2px; }
a { color: var(--link); text-decoration: none; }
a:hover { text-decoration: underline; }
.subtitle { color: var(--gray-80); font-size: 17px; margin-top: 0; }
.note { font-size: 14px; color: var(--gray-80); margin: 6px 0 0 25px; }
table.chart { margin: 4px 0 0 0; padding: 5px 0 5px 25px; border-collapse: collapse; width: 100%; }
table.chart td, table.chart th { line-height: 14px; margin: 0; padding: 1px 0; }
table.chart th { font-size: 14px; width: 118px; text-align: left; font-weight: normal; color: var(--gray-80); }
table.chart .chart-bar {
  display: inline-block;
  font: 13px var(--body);
  color: var(--light);
  background: var(--link);
  border-bottom: solid 1px var(--link-dark);
  text-align: right;
  border-radius: 2px;
  white-space: nowrap;
  min-width: 54px;
}
table.chart .chart-bar.wren { background: var(--wren); }
table.chart .chart-bar.wren-jit { background: var(--link); }
table.chart .chart-bar.luajit { background: var(--luajit); border-bottom-color: hsl(28, 75%, 30%); }
table.chart .chart-bar.luajit-off { background: var(--luajit-off); border-bottom-color: hsl(28, 45%, 45%); color: #4a3520; }
table.summary { border-collapse: collapse; margin-top: 14px; font-size: 14px; width: 100%; }
table.summary th, table.summary td { text-align: right; padding: 5px 10px; border-bottom: 1px solid var(--gray-10); }
table.summary th:first-child, table.summary td:first-child { text-align: left; }
table.summary th { color: var(--gray-80); font-weight: 600; }
.meta { font-size: 14px; color: var(--gray-80); border-top: 1px solid var(--gray-20); margin-top: 48px; padding-top: 12px; }
.legend { font-size: 14px; color: var(--gray-80); margin-top: 8px; }
.swatch { display: inline-block; width: 11px; height: 11px; border-radius: 2px; margin: 0 4px 0 12px; vertical-align: -1px; }
.swatch:first-child { margin-left: 0; }
@media (max-width: 560px) {
  table.chart th { width: 96px; font-size: 13px; }
}
"""


def render(entries: list[dict], cpu: str, when: str) -> str:
    sections = []
    for entry in entries:
        label = entry["benchmark"]
        chart = render_chart(entry)
        if not chart:
            continue
        desc = DESCRIPTIONS.get(label, "")
        parts = [f"<h3>{html.escape(label)}</h3>"]
        if desc:
            parts.append(f"<p>{desc}</p>")
        parts.append(chart)
        note = render_note(entry)
        if note:
            parts.append(note)
        sections.append("\n".join(parts))

    wren_version = tool_version(["git", "-C", str(ROOT / "vendor" / "wren"),
                                 "rev-parse", "--short", "HEAD"], "unknown")
    luajit_version = tool_version(["luajit", "-v"], "LuaJIT (not found)")

    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>WrenJIT Performance</title>
<link href="https://fonts.googleapis.com/css?family=Source+Sans+Pro:400,600|Source+Code+Pro:400|Sanchez:400" rel="stylesheet">
<style>{STYLE}</style>
</head>
<body>
<div class="page">

<h1>WrenJIT Performance</h1>
<p class="subtitle">A tracing JIT for Wren, measured against LuaJIT. Shorter bars are better.</p>

<p>Every benchmark below exists twice: once in Wren, once in Lua, written to do
the same work in the same shape. Both print their own elapsed time, so process
startup and JIT compilation stay out of the numbers. Each bar is the best of
several runs.</p>

<p class="legend">
<span class="swatch" style="background: var(--link)"></span>wren (jit)
<span class="swatch" style="background: var(--wren)"></span>wren (no jit)
<span class="swatch" style="background: var(--luajit-off)"></span>luajit (-joff)
<span class="swatch" style="background: var(--luajit)"></span>luajit
</p>

<h2>Where things stand</h2>

<p>The JIT records traces from loops. It starts counting at the <code>LOOP</code>
bytecode, records one iteration, and compiles it. Recursion never reaches that
counter, so on the recursive benchmarks the JIT compiles nothing at all and Wren
runs at plain interpreter speed. Those rows are the gap worth closing.</p>

{ratio_table(entries)}

<h2>Benchmarks</h2>

{(chr(10) * 2).join(sections)}

<h2>Where the time goes</h2>

<p>Sampling the interpreter during <code>fib</code> puts about 82% of samples in
<code>runInterpreter</code> itself, with the rest spread across
<code>validateNum</code>, <code>prim_num_lt</code>, <code>prim_num_minus</code>,
<code>prim_num_plus</code>, and <code>wrenEnsureStack</code>.</p>

<p>That distribution follows from how Wren dispatches arithmetic. An expression
like <code>n - 1</code> compiles to <code>CALL_1</code>, which looks up the
receiver's class, bounds-checks the method table, switches on the method type,
calls a primitive through a function pointer, and validates its argument. LuaJIT's
interpreter does a type check and the subtraction inline. The same structure that
makes Wren's method dispatch uniform is what the JIT exists to remove, and on
recursive code it currently removes none of it.</p>

<div class="meta">
<p>Measured on {html.escape(cpu)} on {html.escape(when)}.<br>
Wren base <code>{html.escape(wren_version)}</code>, {html.escape(luajit_version)}.<br>
Generated by <code>bench/gen_report.py</code> from
<code>bench/run_benchmarks.py --json</code>.
Source: <a href="https://github.com/Lulzx/WrenJIT">github.com/Lulzx/WrenJIT</a>.</p>
</div>

</div>
</body>
</html>
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", help="JSON from run_benchmarks.py --json")
    parser.add_argument("--output", help="Write here instead of stdout")
    args = parser.parse_args()

    entries = json.loads(Path(args.results).read_text())
    cpu = tool_version(["sysctl", "-n", "machdep.cpu.brand_string"],
                       platform.processor() or "unknown CPU")
    page = render(entries, cpu, date.today().isoformat())

    if args.output:
        out = Path(args.output)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(page)
        print(f"wrote {out}", file=sys.stderr)
    else:
        sys.stdout.write(page)
    return 0


if __name__ == "__main__":
    sys.exit(main())
