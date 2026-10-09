#!/usr/bin/env python3
"""Runs one circuit-trace scenario everywhere and reports where the runs differ.

For a skeleton, the script:

  1. records it with Circuit sequentially (1 worker) and in parallel
     (`--workers`, default 8), and compares the two recordings;
  2. records it on each reference harness given with `--harness` (an external
     checkout with tools/run_repeat.sh), twice on fresh worlds, and checks
     that the runs repeat; a harness may refuse a skeleton that does not apply
     to its version;
  3. evaluates every official recording with Circuit and locates the first
     difference from Circuit's own recording;
  4. compares the official recordings of different versions with each other;

and prints a Markdown report (also written to <out>/report.md). Real-client
checks are client scripts with their own expectations (compatibility/cases.json);
they are not driven from trace skeletons.

Usage:
  tools/scenario.py tests/fixtures/skeletons/item_drop_piston.ctrace \\
      --harness ../circuit-harness --harness ../circuit-harness-1.21.4 [--out DIR] [--workers 8]
"""
import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


def run(command, cwd=None):
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True)
    return result.returncode, (result.stdout + result.stderr).strip()


def game_version(path):
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("game-version "):
                return line.split()[1]
    return "?"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("skeleton")
    parser.add_argument("--harness", action="append", default=[], help="reference harness checkout (repeatable)")
    parser.add_argument("--server", default=os.path.join(ROOT, "build", "linux", "x86_64", "debug", "circuit-server"))
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--out")
    args = parser.parse_args()

    skeleton = os.path.abspath(args.skeleton)
    name = os.path.splitext(os.path.basename(skeleton))[0]
    out = os.path.abspath(args.out or os.path.join(ROOT, ".local", "scenarios", name))
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    report = [f"# Scenario `{name}`", "", f"Skeleton: `{os.path.relpath(skeleton, ROOT)}`", ""]
    failures = 0

    # 1. Circuit, sequential and parallel.
    report += ["## Circuit", ""]
    circuit = {}
    for workers in (1, args.workers):
        path = os.path.join(out, f"circuit-{workers}.ctrace")
        code, text = run([args.server, "--record-fixture", skeleton, "--output", path, "--workers", str(workers)])
        circuit[workers] = path if code == 0 else None
        report.append(f"- {workers} worker{'s' if workers > 1 else ''}: " + ("recorded" if code == 0 else f"not recorded: {text.splitlines()[-1]}"))
    if circuit[1] and circuit[args.workers]:
        code, text = run([args.server, "--diff-fixtures", circuit[1], circuit[args.workers]])
        report.append(f"- sequential against parallel: {'identical' if code == 0 else text}")
        failures += code != 0
    report.append("")

    # 2. Official servers through the reference harnesses.
    report += ["## Official servers", ""]
    official = {}
    for harness in args.harness:
        harness = os.path.abspath(harness)
        path = os.path.join(out, f"official-{os.path.basename(harness)}.ctrace")
        code, text = run([os.path.join(harness, "tools", "run_repeat.sh"), skeleton, path], cwd=harness)
        label = os.path.basename(harness)
        if code == 3:
            report.append(f"- `{label}`: refused, {text.splitlines()[-1].split(': ', 1)[-1]}")
            continue
        if code != 0:
            report.append(f"- `{label}`: {'not repeatable' if 'NOT repeatable' in text else 'recording failed'}: {text.splitlines()[-1]}")
            failures += 1
            continue
        version = game_version(path)
        official[version] = path
        report.append(f"- `{label}` ({version}): recorded twice, identical")
    report.append("")

    # 3. Circuit against each official recording.
    if official:
        report += ["## Circuit against the official servers", ""]
        for version, path in sorted(official.items()):
            code, text = run([args.server, "--check-fixture", path])
            first = text.splitlines()[0] if text else ""
            outcome = first.split(" ", 1)[0]
            detail = first.split("] ", 1)[-1]
            report.append(f"- {version}: **{outcome}** {detail}")
            if circuit[1]:
                code, text = run([args.server, "--diff-fixtures", path, circuit[1]])
                report.append(f"  - first difference from Circuit's recording: {'none' if code == 0 else text.split(': ', 1)[-1]}")
        report.append("")

    # 4. Versions against each other.
    if len(official) > 1:
        report += ["## Versions", ""]
        versions = sorted(official)
        for i in range(len(versions) - 1):
            a, b = versions[i], versions[i + 1]
            code, text = run([args.server, "--diff-fixtures", official[a], official[b]])
            report.append(f"- {a} against {b}: {'identical observations' if code == 0 else text.split(': ', 1)[-1]}")
        report.append("")

    report += ["Real-client checks are separate client scripts (`compatibility/cases.json`); trace skeletons do not drive them.", ""]
    text = "\n".join(report)
    with open(os.path.join(out, "report.md"), "w", encoding="utf-8") as f:
        f.write(text)
    print(text)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
