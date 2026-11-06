#!/usr/bin/env python3
"""Fetches the shader compiler sources the ARMSX2 Vulkan build links.

shaderc's own source ships inside ARMSX2 (third_party/armsx2/.../3rdparty/shaderc) and is part of the import. Its
build-time dependencies are separate, large repositories and are not committed: this script clones each one at the
exact revision ARMSX2's shaderc pins in its DEPS file and puts it where shaderc's CMakeLists expects it.

    python tools/fetch_armsx2_shader_deps.py

The directories it creates are git-ignored. Re-run it after updating the ARMSX2 import; the pins below must match the
DEPS file of the imported shaderc, and the script fails loudly if they do not.
"""

import os
import re
import subprocess
import sys

# Pins from third_party/armsx2/.../3rdparty/shaderc/DEPS at ARMSX2 1049db37fb.
PINS = {
    "glslang": ("https://github.com/KhronosGroup/glslang.git", "09c541ee5b22bbac307987b50d86ec2b4f683d75"),
    "spirv-headers": ("https://github.com/KhronosGroup/SPIRV-Headers.git", "465055f6c9128772e20082e893d974146acf7a02"),
    "spirv-tools": ("https://github.com/KhronosGroup/SPIRV-Tools.git", "e4bceacf59fdfe742047e94e41ef65a48999a0dd"),
}


def repository_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(args, cwd=None):
    print("+ " + " ".join(args))
    result = subprocess.run(args, cwd=cwd)
    if result.returncode != 0:
        raise SystemExit("command failed (%d): %s" % (result.returncode, " ".join(args)))


def output(args, cwd=None):
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit("command failed (%d): %s\n%s" % (result.returncode, " ".join(args), result.stderr))
    return result.stdout.strip()


def read_deps_pins(shaderc_dir):
    deps = os.path.join(shaderc_dir, "DEPS")
    with open(deps, encoding="utf-8") as f:
        text = f.read()
    names = {
        "glslang": "glslang_revision",
        "spirv-headers": "spirv_headers_revision",
        "spirv-tools": "spirv_tools_revision",
    }
    found = {}
    for name, variable in names.items():
        match = re.search(r"'%s'\s*:\s*'([0-9a-f]{40})'" % variable, text)
        if not match:
            raise SystemExit("cannot find %s in %s" % (variable, deps))
        found[name] = match.group(1)
    return found


def ensure_checkout(name, url, revision, target):
    if os.path.isdir(os.path.join(target, ".git")):
        head = output(["git", "rev-parse", "HEAD"], cwd=target)
        if head == revision:
            print("%s: already at %s" % (name, revision))
            return
        raise SystemExit(
            "%s exists at %s, expected %s; remove %s and run again" % (name, head, revision, target))
    if os.path.exists(target):
        raise SystemExit("%s exists and is not a git checkout; remove it and run again" % target)

    print("=== %s: %s at %s" % (name, url, revision))
    os.makedirs(target)
    run(["git", "init", "--quiet"], cwd=target)
    run(["git", "remote", "add", "origin", url], cwd=target)
    # GitHub answers a direct fetch of a commit SHA. Fall back to a full fetch if it does not.
    fetch = subprocess.run(["git", "fetch", "--depth", "1", "origin", revision], cwd=target)
    if fetch.returncode != 0:
        print("%s: direct fetch refused, fetching the full history instead" % name)
        run(["git", "fetch", "origin"], cwd=target)
    run(["git", "checkout", "--quiet", "--detach", revision], cwd=target)

    head = output(["git", "rev-parse", "HEAD"], cwd=target)
    if head != revision:
        raise SystemExit("%s: checked out %s, expected %s" % (name, head, revision))
    print("%s: verified %s" % (name, revision))


def main():
    root = repository_root()
    shaderc = os.path.join(
        root, "third_party", "armsx2", "platforms", "android", "app", "src", "main", "cpp",
        "3rdparty", "shaderc")
    if not os.path.isfile(os.path.join(shaderc, "DEPS")):
        raise SystemExit("shaderc not found at %s; the ARMSX2 import is incomplete" % shaderc)

    deps_pins = read_deps_pins(shaderc)
    for name, revision in deps_pins.items():
        if PINS[name][1] != revision:
            raise SystemExit(
                "%s: this script pins %s but the imported shaderc DEPS pins %s; update the script"
                % (name, PINS[name][1], revision))
    print("shaderc DEPS matches the pins in this script")

    for name in ("glslang", "spirv-headers", "spirv-tools"):
        url, revision = PINS[name]
        ensure_checkout(name, url, revision, os.path.join(shaderc, "third_party", name))

    print("all shader compiler dependencies are in place")


if __name__ == "__main__":
    sys.exit(main())
