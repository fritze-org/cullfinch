# 0004 — Input image fixtures are generated at run time

Status: accepted · Date: 2026-09-10

## Context

The specification calls for synthetic JPEGs plus small, clearly licensed
orientation and colour fixtures checked into the repository, and separately
requires that case-colliding filenames are generated at run time rather than
committed.

## Decision

`tests/support/cullfinch/testsupport/TempCollection` writes every image fixture
at run time with `QImageWriter`. RAW companions are arbitrary opaque bytes.
Nothing binary is committed as an *input*.

Expected *outputs* are a different question, settled separately by
[decision 0009](0009-visual-regression-references.md): the visual regression
suite's reference images are committed, because a reference a test generates is
not an expectation.

## Consequences

- Every test run exercises the *deployed* JPEG plugin, in both directions. A
  missing image-format plugin fails the fixture builder immediately rather than
  producing a confusing decode error later.
- There are no binary files for the whitespace, formatting or large-file hooks
  to mishandle, and no licence questions about committed photographs.
- Case-colliding and Unicode-normalisation fixtures are created only on volumes
  that can actually hold them; `supportsCaseDistinctNames()` turns the rest into
  an explicit `QSKIP` rather than a failure.
- **Gap:** camera-produced EXIF orientation and embedded ICC profiles are not
  covered by generated fixtures. Orientation handling is delegated to
  `QImageReader::setAutoTransform`, and the colour policy is documented as
  uncalibrated. Adding a few tiny, clearly licensed real-camera fixtures is the
  right follow-up before claiming orientation correctness.
