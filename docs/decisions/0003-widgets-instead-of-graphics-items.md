# 0003 — Comparison surfaces use widgets, not graphics items

Status: accepted · Date: 2026-09-10

## Context

The design proposed the Graphics View framework for interactive graphical items,
and separately required that keyboard operation is mandatory and that custom
graphics items expose accessible names and actions.

Those two requirements pull against each other. `QGraphicsItem` has no
accessibility interface of its own: exposing names, roles, states and actions
means implementing a bespoke `QAccessibleInterface` bridge and hand-rolling
focus handling, and every platform screen reader then depends on that bridge
being right.

## Decision

The image canvas and each wall tile are `QWidget`s. The wall positions them with
the deterministic layout computed by `cullfinch_flow_wall`, which stays pure
geometry and is unit-tested without any GUI.

## Consequences

- Focus order, focus indicators, accessible names and platform screen-reader
  integration come from the widget system, which already implements them
  correctly on both platforms.
- `QTest::mouseClick` and `QTest::keyClick` address a real widget, so GUI tests
  assert against asset identifiers and geometry rather than scene coordinates.
- The layout policy is unchanged from the specification: an equal-cell grid that
  maximises the smallest fitted-image area, with stable tie-breaking.
- If a future flow genuinely needs a scene graph, it can use Graphics View
  inside its own view without affecting the others — that is what the flow-view
  boundary is for.
