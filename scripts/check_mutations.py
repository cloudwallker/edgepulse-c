#!/usr/bin/env python3
"""Build five deliberate C defects in temporary copies and check detection.

This is a deterministic regression sensitivity check, not a mutation score
over all possible defects. Only a successfully compiled test that exits
nonzero counts as detection; compiler errors and timeouts are test errors.
The original source tree is never modified.
"""

import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


TIMEOUT_SECONDS = 30
REPETITIONS = 3
STRICT_FLAGS = ("-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wpedantic")
SUITES = {
    "protocol": ("src/protocol.c", "tests/test_protocol.c"),
    "store": ("src/protocol.c", "src/store.c", "tests/test_store.c"),
    "device": (
        "src/protocol.c", "src/store.c", "src/device.c", "tests/test_device.c"
    ),
}
MUTATIONS = (
    {
        "id": "crc_initial_value",
        "description": "CRC initialization changed from 0xffff to zero",
        "file": "src/protocol.c",
        "old": "uint16_t crc = 0xffffu;",
        "new": "uint16_t crc = 0u;",
        "suite": "protocol",
    },
    {
        "id": "queue_drop_accounting",
        "description": "Queue-full samples no longer increment the drop counter",
        "file": "src/device.c",
        "old": "d->metrics.dropped++;",
        "new": "d->metrics.dropped+=0u;",
        "suite": "device",
    },
    {
        "id": "command_deduplication",
        "description": "Successful command IDs are never recognized as duplicates",
        "file": "src/device.c",
        "old": "if(old->valid && old->id==id)",
        "new": "if(old->valid && old->id==id && false)",
        "suite": "device",
    },
    {
        "id": "watchdog_threshold",
        "description": "Watchdog threshold is raised to 0x7fffffff milliseconds",
        "file": "src/device.c",
        "old": "elapsed>=EP_WATCHDOG_MS",
        "new": "elapsed>=UINT32_C(0x7fffffff)",
        "suite": "device",
    },
    {
        "id": "nvm_erase_active_slot",
        "description": "A save erases the other slot, destroying committed fallback data",
        "file": "src/store.c",
        "old": "memset(store->slots[target], 0xff, EP_SLOT_SIZE);",
        "new": "memset(store->slots[1u - target], 0xff, EP_SLOT_SIZE);",
        "suite": "store",
    },
)


def suppress_windows_error_dialogs():
    """Child test processes inherit this mode, so assert aborts cannot hang UI."""
    if os.name == "nt":
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.SetErrorMode.argtypes = [ctypes.c_uint]
        kernel32.SetErrorMode.restype = ctypes.c_uint
        # SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX
        kernel32.SetErrorMode(0x0001 | 0x0002 | 0x8000)


def text(value):
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return value or ""


def run(command, cwd):
    try:
        result = subprocess.run(
            command, cwd=str(cwd), capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=TIMEOUT_SECONDS,
            check=False,
        )
        return {
            "process_status": "exited",
            "exit_code": result.returncode,
            "stdout": result.stdout,
            "stderr": result.stderr,
        }
    except subprocess.TimeoutExpired as error:
        return {
            "process_status": "timeout", "exit_code": None,
            "stdout": text(error.stdout), "stderr": text(error.stderr),
        }
    except OSError as error:
        return {
            "process_status": "launch_error", "exit_code": None,
            "stdout": "", "stderr": str(error),
        }


def succeeded(result):
    return result["process_status"] == "exited" and result["exit_code"] == 0


def compile_suite(compiler, directory, suite):
    executable = directory / ("test_" + suite + (".exe" if os.name == "nt" else ""))
    command = [compiler, *STRICT_FLAGS, "-I", "include", *SUITES[suite],
               "-o", executable.name]
    return executable, run(command, directory)


def snapshot(source, destination):
    destination.mkdir()
    for name in ("include", "src", "tests"):
        shutil.copytree(source / name, destination / name)


def run_signature(result):
    return (result["process_status"], result["exit_code"],
            result["stdout"], result["stderr"])


def baseline(compiler, directory):
    results = []
    for suite in SUITES:
        executable, compilation = compile_suite(compiler, directory, suite)
        entry = {"suite": suite, "compile": compilation}
        if not succeeded(compilation):
            entry["status"] = "test_error"
        else:
            entry["run"] = run([str(executable)], directory)
            entry["status"] = "passed" if succeeded(entry["run"]) else "test_error"
        results.append(entry)
    return results


def check_mutation(compiler, pristine, directory, mutation):
    snapshot(pristine, directory)
    entry = {"id": mutation["id"], "description": mutation["description"],
             "suite": mutation["suite"], "runs": []}
    path = directory / mutation["file"]
    original = path.read_text(encoding="utf-8")
    occurrences = original.count(mutation["old"])
    if occurrences != 1:
        entry.update(status="test_error", error="Mutation anchor must occur exactly once",
                     anchor_occurrences=occurrences)
        return entry
    path.write_text(original.replace(mutation["old"], mutation["new"], 1), encoding="utf-8")
    executable, compilation = compile_suite(compiler, directory, mutation["suite"])
    entry["compile"] = compilation
    if not succeeded(compilation):
        entry["status"] = "test_error"
        return entry
    for _ in range(REPETITIONS):
        entry["runs"].append(run([str(executable)], directory))
    entry["reproducible"] = all(
        run_signature(result) == run_signature(entry["runs"][0])
        for result in entry["runs"]
    )
    if any(result["process_status"] != "exited" for result in entry["runs"]):
        entry["status"] = "test_error"
    elif not entry["reproducible"]:
        entry["status"] = "test_error"
    elif all(result["exit_code"] != 0 for result in entry["runs"]):
        entry["status"] = "detected"
    else:
        entry["status"] = "survived"
    return entry


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="gcc", help="GCC executable name or path")
    args = parser.parse_args()
    compiler = shutil.which(args.compiler)
    summary = {
        "status": "test_error", "compiler": Path(args.compiler).name,
        "strict_flags": list(STRICT_FLAGS), "timeout_seconds": TIMEOUT_SECONDS,
        "repetitions": REPETITIONS, "baseline": [], "mutations": [],
        "detected": 0, "expected_mutations": len(MUTATIONS),
    }
    if compiler is None:
        summary["error"] = "Compiler executable was not found"
        print(json.dumps(summary, indent=2))
        return 1
    # The compiler is invoked from copied temporary trees, so relative paths
    # supplied by the caller must be resolved before changing child cwd.
    compiler = str(Path(compiler).resolve())
    suppress_windows_error_dialogs()
    source = Path(__file__).resolve().parents[1]
    try:
        with tempfile.TemporaryDirectory(prefix="edgepulse-mutations-") as temp:
            temporary = Path(temp)
            pristine = temporary / "baseline"
            snapshot(source, pristine)
            summary["source_sha256"] = {
                str(path.relative_to(pristine)).replace("\\", "/"):
                hashlib.sha256(path.read_bytes()).hexdigest()
                for name in ("include", "src", "tests")
                for path in sorted((pristine / name).rglob("*")) if path.is_file()
            }
            summary["baseline"] = baseline(compiler, pristine)
            if all(result["status"] == "passed" for result in summary["baseline"]):
                for mutation in MUTATIONS:
                    summary["mutations"].append(check_mutation(
                        compiler, pristine, temporary / mutation["id"], mutation
                    ))
                summary["detected"] = sum(
                    result["status"] == "detected" for result in summary["mutations"]
                )
                summary["status"] = (
                    "passed" if summary["detected"] == len(MUTATIONS) else "failed"
                )
            else:
                summary["error"] = "Unmodified baseline must pass all three C test suites"
    except (OSError, ValueError) as error:
        summary["status"] = "test_error"
        summary["error"] = str(error)
    print(json.dumps(summary, indent=2))
    return 0 if summary["status"] == "passed" else 1


if __name__ == "__main__":
    sys.exit(main())
