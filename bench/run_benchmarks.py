#!/usr/bin/env python3

import argparse
import json
import re
import statistics
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
WREN_BENCH_DIR = ROOT / "vendor" / "wren" / "test" / "benchmark"
REPO_BENCH_DIR = ROOT / "bench"
LUA_BENCH_DIR = REPO_BENCH_DIR / "lua_equivalents"

PAGE_BENCHMARKS = ["method_call", "delta_blue", "binary_trees", "fib"]
REPO_BENCHMARKS = ["bench_sum", "bench_for", "bench_fib"]
BENCHMARKSGAME_BENCHMARKS = [
    "benchmarksgame_fannkuch_redux",
    "benchmarksgame_nbody",
    "benchmarksgame_spectral_norm",
    "benchmarksgame_mandelbrot",
    "benchmarksgame_fasta",
    "benchmarksgame_k_nucleotide",
    "benchmarksgame_reverse_complement",
    "benchmarksgame_binary_trees",
    "benchmarksgame_pidigits",
    "benchmarksgame_regex_redux",
]

# Recursive workloads. None of these contain a traceable loop in the hot path,
# so the JIT compiles zero traces for all of them today.
RECURSION_BENCHMARKS = [
    "bench_fib",
    "bench_tak",
    "bench_ack",
    "bench_mutual",
    "bench_deep",
    "bench_trees",
    "bench_method_call",
]

ELAPSED_RE = re.compile(
    r"elapsed:\s*([0-9]+(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)"
)
TRACE_RE = re.compile(r"Traces compiled: (\d+), aborted: (\d+), exits: (\d+)")
ABORT_RE = re.compile(r"^\[JIT\] abort: (.+)$", re.MULTILINE)


@dataclass
class TrialResult:
    elapsed_seconds: float
    traces_compiled: int
    traces_aborted: int
    total_exits: int
    abort_reasons: list[str]


@dataclass
class Summary:
    label: str
    path: str
    mode: str
    best_seconds: float
    median_seconds: float
    mean_seconds: float
    stdev_seconds: float
    best_trial: int
    traces_compiled: int
    traces_aborted: int
    total_exits: int
    abort_reasons: list[str]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run WrenJIT benchmarks using the benchmark self-times."
    )
    parser.add_argument(
        "--suite",
        choices=["page", "repo", "recursion", "benchmarksgame", "all"],
        default="page",
        help="Named benchmark suite to run when --benchmarks is not set.",
    )
    parser.add_argument(
        "--luajit",
        nargs="?",
        const="luajit",
        default=None,
        metavar="BINARY",
        help=(
            "Also time the matching bench/lua_equivalents/<name>.lua under "
            "LuaJIT, with the JIT both off and on."
        ),
    )
    parser.add_argument(
        "--benchmarks",
        help=(
            "Comma-separated benchmark names or .wren paths. Names are resolved "
            "against vendor/wren/test/benchmark and bench/."
        ),
    )
    parser.add_argument(
        "--binary",
        default=str(ROOT / "build_perf" / "wrenjit_cli"),
        help="Path to the wrenjit_cli binary.",
    )
    parser.add_argument(
        "--trials",
        type=int,
        default=10,
        help="Number of trials per benchmark and mode.",
    )
    parser.add_argument(
        "--modes",
        choices=["both", "jit", "no-jit"],
        default="both",
        help="Which execution modes to run.",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Emit structured JSON instead of a text table.",
    )
    parser.add_argument(
        "--output",
        help="Write output to this file instead of stdout.",
    )
    return parser.parse_args()


def resolve_named_benchmark(name: str) -> tuple[str, Path]:
    candidate = name.strip()
    if not candidate:
        raise ValueError("Benchmark names must not be empty.")

    raw_path = Path(candidate)
    if raw_path.is_absolute() or candidate.endswith(".wren") or "/" in candidate:
        path = raw_path if raw_path.is_absolute() else (ROOT / raw_path)
        if not path.exists():
            raise FileNotFoundError(f"Benchmark not found: {path}")
        return path.stem, path

    for path in (
        WREN_BENCH_DIR / f"{candidate}.wren",
        REPO_BENCH_DIR / f"{candidate}.wren",
        REPO_BENCH_DIR / f"bench_{candidate}.wren",
    ):
        if path.exists():
            return path.stem, path

    raise FileNotFoundError(f"Unknown benchmark '{candidate}'.")


def resolve_benchmarks(args: argparse.Namespace) -> list[tuple[str, Path]]:
    if args.benchmarks:
        names = args.benchmarks.split(",")
        return [resolve_named_benchmark(name) for name in names]

    suite_names: list[str]
    if args.suite == "page":
        suite_names = PAGE_BENCHMARKS
    elif args.suite == "repo":
        suite_names = REPO_BENCHMARKS
    elif args.suite == "recursion":
        suite_names = RECURSION_BENCHMARKS
    elif args.suite == "benchmarksgame":
        suite_names = BENCHMARKSGAME_BENCHMARKS
    else:
        seen: dict[str, None] = {}
        for name in (
            PAGE_BENCHMARKS
            + REPO_BENCHMARKS
            + RECURSION_BENCHMARKS
            + BENCHMARKSGAME_BENCHMARKS
        ):
            seen.setdefault(name, None)
        suite_names = list(seen)

    return [resolve_named_benchmark(name) for name in suite_names]


def selected_modes(mode_arg: str) -> list[tuple[str, str]]:
    if mode_arg == "jit":
        return [("jit", "--jit")]
    if mode_arg == "no-jit":
        return [("no-jit", "--no-jit")]
    return [("no-jit", "--no-jit"), ("jit", "--jit")]


def run_trial(binary: Path, benchmark: Path, flag: str) -> TrialResult:
    process = subprocess.run(
        [str(binary), str(benchmark), flag],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if process.returncode != 0:
        raise RuntimeError(
            f"{benchmark.name} {flag} failed with exit code {process.returncode}\n"
            f"stdout:\n{process.stdout}\n"
            f"stderr:\n{process.stderr}"
        )

    elapsed_match = ELAPSED_RE.search(process.stdout)
    if elapsed_match is None:
        raise RuntimeError(
            f"{benchmark.name} {flag} did not print an elapsed time.\n"
            f"stdout:\n{process.stdout}\n"
            f"stderr:\n{process.stderr}"
        )

    trace_match = TRACE_RE.search(process.stderr)
    if trace_match is None:
        raise RuntimeError(
            f"{benchmark.name} {flag} did not print JIT trace statistics.\n"
            f"stderr:\n{process.stderr}"
        )

    traces_compiled, traces_aborted, total_exits = (
        int(trace_match.group(1)),
        int(trace_match.group(2)),
        int(trace_match.group(3)),
    )

    return TrialResult(
        elapsed_seconds=float(elapsed_match.group(1)),
        traces_compiled=traces_compiled,
        traces_aborted=traces_aborted,
        total_exits=total_exits,
        abort_reasons=sorted(set(ABORT_RE.findall(process.stderr))),
    )


def lua_counterpart(label: str) -> Path | None:
    path = LUA_BENCH_DIR / f"{label}.lua"
    return path if path.exists() else None


def run_lua_trial(binary: str, benchmark: Path, jit_on: bool) -> float:
    command = [binary]
    if not jit_on:
        command.append("-joff")
    command.append(str(benchmark))

    process = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if process.returncode != 0:
        raise RuntimeError(
            f"{benchmark.name} failed with exit code {process.returncode}\n"
            f"stdout:\n{process.stdout}\nstderr:\n{process.stderr}"
        )

    match = ELAPSED_RE.search(process.stdout)
    if match is None:
        raise RuntimeError(
            f"{benchmark.name} did not print an elapsed time.\n"
            f"stdout:\n{process.stdout}"
        )
    return float(match.group(1))


def summarize(label: str, path: Path, mode: str, trials: list[TrialResult]) -> Summary:
    elapsed = [trial.elapsed_seconds for trial in trials]
    best = min(elapsed)
    best_index = elapsed.index(best)
    best_trial = trials[best_index]

    return Summary(
        label=label,
        path=str(path.relative_to(ROOT)),
        mode=mode,
        best_seconds=best,
        median_seconds=statistics.median(elapsed),
        mean_seconds=statistics.mean(elapsed),
        stdev_seconds=statistics.pstdev(elapsed),
        best_trial=best_index + 1,
        traces_compiled=best_trial.traces_compiled,
        traces_aborted=best_trial.traces_aborted,
        total_exits=best_trial.total_exits,
        abort_reasons=best_trial.abort_reasons,
    )


def speedup(no_jit: Summary, jit: Summary) -> float:
    return no_jit.best_seconds / jit.best_seconds


def render_text(
    results: dict[str, dict[str, Summary]],
    lua_results: dict[str, dict[str, float]] | None = None,
) -> None:
    lua_results = lua_results or {}
    lua_header = (
        f" {'lj-off ms':>10s} {'lj-on ms':>10s} {'vs lj-on':>9s}"
        if lua_results
        else ""
    )
    header = (
        f"{'benchmark':18s} {'no-jit ms':>10s} {'jit ms':>10s} "
        f"{'speedup':>8s} {'compiled':>8s} {'aborted':>8s} {'exits':>8s}"
        f"{lua_header}"
    )
    print(header)
    print("-" * len(header))

    for label, modes in results.items():
        no_jit = modes.get("no-jit")
        jit = modes.get("jit")
        best = jit or no_jit

        if no_jit and jit:
            row = (
                f"{label:18s} "
                f"{no_jit.best_seconds * 1000:10.3f} "
                f"{jit.best_seconds * 1000:10.3f} "
                f"{speedup(no_jit, jit):8.2f} "
                f"{jit.traces_compiled:8d} "
                f"{jit.traces_aborted:8d} "
                f"{jit.total_exits:8d}"
            )
        else:
            row = (
                f"{label:18s} "
                f"{'--':>10s} "
                f"{best.best_seconds * 1000:10.3f} "
                f"{'--':>8s} "
                f"{best.traces_compiled:8d} "
                f"{best.traces_aborted:8d} "
                f"{best.total_exits:8d}"
            )

        lua = lua_results.get(label)
        if lua_results:
            if lua:
                ratio = best.best_seconds / lua["on"]
                row += (
                    f" {lua['off'] * 1000:10.3f} {lua['on'] * 1000:10.3f} "
                    f"{ratio:8.2f}x"
                )
            else:
                row += f" {'--':>10s} {'--':>10s} {'--':>9s}"
        print(row)

        for mode_name in ("jit", "no-jit"):
            summary = modes.get(mode_name)
            if summary is None or not summary.abort_reasons:
                continue
            reasons = ", ".join(summary.abort_reasons)
            print(f"  {mode_name:16s} abort reasons: {reasons}")


def render_json(
    results: dict[str, dict[str, Summary]],
    lua_results: dict[str, dict[str, float]] | None = None,
) -> None:
    lua_results = lua_results or {}
    payload = []
    for label, modes in results.items():
        entry = {"benchmark": label}
        for mode_name, summary in modes.items():
            entry[mode_name] = asdict(summary)
        if "no-jit" in modes and "jit" in modes:
            entry["speedup"] = speedup(modes["no-jit"], modes["jit"])
        if label in lua_results:
            entry["luajit"] = lua_results[label]
        payload.append(entry)
    json.dump(payload, sys.stdout, indent=2)
    print()


def main() -> int:
    args = parse_args()
    binary = Path(args.binary)
    if not binary.exists():
        print(f"Binary not found: {binary}", file=sys.stderr)
        return 1
    if args.trials < 1:
        print("--trials must be >= 1", file=sys.stderr)
        return 1

    results: dict[str, dict[str, Summary]] = {}
    lua_results: dict[str, dict[str, float]] = {}
    benchmarks = resolve_benchmarks(args)

    for label, path in benchmarks:
        mode_map: dict[str, Summary] = {}
        results[label] = mode_map
        for mode_name, flag in selected_modes(args.modes):
            trials = [run_trial(binary, path, flag) for _ in range(args.trials)]
            mode_map[mode_name] = summarize(label, path, mode_name, trials)

        if args.luajit is None:
            continue
        lua_path = lua_counterpart(label)
        if lua_path is None:
            continue
        lua_results[label] = {
            "off": min(
                run_lua_trial(args.luajit, lua_path, False)
                for _ in range(args.trials)
            ),
            "on": min(
                run_lua_trial(args.luajit, lua_path, True)
                for _ in range(args.trials)
            ),
        }

    if args.output:
        output = Path(args.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        original_stdout = sys.stdout
        try:
            with output.open("w") as stream:
                sys.stdout = stream
                if args.json:
                    render_json(results, lua_results)
                else:
                    render_text(results, lua_results)
        finally:
            sys.stdout = original_stdout
        print(f"wrote {output}", file=sys.stderr)
    elif args.json:
        render_json(results, lua_results)
    else:
        render_text(results, lua_results)

    return 0


if __name__ == "__main__":
    sys.exit(main())
