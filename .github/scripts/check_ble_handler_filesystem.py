#!/usr/bin/env python3
"""Fail when a Bluetooth callback can reach the filesystem.

GATT handlers and connection callbacks run on the Bluetooth RX thread, whose stack is 1 KB
(CONFIG_BT_RX_STACK_SIZE). A FatFs call needs far more than that, so a handler that touches the
SD card overflows the stack and faults the chip. From a host that looks only like a dropped
connection, which is why both instances took a full session each to find:

- reading a segment size or timestamp index from parse_storage_command() (fs_stat)
- writing an index record when the app set the clock, via
  time_sync_write_handler -> rtc_set_epoch -> storage_index_mark (fs_open/fs_write/fs_sync)

Both are fixed by doing the work on the storage thread instead. This walks the call graph from
every registered callback and reports any route that still ends in a filesystem call.
"""

from __future__ import annotations

import argparse
import re
import sys
from collections import deque
from pathlib import Path

FIRMWARE_ROOTS = ("omi/firmware/devkit/src", "omi/firmware/omi/src")

# Vendored third-party code has no Bluetooth surface, and its symbols would otherwise collide
# with firmware names in a call graph keyed by function name.
VENDORED_MARKERS = ("/lib/opus-",)

# Zephyr's filesystem API and the block layer under it. Everything heavy enough to matter goes
# through one of these; nothing in the firmware calls FatFs (f_*) directly.
FS_CALL_RE = re.compile(r"\b(?:fs_[a-z_]+|disk_access_[a-z_]+)\s*\(")

# A definition starts in column 0 in this codebase, and its parameter list may span lines, so
# the body is found by matching brackets rather than by regex.
DEFINITION_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_ \t\*]*?\b([a-z_][A-Za-z0-9_]*)\s*\(", re.MULTILINE)
CALL_RE = re.compile(r"\b([a-z_][A-Za-z0-9_]*)\s*\(")
GATT_MACRO_RE = re.compile(r"\bBT_GATT_(?:CHARACTERISTIC|CCC)\s*\(")
CONN_CALLBACK_RE = re.compile(
    r"\.(?:connected|disconnected|le_param_updated|le_phy_updated|le_data_len_updated|security_changed|recycled)"
    r"\s*=\s*([a-z_][A-Za-z0-9_]*)"
)
IDENTIFIER_RE = re.compile(r"\b([a-z_][A-Za-z0-9_]*)\b")

C_KEYWORDS = frozenset(
    {"if", "for", "while", "switch", "return", "sizeof", "defined", "do", "else", "case", "goto"}
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--changed-files", help="File containing changed repository-relative paths.")
    parser.add_argument("--root", default=".", help="Repository root.")
    return parser.parse_args()


def strip_comments(text: str) -> str:
    """Blank out comments and literals, preserving line structure so column-0 anchoring holds."""
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        pair = text[i : i + 2]
        if pair == "/*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("\n" * text.count("\n", i, end))
            i = end
        elif pair == "//":
            end = text.find("\n", i)
            i = n if end < 0 else end
        elif text[i] in "\"'":
            quote, j = text[i], i + 1
            while j < n and text[j] != quote:
                j += 2 if text[j] == "\\" else 1
            out.append("''")
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def match_bracket(text: str, start: int, opener: str, closer: str) -> int:
    depth = 0
    for i in range(start, len(text)):
        if text[i] == opener:
            depth += 1
        elif text[i] == closer:
            depth -= 1
            if depth == 0:
                return i
    return -1


def parse_functions(text: str) -> dict[str, str]:
    """Function name to body, for definitions only — a declaration has no brace."""
    functions: dict[str, str] = {}
    for match in DEFINITION_RE.finditer(text):
        name = match.group(1)
        if name in C_KEYWORDS:
            continue
        paren_close = match_bracket(text, match.end() - 1, "(", ")")
        if paren_close < 0:
            continue
        tail = text[paren_close + 1 :]
        if not tail.lstrip().startswith("{"):
            continue
        brace_open = paren_close + 1 + (len(tail) - len(tail.lstrip()))
        brace_close = match_bracket(text, brace_open, "{", "}")
        if brace_close < 0:
            continue
        functions[name] = text[brace_open : brace_close + 1]
    return functions


def find_ble_callbacks(text: str, known: set[str]) -> set[str]:
    """Functions registered as GATT characteristic/CCC callbacks or connection callbacks."""
    callbacks: set[str] = set()
    for match in GATT_MACRO_RE.finditer(text):
        close = match_bracket(text, match.end() - 1, "(", ")")
        if close < 0:
            continue
        for identifier in IDENTIFIER_RE.findall(text[match.end() : close]):
            if identifier in known:
                callbacks.add(identifier)
    for match in CONN_CALLBACK_RE.finditer(text):
        if match.group(1) in known:
            callbacks.add(match.group(1))
    return callbacks


def route_to_filesystem(start: str, calls: dict[str, set[str]], reaches_fs: set[str]) -> list[str] | None:
    """Shortest call chain from `start` to a function that calls the filesystem."""
    queue = deque([(start, [start])])
    seen = {start}
    while queue:
        name, chain = queue.popleft()
        if name in reaches_fs:
            return chain
        for callee in sorted(calls.get(name, ())):
            if callee not in seen and callee in calls:
                seen.add(callee)
                queue.append((callee, chain + [callee]))
    return None


def group_by_firmware(sources: dict[str, str]) -> dict[str, dict[str, str]]:
    """Split sources per firmware image.

    devkit and omi are separate binaries that share 104 function names, so a single graph lets
    one image's stub mask the other's real implementation — which is exactly how the first draft
    of this check passed while the bug it was written for was present.
    """
    groups: dict[str, dict[str, str]] = {}
    for path, text in sources.items():
        key = next((root for root in FIRMWARE_ROOTS if path.startswith(root)), "")
        groups.setdefault(key, {})[path] = text
    return groups


def analyze_image(sources: dict[str, str]) -> list[str]:
    stripped = {path: strip_comments(text) for path, text in sources.items()}

    # A name can still be defined more than once within one image (file-local statics). Union
    # the definitions so a clean namesake cannot hide a dirty one.
    calls: dict[str, set[str]] = {}
    reaches_fs: set[str] = set()
    location: dict[str, str] = {}
    for path, text in stripped.items():
        for name, body in parse_functions(text).items():
            calls.setdefault(name, set()).update(set(CALL_RE.findall(body)) - C_KEYWORDS)
            if FS_CALL_RE.search(body):
                reaches_fs.add(name)
                location[name] = path
            location.setdefault(name, path)

    callbacks: dict[str, str] = {}
    for path, text in stripped.items():
        for name in find_ble_callbacks(text, set(calls)):
            callbacks[name] = path

    errors: list[str] = []
    for name in sorted(callbacks):
        chain = route_to_filesystem(name, calls, reaches_fs)
        if chain:
            errors.append(
                f"{callbacks[name]}: Bluetooth callback {name}() reaches the filesystem via "
                f"{' -> '.join(chain)}(), defined in {location[chain[-1]]}. "
                "Latch the request and do the work on the storage thread instead "
                "(see storage_index_mark)."
            )
    return errors


def analyze(sources: dict[str, str]) -> list[str]:
    errors: list[str] = []
    for _, image in sorted(group_by_firmware(sources).items()):
        errors.extend(analyze_image(image))
    return errors


def collect_sources(root: Path) -> dict[str, str]:
    sources: dict[str, str] = {}
    for relative in FIRMWARE_ROOTS:
        directory = root / relative
        if not directory.is_dir():
            continue
        for path in sorted(directory.rglob("*.c")):
            relative = path.relative_to(root).as_posix()
            if any(marker in f"/{relative}" for marker in VENDORED_MARKERS):
                continue
            sources[relative] = path.read_text(encoding="utf-8", errors="replace")
    return sources


def is_triggered(changed_paths: list[str]) -> bool:
    return any(path.startswith(root) for path in changed_paths for root in FIRMWARE_ROOTS)


def main() -> int:
    args = parse_args()
    root = Path(args.root).resolve()

    if args.changed_files:
        changed_file = Path(args.changed_files)
        if not changed_file.is_absolute():
            changed_file = root / changed_file
        try:
            changed_paths = [
                line.strip() for line in changed_file.read_text(encoding="utf-8").splitlines() if line.strip()
            ]
        except OSError as exc:
            print(f"FAIL: could not read changed-files input: {exc}")
            return 1
        if not is_triggered(changed_paths):
            print("OK: no firmware sources changed.")
            return 0

    sources = collect_sources(root)
    if not sources:
        print(f"FAIL: no firmware sources found under {', '.join(FIRMWARE_ROOTS)}")
        return 1

    errors = analyze(sources)
    if errors:
        print("FAIL: Bluetooth callbacks must not touch the filesystem")
        for error in errors:
            print(f"- {error}")
        return 1

    print(f"OK: no filesystem access reachable from a Bluetooth callback ({len(sources)} sources).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
