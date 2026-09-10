# 0006 — Operation safety is a separate flag from pairing state

Status: accepted · Date: 2026-09-10

## Context

The specification insists that `Reject` (intent), `Stale` (pairing) and
`PartiallyStaged` (operation) stay independent rather than collapsing into one
status enum. It also requires that symlinks, hardlink aliases and unclassified
same-stem files are flagged rather than acted on.

Modelling those three as `PairingState::Ambiguous` would have been the obvious
shortcut — and it would have been wrong. A symlinked JPG pairs perfectly well
and is perfectly safe to *look at*; what is unsafe is *moving* it, because the
file it points at is a second representation of something else.

## Decision

`PhotoAsset` carries `operationsBlocked` and `diagnostics` alongside
`pairingState` and `disposition`. `isComparable()` consults pairing state;
`isOperable()` consults both.

| Situation | Pairing state | Comparable | Operable |
|---|---|---|---|
| Symlinked member | unchanged | yes | no |
| Hardlink alias of another discovered file | unchanged | yes | no |
| Unclassified same-stem file (`A.JPG`, `A.RAF`, `A.txt`) | `Resolved` | yes | no |
| Duplicate previews (`A.JPG`, `A.jpeg`) | `Ambiguous` | no | no |
| Stem differs only by case (`A.JPG`, `a.RAF`) | `Ambiguous` | no | no |
| A previously known member vanished | `Stale` | no | no |

## Consequences

- Browsing and comparison are never blocked by a concern that only affects file
  operations, which keeps the browser usable on real directories.
- `OperationPlanner` refuses any asset where `isOperable()` is false and reports
  the specific diagnostic, so the review screen explains the blocker instead of
  silently omitting the photo.
- A survivor of a comparison can still turn out to be unmovable later. That is
  correct: the check belongs immediately before the files move, and preflight
  repeats it against the filesystem.
