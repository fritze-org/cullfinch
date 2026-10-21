# 0012 — The crop tool is a third executable, and writes only new files beside a photo

Status: accepted · Date: 2026-10-18

## Context

Cropping a folder of photos one after another is a task of its own: no comparison, no marks, no
deletion — look at a photo, cut it, move on. Nothing in Cullfinch wrote image data until now. The
comparison views never crop ([design §7](../design.md)), and the wall viewer guarantees that it
cannot change a file ([decision 0011](0011-standalone-wall-viewer.md)).

The pieces were already there: `DirectoryScanner::enumerate` and `StemAssociationResolver` turn a
directory into photos as the browser sees them, and `PreviewLoader` with `QtImageService` decodes
them. Qt can encode a JPEG but drops its EXIF, and it cannot write XMP.

## Decision

`cullfinch-crop` is a third executable, built from `src/cropper/` and installed next to `cullfinch`
and `cullfinch-wall`. It owns no session, database, marks or Trash. It is not a mode of the wall
viewer, because that would end the wall's guarantee.

- **The originals are never changed.** An accepted crop of `IMG_1.jpg` goes to a new
  `IMG_1.crop.jpg` beside it. The original's extension is kept as it is spelt. Each crop is
  assembled under a hidden temporary name and renamed into place, so an interrupted write never
  leaves a half-written crop behind.
- **The photo is cropped upright, and its metadata travels with it.** The crop is drawn on the
  photo after its EXIF orientation, and it is written from the full-resolution file decoded the
  same way, at JPEG quality 95. EXIF, IPTC, XMP and the colour profile are copied with
  **exiv2**, which is a new vcpkg dependency (GPL-2.0-or-later). The copy changes three things:
  - the orientation is reset to 1, because the pixels are now stored upright
  - the pixel dimensions become the crop's
  - the embedded thumbnail is dropped, because it would still show the whole photo
- **A RAW companion gets the same crop as Camera Raw settings.** The crop is merged into
  `IMG_1.xmp` (Adobe's name for the sidecar) as `crs:HasCrop`, `crs:CropTop/Left/Bottom/Right`
  and `crs:CropAngle=0`. Every other property stays as it was. A sidecar that cannot be read is
  reported and left alone, because it may hold somebody's edits. The coordinates are measured in
  the RAW's stored, unrotated frame, as Camera Raw measures them: the crop is mapped back through
  the RAW's EXIF orientation, or through the JPG's when exiv2 cannot read the RAW.
- **A photo's crop is a fraction of the photo.** It is drawn on a screen-sized preview and applied
  to the full-resolution JPG and to the RAW. Only a normalised rectangle means the same on all
  three.
- **Runs resume.** A `*.crop.*` file never becomes a step of its own. A photo whose `.crop` JPG
  exists, or whose sidecar has `crs:HasCrop=True`, counts as done and is left out. `--redo` shows
  every photo again, and saving replaces the earlier crop. `--recursive` works as it does in the
  wall viewer. Neither flag is remembered.
- **RAW-only photos are skipped.** Nothing in Cullfinch decodes RAW. The window says how many it
  left out.
- **Accepting moves on at once.** Writing the crop means decoding and encoding the full-resolution
  file, so it runs on one worker thread, in order. A write that fails is reported in the window,
  and the window stays open at the end instead of closing over the failure.

## Consequences

- The package carries `libexiv2` and `libexpat`. The Qt deployment script picks them up, and the
  packaged-application suite starts the installed tool, so a package without them fails there.
- **darktable does not import the sidecar.** darktable keeps its own `IMG_1.CR2.xmp` and rewrites
  it from its library, so a crop merged into that file would be ignored and later dropped. Its
  Lightroom import (`src/develop/lightroom.c`) finds `IMG_1.xmp`, and reads `crs:Crop*` in the
  same unrotated frame. But it accepts only a sidecar whose `xmpMM:History` names "Lightroom" or
  "Camera Raw" as the software that wrote it. Recording another program's name would misstate
  where the file came from, so the sidecar carries no such history. Lightroom and Camera Raw read
  it; darktable users crop the RAW in darktable.
- **The RAW crop assumes the JPG and the RAW show the same frame.** That is true when the camera
  wrote both at full size and the same aspect. A camera set to record its JPG in another aspect
  (16:9 from a 3:2 sensor, say) frames the two differently, and the sidecar crop then lands
  somewhere else on the RAW.
- `IMG_1.crop.jpg` is an ordinary photo to the browser, the comparison flows and the wall, and it
  is meant to be: it is a new picture. Only the crop tool leaves it out.
