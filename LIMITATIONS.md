# HNwd — limitations

What HNwd does not decode, does not know, or handles in a restricted way.
These are not planned tasks: most of them can only be settled by a file that
shows the case. Where an interpretation does not hold, HNwd returns an error
rather than a guess.

Work that is planned is in [TODO.md](TODO.md). What the format looks like, and
what has been tested against files, is in [doc/format.md](doc/format.md).

## The format: what is not decoded

- **The length unit.** Codes 0–3 are metres, centimetres, millimetres and
  feet. Codes above 3 have not been seen.
- **Text (class 103)** is returned as a string, a box, a rotation and a font
  index, together with the names and sizes of the fonts in the list. That the
  index points into that list is not confirmed. The glyphs are not decoded;
  the viewer draws the text with its own font.
- **Circles and cylinder surfaces** are always divided into 32 steps starting
  in the direction a. Where Navisworks starts its division is not known, and o
  has only been seen as zero.
- **Quantised positions** give `Unsupported`. There are no examples to decode
  from.
- **Class 92 in the tree** is decoded only in the one size that has been seen
  (96, also in format version 451), and its content is not known.
- **The flags** on the appearance (class 55) and on the id classes 66 and 70,
  and the number last in a point record: their meaning is not known. The flags
  in classes 55, 66 and 70 are read and stored in core; the number after flag
  64 and the point record's number are read, checked against 0 and 1, and not
  stored. None of them are returned through `Document`. Format version 451 has
  no other values; there, flag 64 only appears together with 32, and the point
  record's number is 0 only in records without colour.
- **UV coordinates and colour on lines** are only tested against synthetic
  records. For quantised UV, the order of the four numbers of the interval and
  the formula are assumed.
- **The material's image** is returned with scale, offset and rotation as they
  are stored; how they apply to the UV values, and the unit of the angle, are
  not confirmed. The material in the tree's class 200 is read and checked but
  not returned, and what uses the materials in `LcReMaterialElement` is not
  known.
- **Classes 79 and 200 in the tree** are not returned. Class 79 in the
  property streams and class 200 occur in format version 451 and are parsed
  there to their exact end, but what they mean is not tested. The nodes'
  transforms and the centre of the geometry are not returned through
  `Document` either; `nwdcheck` reads them from core.
- **The fields in class 32 and class 27** whose meaning is not known, see
  [doc/research/klass32.md](doc/research/klass32.md). In class 27, numbers 3–8
  are confirmed as the light's `specularColor` and the last three as its
  direction; the first three and the coordinate system are not confirmed.
- **The same element in several places in the tree** (the same element index
  under different parents) is returned as stored, without interpretation.
- **The small packages** have several fields whose role is unknown and which
  are not returned, see [doc/research/element.md](doc/research/element.md):
  whether a view is orthographic, the roles of the background colours, the
  third axis of the grids, the headlight, the speed and the culling, the
  section planes' `mode` and flags, which side of a section plane is kept, and
  in the clash tests the tests' numbers and the models' `mode`. The form of
  the empty packages (`LcOpCommonSchemas`, `LcOpZoneElement` and the others)
  when they have content is not known, nor are the records in `LcOpXRefTable`:
  the parsers in core and `nwddump` give `Unsupported` for both, while
  `Document` returns the XRef table as stored and does not read the empty
  packages. The clash tests, the headlight, the speed, the culling,
  `LcOpShadOverridesElement` and `LcOpTextureSpaceElement` are parsed in core
  and shown by `nwddump --elements`, but are not returned through `Document`.
- **The 40 raw bytes** in `LcOpNwdSerial` and `LcOpNwdGeometryCompress` are
  not decoded; format version 451 has the same bytes as the example `.nwd`.
- **External point clouds.** No example shows how a file points to them.
- **Strings that are not UTF-8** are returned as stored. No example has any,
  so which encoding they would have is not known.

## Known weaknesses

- **`Options::max_tree_nodes`** is 8 million nodes by default. Whether a large
  merged model has more is not known; it then gives `LimitExceeded`, and the
  limit has to be raised.
- **Two packages with the same name** among those `Document::open()` uses give
  `Corrupt`. Whether a real file can have that is not known.
- **The format version** in the header is not checked: the example files have
  version 448, and another version is read with the same rules and gives an
  error where it differs.
- **Paths over 260 characters on Windows** require the caller to give them
  with the prefix `\\?\`.
