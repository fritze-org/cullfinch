# Visual regression references

Reference images for `tst_visual_regression`, the suite that renders the production views
offscreen and compares the pixels. They are the one place in this repository where committed
binaries are the point rather than an accident — see
[decision 0009](../../../docs/decisions/0009-visual-regression-references.md).

## Layout

| Directory | Holds |
|---|---|
| `shared/` | Renderings that draw no text, or whose text is masked out. One set, used everywhere |
| `<platform>-<style>-<digest>/` | Renderings a font still reaches, recorded per environment |

The directory name is derived from everything a rendering depends on that is not cullfinch: the
Qt version, the platform plugin, the widget style, the device pixel ratio and the resolved default
font's metrics. Two machines that agree on all of those share a directory; one that does not gets
its own, and until somebody records it the font-dependent cases skip rather than fail. The inputs
behind a digest are printed by the suite and written to `environment.txt` — both next to the
failure artifacts and, for a recorded set, beside the images themselves.

### What is recorded now

| Case | Where | Why |
|---|---|---|
| `wall-surface`, `wall-surface-after-elimination`, `versus-panes` | `shared/` | Photos and masked caption strips inside widgets pinned to a stated size. No font reaches the compared pixels |
| `browser-grid` | per environment | The cell is a fixed size, but the delegate splits it between thumbnail and label by the label's own height, so a taller font draws a smaller thumbnail |
| `browser-status-bar-writable`, `browser-status-bar-read-only` | per environment | Nothing but the three permanent labels |

The recorded environment is the primary Linux CI job: Qt 6.11.1, `offscreen`, Fusion, device pixel
ratio 1, DejaVu Sans at 13px. A `shared/` case that turns out to differ somewhere else is a
mismatch rather than a skip; if that happens, move it into the environment directory and say in its
comment what the font reaches.

## Recording

```sh
CULLFINCH_UPDATE_VISUAL_REFERENCES=1 ctest --preset dev-fast --label-regex visual
git diff --stat tests/fixtures/visual        # the approval step is reviewing this
```

Recording writes into this directory, so a new or changed reference arrives as a reviewable
change rather than as an artifact somebody copies into place by hand. **Look at the images before
committing them**: a recording run cannot tell an intended redesign from the regression it was
supposed to catch.

## Reading a failure

A case that differs writes `<case>-actual.png`, `<case>-expected.png` and `<case>-diff.png` to
`$CULLFINCH_ARTIFACT_DIR/visual` — the same directory the session helpers publish their logs
through, so CI uploads them with the rest of the failure evidence. Differing pixels are magenta in
the diff; everything else is washed out so the difference reads as "here, in this view".

## Other environment variables

| Variable | Effect |
|---|---|
| `CULLFINCH_REQUIRE_VISUAL_REFERENCES` | An environment with no recorded references fails instead of skipping. Set on the CI step, so a runner image that changes its fonts fails here rather than passing having compared nothing |
| `CULLFINCH_VISUAL_REFERENCE_DIR` | Read references from somewhere other than this directory |
