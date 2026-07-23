#!/usr/bin/env python3
# Auto-derive -DVERSION from git when the build doesn't set one. CI injects VERSION via
# PLATFORMIO_BUILD_FLAGS (tag name / short SHA), but plain local `pio run` builds didn't -
# the firmware then falls back to reporting "<sketch-md5>-<build-timestamp>" in its MQTT
# telemetry, which is unreadable in the companion's nodes table. An explicitly provided
# VERSION (env var or build_flags) always wins; this only fills the gap.
import re
import subprocess

Import("env")  # noqa: F821 - provided by PlatformIO/SCons

def version_already_defined() -> bool:
    return any(re.search(r"-D\s*VERSION\b", str(flag)) for flag in env.get("BUILD_FLAGS") or [])

if not version_already_defined():
    try:
        version = subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=env["PROJECT_DIR"], text=True).strip()
    except Exception:
        version = ""
    if version:
        env.Append(CPPDEFINES=[("VERSION", env.StringifyMacro(version))])
        print(f"version_from_git: VERSION={version}")
