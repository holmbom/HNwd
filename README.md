**English** · [Svenska](README.sv.md)

# HNwd

A C++20 library for reading Navisworks files (`.nwd`, `.nwc`) without
Navisworks, with no dependencies. One public header, `include/HNwd.h`.

**Status: early development, version 0.2.0.** [TODO.md](TODO.md)
says what is left.

## What it reads

- **Geometry:** triangles, lines, points, circles, cylinder surfaces and text
  (string, box and orientation; not glyphs), with normals, colours and UV.
- **Placement in the world:** each fragment's pose, with `world_point()` and
  `world_normal()`, and its world box.
- **Materials:** colours per look, and the material description with its
  base colour, embedded image (JPEG, PNG) and UV transform.
- **The object tree** with names, tabs and properties: all of it in memory, or
  as a skeleton whose properties are read per node when they are needed
  (`TreeContent::Structure` with `PropertyReader`).
- **The models** in a file with several, also models inside models, with
  source, loader and length unit, and the model a node or a fragment belongs to.
- **A spatial tree** over the fragments, with a world box and a primitive
  count per node, for choosing what to load by level of detail: the file's
  own, or one HNwd builds.
- **Views:** the current view, the home view and the saved views in folders,
  with section planes and redlines. **Grids** with lines and levels, fonts,
  scene materials, background, lights, the file's identity and the loader's
  log.
- **Threads:** the const calls may run from several threads, each thread
  reading geometry or properties has its own `GeometryReader` or
  `PropertyReader`, and `load_tree()` may run while others read.
- **Progress and cancel:** the long calls (`load_geometry()`,
  `load_fragments()`, `load_spatial()`, `load_tree()`) take a function that
  is told how far they have come and can stop them (`Status::Cancelled`).
- **Memory:** `memory_bytes()` says what the document holds. The file is
  memory-mapped and read through its directory; geometry is decompressed piece
  by piece through a cache with a limit, so a file of several GB costs only
  what is used.

## Reading a file

```cpp
#include "HNwd.h"

HNwd::Document doc;
if (doc.open("model.nwd") != HNwd::Status::Ok)
    return 1;                                   // doc.last_error() says why
if (doc.load_fragments() != HNwd::Status::Ok)   // placement, looks, materials
    return 1;

HNwd::Fragment f;
HNwd::Mesh mesh;
for (size_t i = 0; i < doc.fragment_count(); ++i) {
    doc.fragment(i, f);
    if (doc.fragment_mesh(i, mesh) != HNwd::Status::Ok)
        continue;                               // e.g. quantised positions: Unsupported
    if (mesh.kind == HNwd::MeshKind::Text || mesh.positions.empty())
        continue;
    double world[3];
    HNwd::world_point(f.pose, &mesh.positions[0], world);   // first vertex
}

HNwd::Unit unit;
if (doc.length_unit(unit) == HNwd::Status::Ok) {
    // unit.metres_per_unit converts the file's coordinates to metres
}

// The tree without properties; a node's properties are read when needed.
if (doc.fragment_count() > 0 &&
    doc.load_tree(HNwd::TreeContent::Structure) == HNwd::Status::Ok) {
    const uint32_t n = doc.node_of_fragment(0);   // the object fragment 0 belongs to
    HNwd::Node node;
    doc.node(n, node);                            // node.name
    HNwd::PropertyReader reader;
    HNwd::NodeProperties props;
    if (reader.open(doc) == HNwd::Status::Ok && reader.read(n, props) == HNwd::Status::Ok) {
        // props.tabs and props.properties
    }
}
```

## Tools and viewer

Built when HNwd is built on its own; not part of the library.

- `nwddump <file>` shows the sections, the directory and each package;
  `--spatial`, `--units`, `--elements` and `--partition` decode parts of it.
- `nwd2obj <in> <out.obj> [--local] [--offset x y z] [--max-fragments N] [--no-mtl]`
  writes the geometry as OBJ in world coordinates, with materials and images.
- `nwdcheck <file> --poses` checks every fragment's pose against the tree's
  transforms.
- `nwdsynth <out.nwd> --fragments N --records M --nodes K` writes a valid
  synthetic file of any size, for testing at scale.
- `nwdview [file]` shows a model in 3D, loaded in worker threads: whatever is
  at least a few pixels on screen is built, within a memory budget for RAM
  (`--budget MB`) and one for the graphics card (`--gpu-budget MB`), both set
  from the machine by default. A click on an object shows its
  name and properties; materials' images, text, grids and saved views are
  drawn; F2 walks in first person. `--view <name>` starts in a saved view,
  and `nwdview <file> --png <image.png>` draws the model to an image and exits.

## Building

Requirements: CMake 3.20+, Ninja and a C++20 compiler. Developed on Windows with
MSYS2 UCRT64 (GCC); the code is written for Windows, Linux and macOS.

```sh
cmake -B build -G Ninja
cmake --build build
./build/tests/HNwd_tests      # from the repository root
```

- Without a build type the build is Release. `-DBUILD_SHARED_LIBS=ON` builds
  shared libraries.
- `HNWD_BUILD_TESTS` (tests and tools), `HNWD_BUILD_VIEWER` (the viewer) and
  `HNWD_INSTALL` (install rules and the CMake package) are on when HNwd is
  built on its own and off when another project pulls it in with FetchContent
  or `add_subdirectory`. Such a project gets only the library: link
  `HNwd::HNwd`.
- The viewer is built on [GlazeUI](https://github.com/holmbom/GlazeUI), which
  CMake fetches when it configures; it needs CMake 3.24.
  `-DHNWD_BUILD_VIEWER=OFF` builds the tests and tools without it, and without
  a network.
- `cmake --install` gives the libraries, `HNwd.h` and a CMake package:
  `find_package(HNwd)` gives `HNwd::HNwd`.

The test files are not in the repository. They are a public example project,
downloaded from
<https://download.bimcollab.com/support/ZOOM/BIMcollab_Navisworks-example-file.zip>
and unpacked into `nwd/bimcollab/`. Tests whose file is missing print `SKIP`;
[tests/test_data.h](tests/test_data.h) has the commands and the paths.

## Limits

- Quantised positions are not decoded; such records give `Unsupported`.
- Text gives its string, box, orientation and font index, not its glyphs.
- Length unit codes other than 0–3 (metre, centimetre, millimetre, foot) are
  not known.
- Fields whose meaning is not known are not given out: see
  [LIMITATIONS.md](LIMITATIONS.md), [doc/format.md](doc/format.md) and
  [doc/research/element.md](doc/research/element.md).
- Linux and macOS are written for but not yet tested.

## Documentation

- [README.sv.md](README.sv.md): the description in Swedish.
- [CLAUDE.md](CLAUDE.md): the developer guide: modules, rules and the build.
  Written for both people and AI coding tools. In Swedish.
- [doc/format.md](doc/format.md): the file format, with what is tested.
- [doc/CHANGELOG.md](doc/CHANGELOG.md) and [TODO.md](TODO.md): what is done and
  what is left.
- [LIMITATIONS.md](LIMITATIONS.md): what is not decoded, and known weaknesses.

## Origin

HNwd started as a copy of [NwcIO](https://github.com/IOExtend/NwcIO) by
mahach666 (MIT), a .NET library that reads the same files. HNwd is written from
scratch in C++ and shares no source files with it; its knowledge of the file
format builds on NwcIO's parsers and format notes. See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## License

[MIT](LICENSE) © 2026 Erik Holmbom

Not affiliated with Autodesk. Navisworks is a trademark of Autodesk, Inc.
