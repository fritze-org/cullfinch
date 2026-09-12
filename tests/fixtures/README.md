# Test fixtures

This directory holds one thing: the reference images the visual regression suite compares against,
under [`visual/`](visual/). Those are expected *outputs*, and an expected output a test generates
is not an expectation — see [decision 0009](../../docs/decisions/0009-visual-regression-references.md).

Every image fixture cullfinch tests *with* is **generated at run time** by
`tests/support/cullfinch/testsupport/TempCollection`, not committed here. See
[decision 0004](../../docs/decisions/0004-generated-image-fixtures.md) for the
reasoning; in short:

- generating JPEGs with `QImageWriter` exercises the deployed image-format
  plugin on every single run, so a missing plugin fails loudly and immediately;
- RAW companions are arbitrary opaque bytes, because cullfinch never decodes
  them — the tests assert those bytes survive a file operation unchanged, which
  proves nothing about any RAW format and is not meant to;
- filenames that differ only by case cannot coexist on a case-insensitive
  volume, so committing them would make the repository unusable there. They are
  created at run time, and `TempCollection::supportsCaseDistinctNames()` turns
  an incapable filesystem into an explicit skip.

If real-camera fixtures are added later — small, clearly licensed files for EXIF
orientation and embedded ICC profiles — they belong here, and `.gitattributes`
already marks this tree as binary so no text hook touches their bytes.
