# Third-party notices

HNwd is licensed under the [MIT License](LICENSE). It builds on the
third-party material below, under its own license.

HNwd has no bundled dependencies. The viewer `nwdview`, which is built by
default only when HNwd is built on its own (`HNWD_BUILD_VIEWER`), links GlazeUI
(<https://github.com/holmbom/GlazeUI>, MIT); CMake fetches it at configure
time, and it is not part of this repository or of the library.

---

## NwcIO

**Where:** HNwd's knowledge of the file format, in `doc/format.md` and in the
readers that follow it: `src/core/dbcache.cpp`, `container.cpp`,
`object_reader.cpp`, `guid_store.cpp`, `source_header.cpp`, `material.cpp`
(the class-185 body), `fragments.cpp`, `partition_props.cpp`,
`hierarchy.cpp`, `tab_reader.cpp` (which reads the same tree again from a
checkpoint), `geometry.cpp` (triangle and line records) and
`geometry_segment.cpp`; the XRef table in
`elements.cpp`; and the link checks and the tree expansion in `src/scene/`.

From HNwd's own analysis of files, within those readers: point, text, circle
and cylinder records in `geometry.cpp`, the reading of the material JSON in
`material.cpp`, and the pose classes with a full matrix in `fragments.cpp`.

HNwd's own: the decompression and the streaming reader (`inflate.cpp`,
`byte_reader.h`), the file mapping (`mapped_file.cpp`), the pose arithmetic
(`pose.cpp`), the spatial index built in `src/scene/spatial_index.cpp`, and
the viewer `nwdview` with its JPEG decoder. From HNwd's own analysis of
files: the partition node and the length unit (`partition.cpp`, class 32,
which NwcIO does not decode), the spatial tree (`spatial.cpp`, which NwcIO
does not read), the small packages in `elements.cpp` other than the XRef
table (`doc/research/element.md`), and the tools `nwdcheck` and `nwdsynth`.

HNwd started as a copy of NwcIO (<https://github.com/IOExtend/NwcIO>,
version 0.13.0), a .NET library that reads the same files. HNwd is written from
scratch in C++ with its own structure: no source file of NwcIO is part of this
repository. What HNwd takes from NwcIO is how the files are read. Its readers
follow the layouts that NwcIO's parsers and its `docs/format-notes.md`
describe, and HNwd's own format notes build on those.

NwcIO carries this notice:

```
MIT License

Copyright (c) 2026 mahach666

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## Trademarks

HNwd is not affiliated with Autodesk. Navisworks is a trademark of Autodesk,
Inc. HNwd contains no Autodesk code and was written from analysis of files
only.
