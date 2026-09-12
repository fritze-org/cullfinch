# 0009 — Visual regression references are committed, and keyed by rendering environment

Status: accepted · Date: 2026-09-12

## Context

The GUI suites check behaviour: which signal fired, what the model holds, which action is enabled.
Nothing rendered a view and looked at it, so a changed margin, a changed paint order or a layout
that quietly collapses is invisible to them until somebody notices it by hand.

Comparing pixels needs a reference to compare against, and
[decision 0004](0004-generated-image-fixtures.md) says image fixtures are generated at run time
rather than committed. That decision is about *inputs*: generating them proves the deployed JPEG
plugin works and keeps filenames that cannot coexist on a case-insensitive volume out of the
repository. Neither reason applies to an *expected output*, and an expected output that the test
generates is not an expectation at all.

The obstacle is that a widget rendering is not a function of cullfinch alone. It also depends on
the Qt version, the platform plugin, the widget style, the device pixel ratio and — the awkward
one — which fonts the machine happens to have installed. Glyph rasterisation is not reproducible
across font stacks, and no tolerance separates "a different font" from "the label moved".

## Decision

Reference images are committed under `tests/fixtures/visual/`, and the suite pins everything about
the rendering it can:

- The widget style is Fusion, its palette is Qt's stated standard palette rather than the desktop
  theme's, the colour scheme is light and the default font's pixel size is fixed.
- Every compared widget is pinned to a size the case states, so a reference records the view and
  not the font metrics of the chrome around it.
- Regions whose content is glyphs — an `ImageCanvas` caption strip, the label band of a browser
  tile — are masked at the geometry the layout produced. The comparison keeps the geometry that
  decides where the text goes and drops the rasterisation.
- The photos are flat colours written by `TempCollection::addSolidJpeg`, not the name-stamped
  fixtures the behavioural suites use.

What survives that is genuinely font-dependent goes in a directory named after a digest of the Qt
version, platform plugin, style, device pixel ratio and resolved font metrics: the browser's status
bar, which is nothing but its three permanent labels, and the browser's grid, whose cells are a
fixed size but are split between thumbnail and label by the label's own height — so a taller font
draws a smaller thumbnail, whatever the mask hides. The wall and versus cases share one `shared/`
set.

That split is a claim about what reaches the pixels, and only one environment has been recorded so
far. A `shared/` case that turns out to differ elsewhere is a mismatch rather than a skip; the
remedy is to move it into the environment directory and record why, not to widen the tolerance.

An environment with no recorded references **skips** rather than fails, and writes what it rendered
to the artifact directory as a candidate. A directory that exists but is missing a case is a
failure: that is somebody adding a case without recording it.

## Consequences

- A layout regression in `WallView`, `VersusView` or the browser grid fails a test instead of
  reaching a release.
- The repository gains committed binaries. `.gitattributes` already marks `tests/fixtures/**` as
  binary and pre-commit already excludes it, so no text hook touches their bytes, and the
  1 MiB `check-added-large-files` limit is enforced on all files, including these.
- Recording is `CULLFINCH_UPDATE_VISUAL_REFERENCES=1`, which writes into the source tree so that
  approving a new rendering means reviewing a diff. A recording run cannot tell an intended
  redesign from the regression it was meant to catch; only the reviewer can.
- The suite runs on the primary Linux job alone, under `QT_QPA_PLATFORM=offscreen`. This is
  cullfinch's own widget rendering and nothing beyond it: it says nothing about desktop
  composition, window decorations or physical output colour, which is what the compositor and
  output smoke checks are for.
- A runner image that changes its fonts changes the digest, and the cases keyed to it would then
  skip — a gating job passing having compared nothing. The CI step therefore sets
  `CULLFINCH_REQUIRE_VISUAL_REFERENCES=1`, so that turns into a failure and somebody re-records
  rather than the suite quietly retiring. A developer's machine, which is not gating, still skips.
