#!/usr/bin/env python3
"""
Measure instructions per cycle (IPC) for COB workloads with Linux perf.

Examples:
  benchmarks/ipc_benchmark.py
  benchmarks/ipc_benchmark.py --workload build --runs 10 --export-markdown benchmarks/ipc.md
  benchmarks/ipc_benchmark.py -- /path/to/cob -C benchmarks/heavy_repo -t compdb
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import shlex
import statistics
import subprocess
import sys
import tempfile
from typing import Sequence


SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent
DEFAULT_REPO = SCRIPT_DIR / "heavy_repo"
DEFAULT_COB = PROJECT_ROOT / "build" / "common-ccache-release" / "cob"
PERF_EVENTS = ("instructions", "cycles")


def normalize_perf_event(event: str) -> str | None:
    for expected in PERF_EVENTS:
        if event == expected or event.endswith(f"/{expected}/"):
            return expected
    return None


def find_cob() -> str:
    env_path = os.environ.get("COB_PATH")
    if env_path:
        return env_path
    if DEFAULT_COB.exists():
        return str(DEFAULT_COB)
    path_cob = shutil.which("cob")
    if path_cob:
        return path_cob
    raise SystemExit(
        "Could not find cob. Set COB_PATH or build build/common-ccache-release/cob."
    )


def ensure_heavy_repo(repo: Path) -> None:
    manifest = repo / "catalyst.build"
    if manifest.exists():
        return
    if repo != DEFAULT_REPO:
        raise SystemExit(f"{repo} does not contain catalyst.build")

    sys.path.insert(0, str(SCRIPT_DIR))
    import generate_heavy_repo

    original_cwd = Path.cwd()
    try:
        os.chdir(PROJECT_ROOT)
        generate_heavy_repo.ensure_dirs()
        print("Generating heavy_repo headers...")
        generate_heavy_repo.generate_headers()
        print("Generating heavy_repo sources...")
        src_info = generate_heavy_repo.generate_sources()
        srcs = [name for name, _ in src_info]
        print("Generating heavy_repo manifests...")
        generate_heavy_repo.generate_catalyst_manifest(srcs)
        generate_heavy_repo.generate_estimates(src_info)
        generate_heavy_repo.generate_ninja_manifest(srcs)
        generate_heavy_repo.generate_makefile(srcs)
    finally:
        os.chdir(original_cwd)


def default_command(workload: str, cob: str, repo: Path) -> list[str]:
    base = [cob, "-C", str(repo)]
    if workload == "compdb":
        return [*base, "-t", "compdb"]
    if workload == "build":
        return base
    if workload == "clean":
        return [*base, "-t", "clean"]
    raise ValueError(f"unknown workload: {workload}")


def prepare_command(workload: str, cob: str, repo: Path) -> list[str] | None:
    base = [cob, "-C", str(repo)]
    if workload == "build":
        return [*base, "-t", "clean"]
    if workload == "clean":
        return base
    return None


def run_quiet(cmd: Sequence[str]) -> None:
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def parse_perf_csv(path: Path) -> dict[str, int]:
    counters: dict[str, int] = {}
    for line in path.read_text().splitlines():
        parts = [part.strip() for part in line.split(",")]
        if len(parts) < 3:
            continue

        raw_value, event = parts[0], parts[2]
        normalized_event = normalize_perf_event(event)
        if normalized_event is None:
            continue
        if raw_value.startswith("<"):
            continue

        value = raw_value.replace(",", "")
        try:
            counters[normalized_event] = counters.get(normalized_event, 0) + int(value)
        except ValueError as exc:
            raise RuntimeError(f"could not parse perf counter line: {line}") from exc

    missing = [event for event in PERF_EVENTS if event not in counters]
    if missing:
        raise RuntimeError(f"perf output did not include counters: {', '.join(missing)}")
    return counters


def perf_stat(cmd: Sequence[str]) -> tuple[int, int, float]:
    with tempfile.NamedTemporaryFile(prefix="cob-ipc-", suffix=".csv", delete=False) as tmp:
        perf_output = Path(tmp.name)

    try:
        perf_cmd = [
            "perf",
            "stat",
            "-x",
            ",",
            "-e",
            ",".join(PERF_EVENTS),
            "-o",
            str(perf_output),
            "--",
            *cmd,
        ]
        result = subprocess.run(
            perf_cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            text=True,
        )
        if result.returncode != 0:
            detail = result.stderr.strip() or f"exit status {result.returncode}"
            raise RuntimeError(f"perf stat failed: {detail}")
        counters = parse_perf_csv(perf_output)
        instructions = counters["instructions"]
        cycles = counters["cycles"]
        ipc = instructions / cycles if cycles else 0.0
        return instructions, cycles, ipc
    finally:
        perf_output.unlink(missing_ok=True)


def mean(values: Sequence[float]) -> float:
    return statistics.fmean(values) if values else 0.0


def stdev(values: Sequence[float]) -> float:
    return statistics.stdev(values) if len(values) > 1 else 0.0


def format_int(value: float) -> str:
    return f"{value:,.0f}"


def write_markdown(
    path: Path,
    command: Sequence[str],
    rows: Sequence[tuple[int, int, float]],
) -> None:
    ipcs = [row[2] for row in rows]
    instructions = [row[0] for row in rows]
    cycles = [row[1] for row in rows]

    lines = [
        "# IPC Benchmark",
        "",
        f"Command: `{' '.join(command)}`",
        "",
        "| Runs | Instructions avg | Cycles avg | IPC avg | IPC stddev |",
        "| ---: | ---: | ---: | ---: | ---: |",
        (
            f"| {len(rows)} | {format_int(mean(instructions))} | "
            f"{format_int(mean(cycles))} | {mean(ipcs):.4f} | {stdev(ipcs):.4f} |"
        ),
        "",
        "| Run | Instructions | Cycles | IPC |",
        "| ---: | ---: | ---: | ---: |",
    ]

    for index, (inst, cyc, ipc) in enumerate(rows, 1):
        lines.append(f"| {index} | {inst:,} | {cyc:,} | {ipc:.4f} |")

    path.write_text("\n".join(lines) + "\n")


def print_results(command: Sequence[str], rows: Sequence[tuple[int, int, float]]) -> None:
    ipcs = [row[2] for row in rows]
    instructions = [row[0] for row in rows]
    cycles = [row[1] for row in rows]

    print("\nIPC benchmark")
    print(f"Command: {' '.join(command)}")
    print(
        "Summary: "
        f"runs={len(rows)}, "
        f"instructions_avg={format_int(mean(instructions))}, "
        f"cycles_avg={format_int(mean(cycles))}, "
        f"ipc_avg={mean(ipcs):.4f}, "
        f"ipc_stddev={stdev(ipcs):.4f}"
    )
    print()
    print(f"{'Run':>3} {'Instructions':>18} {'Cycles':>18} {'IPC':>8}")
    print("-" * 52)
    for index, (inst, cyc, ipc) in enumerate(rows, 1):
        print(f"{index:>3} {inst:>18,} {cyc:>18,} {ipc:>8.4f}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Measure COB workload IPC using Linux perf stat."
    )
    parser.add_argument(
        "--workload",
        choices=("compdb", "build", "clean"),
        default="compdb",
        help="Default COB workload to run when no command is supplied.",
    )
    parser.add_argument("--repo", type=Path, default=DEFAULT_REPO)
    parser.add_argument("--cob", default=None, help="Path to the cob executable.")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument(
        "--export-markdown",
        type=Path,
        help="Write benchmark results to this Markdown file.",
    )
    parser.add_argument(
        "--prepare",
        help="Command to run before each warmup and measured run.",
    )
    parser.add_argument(
        "command",
        nargs=argparse.REMAINDER,
        help="Command to measure. Prefix with -- to separate it from benchmark flags.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.runs < 1:
        raise SystemExit("--runs must be at least 1")
    if args.warmups < 0:
        raise SystemExit("--warmups must be non-negative")

    command = args.command
    if command and command[0] == "--":
        command = command[1:]

    using_default_command = not command
    repo = args.repo
    if args.prepare:
        prepare = shlex.split(args.prepare)
    elif using_default_command:
        cob = args.cob or find_cob()
        ensure_heavy_repo(repo)
        command = default_command(args.workload, cob, repo)
        prepare = prepare_command(args.workload, cob, repo)
    else:
        prepare = None

    for _ in range(args.warmups):
        if prepare:
            run_quiet(prepare)
        run_quiet(command)

    rows: list[tuple[int, int, float]] = []
    for index in range(args.runs):
        if prepare:
            run_quiet(prepare)
        print(f"perf run {index + 1}/{args.runs}...", flush=True)
        rows.append(perf_stat(command))

    print_results(command, rows)

    if args.export_markdown:
        write_markdown(args.export_markdown, command, rows)
        print(f"\nWrote {args.export_markdown}")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
