# 0008 — The language standard is C++23, not the specified C++20

Status: accepted · Date: 2026-09-12

## Context

[`design.md`](../design.md) specifies C++20. The implementation honoured that, and
`CMAKE_CXX_STANDARD` and `cxx_std_23`'s predecessor agreed with it.

SonarQube Cloud analyses this repository through automatic analysis, which reads the sources
directly and therefore has no compilation database. With nothing to learn the target standard from,
the analyser assumes the newest standard it supports. That is not a misconfiguration we introduced —
it is the documented default — but it meant a standing set of findings recommended facilities that
did not exist in our dialect:

| Rule | Recommendation | Available since |
|---|---|---|
| `cpp:S7035` | `std::to_underlying` instead of `static_cast` | C++23 |
| `cpp:S7040` | delimited escapes, `"\x{1f}"` instead of `"\x1f"` | C++23 |
| `cpp:S5566` | `std::ranges::contains` instead of a search loop | C++23 |

Two ways to make the analyser and the code agree: pin the analyser back to C++20 with
`sonar.cfamily.reportingCppStandardOverride`, or move the code forward. The advice is good advice —
`to_underlying` cannot silently widen the wrong type the way `static_cast<int>` can, and a delimited
escape cannot absorb a following hex digit — so the code moves.

## Decision

Owned targets compile as C++23. `CMAKE_CXX_STANDARD` is 23 and
`cullfinch_project_options` requires `cxx_std_23`, so the standard is still stated once and applied
through one interface target.

Facilities are adopted where they are supported by **every** toolchain in CI, not merely by the
newest. `std::to_underlying` (GCC 11, libc++ 14) and delimited escape sequences (GCC 13, Clang 15)
clear that bar. `std::ranges::contains` does not — libc++ only gained it in 19, which is newer than
the AppleClang on the macOS runners — so searches use `std::ranges::find` or
`std::ranges::any_of` from C++20. Both satisfy `cpp:S5566`, which objects to the hand-written loop
rather than to any particular algorithm.

## Consequences

- The prerequisite in [`README.md`](../../README.md) is a C++23 compiler. The floor is GCC 13 or
  Clang 15; `ubuntu-24.04` and `macos-15` both clear it, and the pinned LLVM 23 used by the
  `clang-tidy` jobs clears it comfortably.
- `.sonarcloud.properties` deliberately leaves `reportingCppStandardOverride` unset, because the
  analyser's default now matches the project. A future downgrade would have to set it.
- New code may use C++23 freely *except* where libc++ lags; that exception is narrow enough to
  check per facility rather than to maintain as a list.
