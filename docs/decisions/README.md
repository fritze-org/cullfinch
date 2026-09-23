# Decision records

One file per accepted decision. Records here either confirm a proposal from
[`../design.md`](../design.md) or state where the implementation deliberately
departs from it, and why.

| Record | Decision |
|---|---|
| [0001](0001-product-name-and-licence.md) | The product is Cullfinch, licensed GPL-3.0-or-later |
| [0002](0002-target-and-option-prefix.md) | Targets and options use a `cullfinch_` / `CULLFINCH_` prefix |
| [0003](0003-widgets-instead-of-graphics-items.md) | Comparison surfaces use widgets, not `QGraphicsItem`s |
| [0004](0004-generated-image-fixtures.md) | Input image fixtures are generated at run time, not committed |
| [0005](0005-coalesced-synchronous-autosave.md) | Draft autosave is coalesced and synchronous in the first release |
| [0006](0006-operations-blocked-flag.md) | Operation safety is a separate flag from pairing state |
| [0007](0007-wayland-primary-linux-backend.md) | Native Wayland is the primary Linux backend |
| [0008](0008-cpp23-language-standard.md) | The language standard is C++23, not the specified C++20 |
| [0009](0009-visual-regression-references.md) | Visual regression references are committed, and keyed by rendering environment |
| [0010](0010-preview-loader.md) | Decode bookkeeping lives in a `PreviewLoader`, not in `ImageCanvas` |
| [0011](0011-standalone-wall-viewer.md) | The standalone wall viewer is a second executable that decides nothing |
