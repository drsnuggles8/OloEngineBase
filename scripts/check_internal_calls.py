#!/usr/bin/env python3
"""
Static analysis: verify C# InternalCall declarations match C++ registrations.

Parses:
  - OloEngine-ScriptCore/src/OloEngine/InternalCalls.cs for [MethodImpl(InternalCall)] names
  - OloEngine/src/OloEngine/Scripting/C#/ScriptGlue.cpp for OLO_ADD_INTERNAL_CALL(Name)

Exits with code 1 and prints a diff if the sets don't match.
"""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

CS_FILE = REPO_ROOT / "OloEngine-ScriptCore" / "src" / "OloEngine" / "InternalCalls.cs"
CPP_FILE = REPO_ROOT / "OloEngine" / "src" / "OloEngine" / "Scripting" / "C#" / "ScriptGlue.cpp"

# Matches:  internal extern static <type> MethodName(...)
# Also:     internal static extern <type> MethodName(...)
CS_PATTERN = re.compile(
    r"\[MethodImpl(?:Attribute)?\s*\(\s*MethodImplOptions\.InternalCall\s*\)\s*\]"
    r"\s+internal\s+(?:extern\s+static|static\s+extern)\s+\S+\s+(\w+)\s*\(",
    re.MULTILINE,
)

# Matches:  OLO_ADD_INTERNAL_CALL(Name); — excludes the #define line
CPP_PATTERN = re.compile(r"^\s+OLO_ADD_INTERNAL_CALL\(\s*(\w+)\s*\)", re.MULTILINE)


def extract_cs_names(path: Path) -> set[str]:
    text = path.read_text(encoding="utf-8-sig")
    return set(CS_PATTERN.findall(text))


def extract_cpp_names(path: Path) -> set[str]:
    text = path.read_text(encoding="utf-8")
    return set(CPP_PATTERN.findall(text))


def main() -> int:
    if not CS_FILE.exists():
        print(f"ERROR: C# file not found: {CS_FILE}", file=sys.stderr)
        return 1
    if not CPP_FILE.exists():
        print(f"ERROR: C++ file not found: {CPP_FILE}", file=sys.stderr)
        return 1

    cs_names = extract_cs_names(CS_FILE)
    cpp_names = extract_cpp_names(CPP_FILE)

    if not cs_names:
        print("ERROR: No InternalCall declarations found in C# file.", file=sys.stderr)
        return 1
    if not cpp_names:
        print("ERROR: No OLO_ADD_INTERNAL_CALL registrations found in C++ file.", file=sys.stderr)
        return 1

    only_cs = sorted(cs_names - cpp_names)
    only_cpp = sorted(cpp_names - cs_names)

    if not only_cs and not only_cpp:
        print(f"OK: {len(cs_names)} internal calls are in sync.")
        return 0

    print("MISMATCH detected between C# declarations and C++ registrations:\n", file=sys.stderr)

    if only_cs:
        print(f"  Declared in C# but NOT registered in C++ ({len(only_cs)}):", file=sys.stderr)
        for name in only_cs:
            print(f"    - {name}", file=sys.stderr)

    if only_cpp:
        print(f"\n  Registered in C++ but NOT declared in C# ({len(only_cpp)}):", file=sys.stderr)
        for name in only_cpp:
            print(f"    + {name}", file=sys.stderr)

    return 1


if __name__ == "__main__":
    sys.exit(main())
