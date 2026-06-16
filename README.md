# arrow-hdf

Generic, schema-driven serialization of Apache Arrow objects to and from HDF5
files.

This is the **M-axis "Arrow ↔ technology" serializer for HDF5** in the Phlex
narrow-waist file-I/O design (`phlex-file-io-design.md`, sections 5–6). It is a
**pure** library: it depends only on **Apache Arrow** and **HDF5** (plus its own
neutral `Address` type). It does **not** depend on Phlex or Wire-Cell Toolkit.

## What it provides

- A Phlex-neutral `Address` (`arrow_hdf/Address.h`): a sequence of
  `(layer, number)` cells plus creator and product names, mapping to a file path
  like `/run/3/event/12/<creator>/<product>`. Includes reversible path-component
  escaping.
- A file-hierarchy descriptor (`arrow_hdf/Hierarchy.h`): the ordered layer names
  and the numeric-sorted cell tree with per-cell product locations, built from
  the addresses found in a file. Consumed by the Phlex read side.
- *(pending HDF5)* `write(arrow::Table, Address)`, `read(Address) → arrow::Table`,
  and a file `scan() → Hierarchy`, with the schema → HDF5 layout convention in
  `docs/layout.md`.

## Layout

- `arrow_hdf/` — headers and sources.
  - `arrow_hdf_core` (always built, no external deps): `Address`, `Hierarchy`.
  - `arrow_hdf` (built when Arrow **and** HDF5 are found): the HDF5 serializer.
- `docs/layout.md` — the schema → HDF5 layout convention.
- `test/` — CTest unit tests (core always; round-trip when HDF5 is present).

## Build

Pins the Spack GCC 15 toolchain via `$CC`/`$CXX` (set by the umbrella `.envrc`):

```bash
export CC="$(spack -e wcph location -i gcc@15)/bin/gcc"
export CXX="$(spack -e wcph location -i gcc@15)/bin/g++"

cmake --preset default -S source/arrow-hdf -B builds/arrow-hdf
cmake --build builds/arrow-hdf
ctest --test-dir builds/arrow-hdf
```

When HDF5 is not yet in the Spack view, the configure step builds and tests the
pure core only and prints a status note; the HDF5 serializer and its round-trip
tests are skipped automatically.

## Dependency status

HDF5 must be added to the Spack `wcph` environment (and the `local/` view
regenerated) before the HDF5 serializer can build — see the project notes.
