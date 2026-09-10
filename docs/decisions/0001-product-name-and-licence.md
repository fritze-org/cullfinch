# 0001 — Product name and licence

Status: accepted · Date: 2026-09-10

## Context

The design specification left both the product name and the application licence
open. The licence choice interacts with Qt: the application uses Qt 6 Widgets,
which is available under the LGPLv3, and distribution has to account for the
licences of the selected Qt modules and every bundled dependency.

## Decision

The product is **cullfinch**.

The application is licensed **GPL-3.0-or-later**. Every owned source file
carries an `SPDX-License-Identifier: GPL-3.0-or-later` header, and the full
licence text is in [`LICENSE`](../../LICENSE).

Qt is linked **dynamically**, through repository-owned dynamic-linkage vcpkg
triplets. That keeps the LGPLv3 relinking obligation satisfiable without
shipping object files.

## Consequences

- GPL-3.0-or-later is compatible with the LGPLv3 Qt build, so there is no
  relicensing friction for the combined work.
- The release workflow generates a dependency and licence inventory per package,
  so what is actually redistributed is recorded rather than assumed.
- Contributions are accepted under the same licence.
