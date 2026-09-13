# 0010 — Decode bookkeeping lives in a PreviewLoader, not in ImageCanvas

Status: accepted · Date: 2026-09-13

## Context

`ImageCanvas` did two jobs. It painted a photo and interpreted the gestures that eliminate
it — [decision 0003](0003-widgets-instead-of-graphics-items.md) is about that half — and it also
ran the decode side: issuing requests at the right pixel size, recognising which arriving result
belongs to the photo currently on screen, and telling its host when a preview had arrived.

The second half is the one with the sharp edges. A result carries a request id, a member id and a
generation, and all three matter: a refinement that arrives after the canvas has moved on must not
be drawn, and readiness reported for the wrong photo is not cosmetic. Decision input stays disabled
until every required preview is ready, so "ready" arriving early lets somebody eliminate a photo
they cannot see.

None of that was reachable from a test without constructing a widget, showing it, and driving a real
event loop through a GUI suite. The bookkeeping was pinned down only indirectly, as a side effect of
tests about clicking on walls and versus panes.

SonarCloud's `cpp:S1448` had also flagged the class for its method count. That finding is a symptom
and not the reason for this split: cutting exactly one method to get under a threshold would have
bought nothing.

## Decision

`PreviewLoader` owns the decode side: the current source, the two in-flight request ids, the fitted
and full-resolution images, the native size, readiness, and any reported error. It is a `QObject`
rather than a widget, and it is told how many device pixels to ask for — deriving that from a
widget's size and device pixel ratio stays in the canvas, which is the only thing that knows it.

`ImageCanvas` owns one, and hosts reach it through `canvas->preview()`:

| Was | Is |
|---|---|
| `canvas->isReady()` | `canvas->preview()->isReady()` |
| `canvas->hasError()`, `canvas->errorText()` | the same two on `canvas->preview()` |
| `canvas->isFullResolutionReady()` | `canvas->preview()->isFullResolutionReady()` |
| `ImageCanvas::readinessChanged` | `PreviewLoader::readinessChanged` |

Requests in flight for a previous photo are disowned rather than cancelled: results are matched
against the member id and generation the loader currently holds, which is the same guard that was
there before and covers the results a cancellation would race.

## Consequences

- The decode side is testable without a widget. `tests/unit/tst_preview_loader.cpp` states what
  readiness means directly: a result for another photo or a superseded generation is ignored, a
  decode failure reports an error instead of readiness, and the full-resolution image is separate
  from the readiness a decision depends on.
- The call sites are one hop longer. That is the point: `preview()->isReady()` says which half of
  the canvas is being asked, where `isReady()` on a widget did not.
- `ImageCanvas` keeps presentation, marks, gestures and painting, and reads its pixels from the
  loader. Five declared methods move out with the bookkeeping, which takes it under the
  `cpp:S1448` limit and retires that finding as a side effect rather than as the point.
