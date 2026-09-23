#!/usr/bin/env python3
"""Run the production combo/macro functions with deterministic host kernel doubles.

Usage: python app/tests/host/run.py --cc cc
       python app/tests/host/run.py --cc /path/to/zig --zig
Only the kernel/device/settings boundaries are mocked; the reviewed C functions
are extracted from the working tree, so tests cannot silently run stale copies.
"""
import argparse
import pathlib
import subprocess
import tempfile


def function(source, name):
    import re
    match = re.search(r"^(?:static )?(?:inline )?[\w *]+\b" + name + r"\(", source, re.M)
    if not match:
        raise ValueError(f"Missing function: {name}")
    brace = source.index("{", match.start())
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--zig", action="store_true")
    args = parser.parse_args()
    app = pathlib.Path(__file__).resolve().parents[2]
    macro = (app / "src/behaviors/behavior_user_macro.c").read_text(encoding="utf-8")
    combo = (app / "src/combo.c").read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="zmk-runtime-tests-") as temp:
        root = pathlib.Path(temp)
        for header in ("zephyr/kernel.h", "zephyr/logging/log.h", "drivers/behavior.h", "zmk/behavior.h"):
            path = root / header
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("#pragma once\n", encoding="utf-8")
        (root / "queue.inc").write_text((app / "src/behavior_queue.c").read_text(encoding="utf-8"), encoding="utf-8")
        start = macro.index("struct user_macro_queue_state {")
        end = macro.index("};", start) + 2
        (root / "macro.inc").write_text(macro[start:end] + "\n" + "\n".join(
            function(macro, name) for name in (
                "expanded_event_count", "validate_slot", "next_macro_entry", "zmk_user_macro_queue"
            )), encoding="utf-8")
        start = macro.index("struct user_macro_v1_step {")
        end = macro.index("struct user_macro_slot_state {")
        (root / "macro-settings.inc").write_text(macro[start:end] + "\n".join(
            function(macro, name) for name in (
                "encode_body", "decode_body", "load_v1_record", "load_v2_blob", "user_macro_handle_set"
            )), encoding="utf-8")
        (root / "combo.inc").write_text("\n".join(function(combo, name) for name in (
            "candidate_is_completely_pressed", "position_state_down", "combo_is_busy",
            "zmk_combo_save_changes", "zmk_combo_discard_changes", "zmk_combo_reset_settings"
        )), encoding="utf-8")
        exe = root / "runtime-tests.exe"
        command = [args.cc] + (["cc"] if args.zig else [])
        command += ["-std=c11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Werror",
                    "-I", str(root), "-I", str(app / "include"),
                    str(pathlib.Path(__file__).with_name("runtime.c")), "-o", str(exe)]
        subprocess.run(command, check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
