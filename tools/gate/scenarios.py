#!/usr/bin/env python3
"""Client scenarios against pinned references; every verdict fails closed.

A scenario is a probe (a script that joins a server and prints what it observes) and, for each
game version, exactly one reference output recorded from the official server. The probes and
the references live outside this repository; this tool takes their directories as arguments
and a manifest that pins each reference by path and SHA-256.

Manifest: one line per scenario and version; '#' starts a comment.

    <scenario> <version> <reference> <sha256> <probe> [option=value ...]
    <scenario> <version> not-applicable

<reference> is relative to the reference directory and <probe> to the probe directory; neither
may contain spaces. Options:

    compare=bytes|added-as-set   bytes (default): identical files. added-as-set: each JSON
                                 line's "added" list is compared as a set
    tags=a,b                     families for --tag
    pack=<directory>             a data pack, relative to the probe directory
    refpack=<directory>          a data pack, relative to the reference directory
    args=a,b                     arguments of the probe after the port
    env:NAME=value               an environment variable of the probe
    timeout=<seconds>            for the probe (default 600)
    exit=<status>                the probe's expected exit status (default 0)

Subcommands (see --help of each): check, list, compare, run, pin, diff.
Exit status is 0 only when the manifest is valid and every selected step passed.
"""
import argparse
import hashlib
import json
import os
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

DEFAULT_VERSIONS = "1.21.8,1.21.4"
COMPARERS = ("bytes", "added-as-set")


class ManifestError(Exception):
    pass


class Entry:
    def __init__(self, scenario, version, line_number):
        self.scenario = scenario
        self.version = version
        self.line_number = line_number
        self.applicable = True
        self.reference = None
        self.sha256 = None
        self.probe = None
        self.compare = "bytes"
        self.tags = []
        self.pack = None
        self.pack_in_references = False
        self.args = []
        self.env = {}
        self.timeout = 600
        self.exit = 0

    @property
    def step(self):
        return f"{self.scenario} {self.version}"


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def inside(directory, relative):
    """The path of a manifest entry, which must stay inside its directory."""
    base = Path(directory).resolve()
    path = (base / relative).resolve()
    if base != path and base not in path.parents:
        raise ManifestError(f"{relative} leaves {directory}")
    return path


def load_manifest(path, versions):
    """Parses and validates a manifest: one entry per scenario and version, no more, no fewer."""
    entries = {}
    errors = []
    try:
        lines = Path(path).read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise ManifestError(f"cannot read the manifest: {error}")
    for number, raw in enumerate(lines, 1):
        text = raw.split("#", 1)[0].strip()
        if not text:
            continue
        fields = text.split()
        where = f"{path}:{number}"
        if len(fields) < 3:
            errors.append(f"{where}: expected '<scenario> <version> ...'")
            continue
        scenario, version = fields[0], fields[1]
        if version not in versions:
            errors.append(f"{where}: version {version} is not one of {', '.join(versions)}")
            continue
        if (scenario, version) in entries:
            first = entries[(scenario, version)].line_number
            errors.append(f"{where}: {scenario} {version} already has a reference (line {first}); exactly one is allowed")
            continue
        entry = Entry(scenario, version, number)
        entries[(scenario, version)] = entry
        if fields[2] == "not-applicable":
            if len(fields) != 3:
                errors.append(f"{where}: nothing may follow not-applicable")
            entry.applicable = False
            continue
        if len(fields) < 5:
            errors.append(f"{where}: expected '<scenario> <version> <reference> <sha256> <probe> [options]'")
            continue
        entry.reference, entry.sha256, entry.probe = fields[2], fields[3].lower(), fields[4]
        if len(entry.sha256) != 64 or any(c not in "0123456789abcdef" for c in entry.sha256):
            errors.append(f"{where}: the pin must be a SHA-256 of 64 hexadecimal digits")
        for option in fields[5:]:
            name, separator, value = option.partition("=")
            if not separator or not value:
                errors.append(f"{where}: bad option '{option}'")
            elif name == "compare":
                if value not in COMPARERS:
                    errors.append(f"{where}: compare must be one of {', '.join(COMPARERS)}")
                entry.compare = value
            elif name == "tags":
                entry.tags = [tag for tag in value.split(",") if tag]
            elif name in ("pack", "refpack"):
                entry.pack = value
                entry.pack_in_references = name == "refpack"
            elif name == "args":
                entry.args = value.split(",")
            elif name.startswith("env:") and len(name) > 4:
                entry.env[name[4:]] = value
            elif name in ("timeout", "exit"):
                if not value.isdigit():
                    errors.append(f"{where}: {name} must be a number")
                else:
                    setattr(entry, name, int(value))
            else:
                errors.append(f"{where}: unknown option '{name}'")
    for scenario in sorted({key[0] for key in entries}):
        for version in versions:
            if (scenario, version) not in entries:
                errors.append(f"{path}: {scenario} has no line for {version}; pin a reference or write 'not-applicable'")
    if not entries and not errors:
        errors.append(f"{path}: no scenario")
    if errors:
        raise ManifestError("\n".join(errors))
    return entries


def pack_of(entry, probes, references):
    """The entry's data pack directory, or None; raises when it is named and absent."""
    if not entry.pack:
        return None
    path = inside(references if entry.pack_in_references else probes, entry.pack)
    if not path.is_dir():
        raise ManifestError(f"data pack {entry.pack} is missing")
    return path


def check_reference(entry, references):
    """Returns the reference's path, or raises when it is absent or no longer what was pinned."""
    path = inside(references, entry.reference)
    if not path.is_file():
        raise ManifestError(f"reference {entry.reference} is missing")
    actual = sha256_of(path)
    if actual != entry.sha256:
        raise ManifestError(f"reference {entry.reference} is not the pinned file (sha256 {actual[:12]}..., pinned {entry.sha256[:12]}...)")
    return path


def select(entries, arguments):
    """The entries a command line names; naming nothing, or something unknown, is an error."""
    scenarios = sorted({key[0] for key in entries})
    chosen = []
    if arguments.all:
        chosen = scenarios
    for tag in arguments.tag or []:
        tagged = sorted({e.scenario for e in entries.values() if tag in e.tags})
        if not tagged:
            raise ManifestError(f"no scenario has the tag {tag}")
        chosen += tagged
    for scenario in arguments.scenarios:
        if scenario not in scenarios:
            raise ManifestError(f"scenario {scenario} is not in the manifest")
        chosen.append(scenario)
    if not chosen:
        raise ManifestError("no scenario selected: name scenarios, or use --tag or --all")
    ordered = []
    for scenario in dict.fromkeys(chosen):
        ordered += [entries[key] for key in sorted(entries) if key[0] == scenario]
    return ordered


def normalized_lines(path):
    """Lines with each JSON object's "added" list in a canonical order."""
    result = []
    with open(path, "r", encoding="utf-8", errors="surrogateescape") as handle:
        for line in handle:
            try:
                value = json.loads(line)
            except ValueError:
                result.append(line)
                continue
            if isinstance(value, dict) and isinstance(value.get("added"), list):
                value["added"] = sorted(value["added"], key=lambda item: json.dumps(item, sort_keys=True))
            result.append(json.dumps(value, sort_keys=True))
    return result


def first_difference(reference, output):
    """A short description of where two files part, by line."""
    with open(reference, "rb") as a, open(output, "rb") as b:
        number = 0
        while True:
            number += 1
            left, right = a.readline(), b.readline()
            if left != right:
                if not left:
                    return f"output has more lines from line {number}"
                if not right:
                    return f"output ends before line {number}"
                return f"first difference at line {number}"
            if not left:
                return "same"


def same(entry, reference, output):
    """(equal, detail) under the entry's comparer."""
    if entry.compare == "bytes":
        if Path(reference).read_bytes() == Path(output).read_bytes():
            return True, "same bytes"
        return False, first_difference(reference, output)
    left, right = normalized_lines(reference), normalized_lines(output)
    if left == right:
        return True, "same with \"added\" as a set"
    for number, (a, b) in enumerate(zip(left, right), 1):
        if a != b:
            return False, f"first difference at line {number}"
    return False, f"{len(right)} lines against {len(left)}"


class Summary:
    def __init__(self):
        self.lines = []

    def record(self, status, step, detail):
        self.lines.append((status, step, detail))
        print(f"{status:<8} {step:<40} {detail}", flush=True)

    def finish(self, title):
        failed = [line for line in self.lines if line[0] not in ("PASS", "N/A")]
        passed = [line for line in self.lines if line[0] == "PASS"]
        print(f"\n== {title}")
        for status, step, detail in self.lines:
            print(f"{status:<8} {step:<40} {detail}")
        if not passed and not failed:
            print("RESULT: FAIL (no step ran)")
            return 1
        if failed:
            print(f"RESULT: FAIL ({len(failed)} of {len(self.lines)} steps did not pass)")
            return 1
        print(f"RESULT: PASS ({len(passed)} steps)")
        return 0


def judge(entry, references, output, summary):
    try:
        reference = check_reference(entry, references)
    except ManifestError as error:
        summary.record("FAIL", entry.step, str(error))
        return
    if not Path(output).is_file():
        summary.record("NOT-RUN", entry.step, f"no output {output}")
        return
    equal, detail = same(entry, reference, output)
    summary.record("PASS" if equal else "FAIL", entry.step, f"{detail}: {entry.reference}")


def command_check(arguments, entries):
    summary = Summary()
    for entry in entries.values():
        if not entry.applicable:
            summary.record("N/A", entry.step, "declared not applicable")
            continue
        try:
            check_reference(entry, arguments.references)
            if arguments.probes and not inside(arguments.probes, entry.probe).is_file():
                raise ManifestError(f"probe {entry.probe} is missing")
            if arguments.probes:
                pack_of(entry, arguments.probes, arguments.references)
        except ManifestError as error:
            summary.record("FAIL", entry.step, str(error))
            continue
        summary.record("PASS", entry.step, f"pinned {entry.reference}")
    return summary.finish(f"manifest {arguments.manifest}")


def command_list(arguments, entries):
    for entry in entries.values():
        if entry.applicable:
            print(f"{entry.scenario}\t{entry.version}\t{entry.probe}\t{entry.reference}\t{entry.compare}\t{','.join(entry.tags)}")
        else:
            print(f"{entry.scenario}\t{entry.version}\tnot-applicable")
    return 0


def command_compare(arguments, entries):
    summary = Summary()
    for entry in select(entries, arguments):
        if not entry.applicable:
            summary.record("N/A", entry.step, "declared not applicable")
            continue
        judge(entry, arguments.references, Path(arguments.outputs) / f"{entry.scenario}-{entry.version}.out", summary)
    return summary.finish(f"stored outputs in {arguments.outputs}")


def port_open(port):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.settimeout(0.5)
        return probe.connect_ex(("127.0.0.1", port)) == 0


def stop(process):
    """Ends the server this tool started, and only that one."""
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=30)
    except subprocess.TimeoutExpired:
        # Not reaped yet, so the process group id is still its own.
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()


def run_one(entry, arguments, configs, port, summary):
    out = Path(arguments.out)
    work = Path(arguments.work)
    name = f"{entry.scenario}-{entry.version}"
    output = out / f"{name}.out"
    output.unlink(missing_ok=True)
    try:
        check_reference(entry, arguments.references)
        probe = inside(arguments.probes, entry.probe)
        if not probe.is_file():
            raise ManifestError(f"probe {entry.probe} is missing")
        if entry.version not in configs:
            raise ManifestError(f"no --config for version {entry.version}")
        config = Path(configs[entry.version])
        pack = pack_of(entry, arguments.probes, arguments.references)
        if pack:
            derived = work / f"{name}.properties"
            derived.write_text(config.read_text(encoding="utf-8").rstrip("\n") + f"\ndata-pack={pack}\n", encoding="utf-8")
            config = derived
    except (ManifestError, OSError) as error:
        summary.record("NOT-RUN", entry.step, str(error))
        return
    if port_open(port):
        summary.record("NOT-RUN", entry.step, f"port {port} is in use by another process")
        return
    world = work / f"world-{name}"
    subprocess.run(["rm", "-rf", str(world)], check=True)
    prefix = ["taskset", "-c", arguments.cpus] if arguments.cpus else []
    server_command = prefix + [str(Path(arguments.server).resolve()), "--serve", "--offline", "--port", str(port),
                               "--world", str(world), "--config", str(config.resolve())]
    with open(out / f"{name}.server.log", "wb") as server_log:
        server = subprocess.Popen(server_command, stdout=server_log, stderr=subprocess.STDOUT,
                                  stdin=subprocess.DEVNULL, cwd=work, start_new_session=True)
    try:
        deadline = time.monotonic() + arguments.start_timeout
        while not port_open(port):
            if server.poll() is not None:
                summary.record("NOT-RUN", entry.step, f"the server exited with status {server.returncode} before listening")
                return
            if time.monotonic() > deadline:
                summary.record("NOT-RUN", entry.step, f"the server did not listen on port {port} in {arguments.start_timeout} s")
                return
            time.sleep(0.5)
        time.sleep(arguments.settle)
        environment = dict(os.environ)
        environment.update(entry.env)
        environment["MC_VERSION"] = entry.version
        environment["STATS"] = str((out / f"{name}.stats.jsonl").resolve())
        probe_command = prefix + arguments.probe_command.split() + [str(probe), str(port)] + entry.args
        started = time.monotonic()
        timed_out = False
        with open(output, "wb") as handle:
            client = subprocess.Popen(probe_command, stdout=handle, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                      cwd=Path(arguments.probes).resolve(), env=environment, start_new_session=True)
            try:
                client.wait(timeout=entry.timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                os.killpg(client.pid, signal.SIGKILL)
                client.wait()
        seconds = int(time.monotonic() - started)
        if timed_out:
            summary.record("FAIL", entry.step, f"the probe did not finish in {entry.timeout} s")
            return
        if server.poll() is not None:
            summary.record("FAIL", entry.step, f"the server exited with status {server.returncode} during the probe")
            return
        if client.returncode != entry.exit:
            summary.record("FAIL", entry.step, f"the probe exited with status {client.returncode}, expected {entry.exit}")
            return
    finally:
        stop(server)
        subprocess.run(["rm", "-rf", str(world)], check=False)
    before = len(summary.lines)
    judge(entry, arguments.references, output, summary)
    if len(summary.lines) > before:
        status, step, detail = summary.lines[-1]
        summary.lines[-1] = (status, step, f"{detail} ({seconds} s)")


def command_run(arguments, entries):
    selected = select(entries, arguments)
    configs = {}
    for item in arguments.config or []:
        version, separator, path = item.partition("=")
        if not separator or not Path(path).is_file():
            raise ManifestError(f"--config {item}: expected <version>=<existing file>")
        configs[version] = path
    server = Path(arguments.server)
    if not server.is_file() or not os.access(server, os.X_OK):
        raise ManifestError(f"--server {arguments.server} is not an executable file")
    Path(arguments.out).mkdir(parents=True, exist_ok=True)
    Path(arguments.work).mkdir(parents=True, exist_ok=True)
    summary = Summary()
    for entry in selected:
        if not entry.applicable:
            summary.record("N/A", entry.step, "declared not applicable")
            continue
        try:
            run_one(entry, arguments, configs, arguments.port, summary)
        except Exception as error:  # a step that could not be judged did not pass
            summary.record("NOT-RUN", entry.step, f"{type(error).__name__}: {error}")
    return summary.finish(f"client scenarios on {arguments.server}")


def command_pin(arguments):
    for reference in arguments.files:
        path = inside(arguments.references, reference)
        if not path.is_file():
            raise ManifestError(f"reference {reference} is missing")
        print(f"{reference} {sha256_of(path)}")
    return 0


def command_diff(arguments):
    """Shows the steps where two outputs differ. The verdict is the bytes, not the parsed view."""
    def parsed(path):
        rows = []
        with open(path, "r", encoding="utf-8", errors="surrogateescape") as handle:
            for line in handle:
                try:
                    rows.append(json.loads(line))
                except ValueError:
                    rows.append(line.rstrip("\n"))
        return rows
    equal = Path(arguments.reference).read_bytes() == Path(arguments.output).read_bytes()
    reference, output = parsed(arguments.reference), parsed(arguments.output)
    for index in range(max(len(reference), len(output))):
        left = reference[index] if index < len(reference) else "<no line>"
        right = output[index] if index < len(output) else "<no line>"
        if left == right:
            continue
        if isinstance(left, dict) and isinstance(right, dict):
            print(f"## line {index + 1}: {left.get('step')} | {right.get('step')}")
            for key in sorted(set(left) | set(right)):
                if left.get(key) != right.get(key):
                    print(f"   {key}\n     reference {json.dumps(left.get(key))[:1200]}\n     output    {json.dumps(right.get(key))[:1200]}")
        else:
            print(f"## line {index + 1}\n     reference {json.dumps(left)[:1200]}\n     output    {json.dumps(right)[:1200]}")
    print(f"{len(reference)} lines against {len(output)}: {'same bytes' if equal else 'DIFFERENT'}")
    return 0 if equal else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)

    def with_manifest(sub, selection):
        sub.add_argument("--manifest", required=True)
        sub.add_argument("--references", required=True, help="directory the manifest's references are relative to")
        sub.add_argument("--versions", default=DEFAULT_VERSIONS, help=f"versions every scenario must cover (default {DEFAULT_VERSIONS})")
        if selection:
            sub.add_argument("--all", action="store_true", help="every scenario of the manifest")
            sub.add_argument("--tag", action="append", help="every scenario with this tag; may repeat")
            sub.add_argument("scenarios", nargs="*")

    sub = commands.add_parser("check", help="validate the manifest and the pins")
    with_manifest(sub, False)
    sub.add_argument("--probes", help="also check that the probes and data packs exist here")
    sub = commands.add_parser("list", help="print the manifest's entries")
    with_manifest(sub, False)
    sub = commands.add_parser("compare", help="judge stored outputs (<outputs>/<scenario>-<version>.out)")
    with_manifest(sub, True)
    sub.add_argument("--outputs", required=True)
    sub = commands.add_parser("run", help="run probes against a server binary and judge the outputs")
    with_manifest(sub, True)
    sub.add_argument("--probes", required=True, help="directory of the probes (their working directory)")
    sub.add_argument("--server", required=True, help="server binary; use a copy, not a build output that may be rewritten")
    sub.add_argument("--config", action="append", help="<version>=<properties file>; one per version")
    sub.add_argument("--port", type=int, required=True, help="port for the server; must be free")
    sub.add_argument("--work", required=True, help="directory for worlds and derived configurations")
    sub.add_argument("--out", required=True, help="directory for outputs and server logs")
    sub.add_argument("--probe-command", default="node", help="interpreter of the probes (default node)")
    sub.add_argument("--cpus", help="CPU list for taskset, to keep timing-sensitive probes off busy cores")
    sub.add_argument("--start-timeout", type=int, default=60)
    sub.add_argument("--settle", type=float, default=2.0, help="seconds between the server listening and the probe")
    sub = commands.add_parser("pin", help="print '<reference> <sha256>' for manifest lines")
    sub.add_argument("--references", required=True)
    sub.add_argument("files", nargs="+")
    sub = commands.add_parser("diff", help="show where two outputs differ; exit 1 unless the bytes are the same")
    sub.add_argument("reference")
    sub.add_argument("output")

    arguments = parser.parse_args()
    try:
        if arguments.command == "pin":
            return command_pin(arguments)
        if arguments.command == "diff":
            return command_diff(arguments)
        versions = [version for version in arguments.versions.split(",") if version]
        entries = load_manifest(arguments.manifest, versions)
        return {"check": command_check, "list": command_list, "compare": command_compare,
                "run": command_run}[arguments.command](arguments, entries)
    except ManifestError as error:
        print(f"FAIL     {error}", file=sys.stderr)
        print("RESULT: FAIL", file=sys.stderr)
        return 1
    except OSError as error:
        print(f"FAIL     {error}", file=sys.stderr)
        print("RESULT: FAIL", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
