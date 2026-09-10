# 0002 — Target and option prefix

Status: accepted · Date: 2026-09-10

## Context

The design specification was written before the product had a name, so it used a
`cull_` prefix for CMake targets (`cull_domain`) and a `CULL_` prefix for options
(`CULL_ENABLE_CLANG_TIDY`, `CULL_VCPKG_ROOT`, `CULL_OFFLINE`).

## Decision

Now that the name is settled, targets use `cullfinch_` and options use
`CULLFINCH_`. The module boundaries themselves are exactly as specified:

| Specification | Implementation |
|---|---|
| `cull_domain` | `cullfinch_domain` |
| `cull_application` | `cullfinch_application` |
| `cull_flow_versus` | `cullfinch_flow_versus` |
| `cull_flow_wall` | `cullfinch_flow_wall` |
| `cull_infrastructure` | `cullfinch_infrastructure` |
| `cull_ui` | `cullfinch_ui` |
| `cull_view_versus`, `cull_view_wall` | `cullfinch_view_versus`, `cullfinch_view_wall` |
| `cull_app` | `cullfinch_composition` plus the `cullfinch_app` executable |

## Consequences

- `cull_app` became two targets. The wiring lives in a library so GUI tests can
  assemble the *production* composition with injectable adapters instead of
  re-implementing it; only `main.cpp` stays in the executable.
- The helper functions follow: `cullfinch_enable_static_analysis`,
  `cullfinch_enable_coverage`, `cullfinch_add_library`, `cullfinch_add_test`.
