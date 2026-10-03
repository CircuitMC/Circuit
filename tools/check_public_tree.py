#!/usr/bin/env python3
"""Reject tracked files covered by the current publication ignore policy."""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def git(*args, **kwargs):
    return subprocess.run(["git", "-C", str(ROOT), *args], **kwargs)


def check_files(files, label):
    if not files:
        return
    result = git("check-ignore", "--no-index", "--stdin", "-z", input=files,
                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode not in (0, 1):
        raise SystemExit(result.stderr.decode("utf-8", "replace"))
    ignored = [name for name in result.stdout.split(b"\0") if name]
    if ignored:
        print(f"Publication rejected: ignored files are tracked in {label}:", file=sys.stderr)
        for name in ignored:
            print(repr(name.decode("utf-8", "replace")), file=sys.stderr)
        raise SystemExit(1)


def check_tree(revision):
    result = git("ls-tree", "-r", "--name-only", "-z", revision, check=True, stdout=subprocess.PIPE)
    check_files(result.stdout, revision)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--staged", action="store_true")
    modes.add_argument("--pre-push", action="store_true")
    args = parser.parse_args()
    if args.staged:
        files = git("ls-files", "-z", check=True, stdout=subprocess.PIPE).stdout
        check_files(files, "index")
    elif args.pre_push:
        revisions = set()
        for line in sys.stdin:
            _, local_sha, _, remote_sha = line.split()
            if not local_sha.strip("0"):
                continue
            arguments = [local_sha]
            if remote_sha.strip("0") and git("cat-file", "-e", remote_sha,
                                             stderr=subprocess.DEVNULL).returncode == 0:
                arguments.append("^" + remote_sha)
            commits = git("rev-list", *arguments, check=True, stdout=subprocess.PIPE).stdout.decode().splitlines()
            revisions.update([local_sha, *commits])
        for revision in sorted(revisions):
            check_tree(revision)
    else:
        check_tree("HEAD")
    print("Publication tree audit passed")


if __name__ == "__main__":
    main()
