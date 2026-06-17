# arrow-hdf

Generic, schema-driven serialization of Apache Arrow objects to and from HDF5
files.

This is the **M-axis "Arrow ↔ technology" serializer for HDF5** in the Phlex
narrow-waist file-I/O design (`phlex-file-io-design.md`, sections 5–6). It is a
**pure** library: it depends only on **Apache Arrow** and **HDF5** (plus its own
neutral `Address` type). It does **not** depend on Phlex or Wire-Cell Toolkit.

## What it provides

- A generic `Address` (`arrow_hdf/Address.h`): a `/`-delimited path built from a
  list of components (each escaped to a safe link name) or from a verbatim path
  string. No domain structure is baked in — a caller maps richer concepts (e.g.
  Phlex `(layer, number)` cells + creator/product) onto the components itself.
  Includes reversible path-component escaping.
- A file-hierarchy descriptor (`arrow_hdf/Hierarchy.h`): a generic trie of path
  components (lexicographically sorted) marking which paths hold a table, built
  from the addresses found in a file by `scan()`.
- `Hdf5File` (`arrow_hdf/Hdf5File.h`): `write(arrow::Table, Address)`,
  `read(Address) → arrow::Table`, and a file `scan() → Hierarchy`, implemented
  with the raw HDF5 C API. Per-column typed datasets (rectangular where the
  schema allows — e.g. a dense frame's charge becomes a 2-D float dataset), with
  the exact `arrow::Schema` stored as an IPC blob for faithful reconstruction.
  See `docs/layout.md` for the schema → HDF5 convention.

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

When HDF5 is not in the Spack view, the configure step builds and tests the pure
core only and prints a status note; the HDF5 serializer and its round-trip tests
are skipped automatically.

## Tests

- `address`, `hierarchy` — pure core (always run).
- `roundtrip` — Arrow → HDF5 → Arrow equality (metadata-checked) for a flat
  table, a `list` column, a nested `list<struct>`, and a dense `fixed_size_list`
  table, plus `scan()`. (Runs when HDF5 is present.)

In-place check: a dense frame's charge round-trips to a 2-D `H5T_IEEE_F32LE`
dataset (NumPy/`h5py`-loadable); verified with `h5dump`.
