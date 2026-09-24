#!/usr/bin/env python3
"""
amalgamate.py
flatten a tree of local #include "..." headers into a single distributable header for consumers who just want #include "embr.h"

NOT a preprocessor. it only recursively inlines local quoted #include directives in dependency order, and:
1. puts every #include <...> to the top of the output, deduplicated, in original order.
2. inlines each local header's contents exactly once.
3. emits #line directives around each inlined chunk so compiler errors, warnings and debugger stepping still point at the real modular source files, not line 4821 of the generated header.
4. leaves every #ifdef/#define/comment/whatever completely untouched.

usage:
    python3 amalgamate.py ENTRY -o OUTPUT [-I DIR ...] [-D NAME[=VALUE] ...]
                           [--guard NAME] [--banner TEXT]

example:
    python3 amalgamate.py include/embr/core/root.h -o embr.h -I include

    python3 amalgamate.py include/embr/core/root.h -o embr_vm.h -I include -D EMBR_WITH_VM
"""

import argparse
import re
import sys
from pathlib import Path

LOCAL_INCLUDE_RE  = re.compile(r'^(\s*)#\s*include\s*"([^"]+)"\s*(//.*)?$')
SYSTEM_INCLUDE_RE = re.compile(r'^(\s*)#\s*include\s*<([^>]+)>\s*(//.*)?$')
COND_OPEN_RE      = re.compile(r'^\s*#\s*(if|ifdef|ifndef)\b')
COND_END_RE       = re.compile(r'^\s*#\s*endif\b')
GUARD_IFNDEF_RE   = re.compile(r'^\s*#\s*ifndef\s+(\w+)\s*$')
GUARD_DEFINE_RE   = re.compile(r'^\s*#\s*define\s+(\w+)\s*$')


def detect_include_guard(lines):
    """
    return the guard macro name if lines opens with the standard #ifndef GUARD / #define GUARD, else None.
    that opening #ifndef wraps the entire file (including all of its #include lines)
    """
    i = 0
    while i < len(lines) and (not lines[i].strip() or lines[i].strip().startswith('//')):
        i += 1
    if i >= len(lines):
        return None
    m1 = GUARD_IFNDEF_RE.match(lines[i])
    if not m1:
        return None
    j = i + 1
    while j < len(lines) and (not lines[j].strip() or lines[j].strip().startswith('//')):
        j += 1
    if j >= len(lines):
        return None
    m2 = GUARD_DEFINE_RE.match(lines[j])
    if not m2 or m2.group(1) != m1.group(1):
        return None
    return m1.group(1)


class Amalgamator:
    def __init__(self, include_dirs):
        self.include_dirs = [Path(d).resolve() for d in include_dirs]
        self.visited       = set()   # resolved Paths already inlined
        self.system_includes = []    # ordered, deduped "<name>" strings
        self._system_seen  = set()
        self.output_lines  = []

    def resolve(self, including_dir: Path, name: str):
        # local includes are resolved relative to the including file first
        # matches compiler behaviour for quoted includes, then against each -I search directory in order given
        candidate = (including_dir / name)
        if candidate.is_file():
            return candidate.resolve()
        for d in self.include_dirs:
            candidate = d / name
            if candidate.is_file():
                return candidate.resolve()
        return None

    def process(self, path: Path):
        path = path.resolve()

        if path in self.visited:
            self.output_lines.append(
                f'// [amalgamate] {path.name} already inlined above, skipping duplicate #include')
            return

        self.visited.add(path)

        try:
            text = path.read_text(encoding='utf-8').splitlines()
        except OSError as e:
            sys.exit(f"error: cannot read '{path}': {e}")

        posix_path = path.as_posix()
        self.output_lines.append(f'// {"-" * 76}')
        self.output_lines.append(f'// begin: {posix_path}')
        self.output_lines.append(f'#line 1 "{posix_path}"')

        # depth of #if/#ifdef/#ifndef nesting
        # #include seen when depth > 0 is conditional and MUST be untouched
        # #elif/#else don't change depth, only the opening directive and #endif do
        #
        # the file's own #ifndef/#define include guard also opens a conditional, but it always evaluates true on first inclusion
        # so it's excluded by starting depth at -1: the guard's #ifndef brings it back to the real baseline of 0.
        guard_name = detect_include_guard(text)
        cond_depth = -1 if guard_name else 0

        for lineno, line in enumerate(text, start=1):
            if COND_OPEN_RE.match(line):
                cond_depth += 1
                self.output_lines.append(line)
                continue
            if COND_END_RE.match(line):
                cond_depth = max(0, cond_depth - 1)
                self.output_lines.append(line)
                continue

            m_local = LOCAL_INCLUDE_RE.match(line)
            if m_local:
                if cond_depth > 0:
                    sys.exit(
                        f'error: {posix_path}:{lineno}: local #include "{m_local.group(2)}" '
                        f'is inside a conditional (#if/#ifdef) block.\n'
                        f'  amalgamate.py only inlines unconditional local includes -- '
                        f'splicing one in while preserving the surrounding #if/#else '
                        f'structure isn\'t supported. move it out of the conditional, '
                        f'or guard the code it defines instead of the include itself.')
                inc_name = m_local.group(2)
                target = self.resolve(path.parent, inc_name)
                if target is None:
                    searched = ", ".join(str(d) for d in self.include_dirs) or "(none)"
                    sys.exit(
                        f'error: {posix_path}:{lineno}: cannot resolve local '
                        f'include "{inc_name}"\n'
                        f'  searched next to including file and: {searched}')
                self.process(target)
                # resume line numbering in the includer once we return
                self.output_lines.append(f'#line {lineno + 1} "{posix_path}"')
                continue

            m_sys = SYSTEM_INCLUDE_RE.match(line)
            if m_sys:
                if cond_depth > 0:
                    # conditional system include (platform/feature-specific):
                    # leave it exactly in place, don't hoist or dedupe it
                    self.output_lines.append(line)
                    continue
                sys_name = m_sys.group(2)
                if sys_name not in self._system_seen:
                    self._system_seen.add(sys_name)
                    self.system_includes.append(sys_name)
                # unconditional: stripped from the body, re-emitted once at the top instead
                continue

            self.output_lines.append(line)

        self.output_lines.append(f'// end: {posix_path}')

    def render(self, entry_display: str, guard: str | None, banner: str | None,
               defines: list[str]) -> str:
        header = []
        if banner:
            for l in banner.splitlines():
                header.append(f'// {l}')
            header.append('//')
        header.append('// ** GENERATED FILE. do not edit directly. **')
        header.append(f'// generated by tools/amalgamate.py from {entry_display}')
        header.append('// edit the modular sources and regenerate instead.')
        header.append('')

        if guard:
            header.append(f'#ifndef {guard}')
            header.append(f'#define {guard}')
            header.append('')

        for d in defines:
            if '=' in d:
                name, value = d.split('=', 1)
                header.append(f'#define {name} {value}')
            else:
                header.append(f'#define {d}')
        if defines:
            header.append('')

        for s in self.system_includes:
            header.append(f'#include <{s}>')
        if self.system_includes:
            header.append('')

        footer = []
        if guard:
            footer.append('')
            footer.append(f'#endif // {guard}')

        return '\n'.join(header + self.output_lines + footer) + '\n'


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('entry', help='entry header to start amalgamation from')
    ap.add_argument('-o', '--output', required=True, help='path to write the generated header')
    ap.add_argument('-I', '--include-dir', action='append', default=[],
                     help='additional local-include search directory (repeatable)')
    ap.add_argument('-D', '--define', action='append', default=[],
                     help='NAME or NAME=VALUE to #define at the top of the output '
                          '(e.g. -D EMBR_WITH_VM). Repeatable.')
    ap.add_argument('--guard', default=None,
                     help='include-guard macro name to wrap the whole output in '
                          '(defaults to derived from output filename)')
    ap.add_argument('--banner', default=None, help='free-text comment banner at the top')
    args = ap.parse_args()

    entry = Path(args.entry)
    if not entry.is_file():
        sys.exit(f"error: entry header not found: {entry}")

    guard = args.guard
    if guard is None:
        stem = Path(args.output).stem.upper().replace('-', '_').replace('.', '_')
        guard = f'{stem}_GENERATED_INCLUDED'

    amal = Amalgamator(args.include_dir)
    amal.process(entry)
    result = amal.render(str(entry), guard, args.banner, args.define)

    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(result, encoding='utf-8')

    print(f"[amalgamate] {entry} -> {out_path}  "
          f"({len(amal.visited)} local headers inlined, "
          f"{len(amal.system_includes)} system includes hoisted)")


if __name__ == '__main__':
    main()
