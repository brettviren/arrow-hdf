# arrow-hdf: schema → HDF5 layout convention

This document defines how `arrow-hdf` maps an `arrow::Table` (and its schema) to
HDF5 structure, and the inverse. A guiding, **best-effort** goal is that the
HDF5 be useful *in place* — loadable with `h5py` as NumPy-like arrays — without
going back through Arrow, for the common rectangular cases. Faithful round-trip
is the hard requirement; in-place usability drives the *default* layout where
the schema allows it.

## 1. Addressing: groups

A product is written at an `Address` = a sequence of `(layer, number)` cells
plus `creator` and `product` names. It maps to the HDF5 group path

```
/<layer0>/<number0>/<layer1>/<number1>/ … /<creator>/<product>
```

e.g. `/run/3/event/12/sigproc/frame`. The `product` group holds the table's
datasets and the schema attributes (below). The cell sequence may be empty (a
product at the job root: `/<creator>/<product>`). Address depth is arbitrary —
stores can land at any layer.

**Component sanitization.** Every non-numeric link name (layer, creator,
product) is percent-escaped: characters outside `[A-Za-z0-9._-]` (including `/`,
NUL, and `%`) become `%XX`. This is reversible (`path_unescape`) and forbids the
HDF5 link separator and NUL. Cell numbers render as plain decimal (always safe).

**Numeric ordering.** Cell group names are the decimal number. HDF5/`h5py`
iterate links lexicographically by default, so `"12"` sorts before `"3"`. We do
not rely on link order for correctness: the file `scan()` parses cell numbers
and **sorts them numerically** when building the `Hierarchy`, so the Phlex read
side always sees numeric order. (Zero-padding was rejected — it needs an
arbitrary fixed width for unbounded run/event numbers.) The numeric cell value
is also stored as an attribute on the cell group for robustness.

## 2. The grouping layer (group-per-cell vs rows)

A group per cell mirrors the hierarchy for browsing, but at large N it bloats
HDF5 metadata and fights columnar efficiency. The **grouping layer** — the depth
at and below which each cell becomes its own group (vs. cells contributing rows
to a shared dataset) — is a deliberate, configurable choice. Default:
group-per-cell at every layer (simplest, best for browsing); a future option can
collapse a high-cardinality inner layer into a row dimension.

## 3. Columns → datasets

Each top-level Arrow column becomes one HDF5 dataset in the `product` group,
named by the field name (sanitized). Mapping by Arrow type:

| Arrow type | HDF5 dataset |
|---|---|
| primitive (int*/uint*/float*/bool) | 1-D dataset of the matching native type, length = num rows |
| `fixed_size_list<T>[k]` | **2-D** dataset `[num_rows, k]` of `T` — rectangular, directly NumPy-loadable |
| `list<T>` (variable) | values dataset + int64 `offsets` dataset (Arrow layout), or HDF5 vlen (see §4) |
| `struct<...>` | a subgroup; one dataset per struct field |
| `list<struct<...>>` | a subgroup with the struct fields as values datasets + a shared `offsets` dataset |
| `utf8` / `large_utf8` | HDF5 variable-length UTF-8 string dataset |
| `large_binary` (blob) | see §5 |
| dictionary | materialized (decoded) to its value type; dictionary kept as an attribute |

Nulls: a column with a validity bitmap also writes a boolean `*.valid` dataset
(or uses an HDF5 fill/NaN convention for floats) so nullability round-trips.

The **dense `wc.frame`** case is the in-place sweet spot: the
`wc.trace.charge : fixed_size_list<float32>[nticks]` column becomes a 2-D
`[nchannels, nticks]` float32 dataset — a waveform image a user loads directly
with `h5py` as a NumPy array.

## 4. Nested / variable-length columns

These do not map to a single rectangular dataset, so they use a faithful
encoding: the Arrow child **values** buffer and the **offsets** buffer are
written as separate datasets (`<field>` + `<field>.offsets`), mirroring Arrow's
own representation. `list<struct<...>>` (e.g. `wc.depo.priors`) becomes a
subgroup holding one values dataset per struct field plus the shared
`<field>.offsets`. This is faithful and reconstructable; it is *not* the
in-place-friendly form, and that tension is accepted per the issue.

## 5. Opaque blobs (`large_binary`)

A `large_binary` column (the Arrow-opaque escape-hatch tier) is written as a
vlen `uint8` dataset (one entry per row), or a values+offsets pair. The
originating type tag travels in the schema metadata (§6). No interpretation is
attempted; the bytes are stored verbatim. Alignment for in-place typed reads is
the caller's concern (see the `wire-cell-arrow` tensor alignment contract).

## 6. Metadata → attributes

- Schema-level metadata (e.g. `arrow.schema = "wc.frame"`, `wc.frame.ident`,
  `wc.frame.time`) → attributes on the `product` group.
- Field-level metadata → attributes on the corresponding dataset.
- The Arrow type of each column (its `ToString()` / a compact type tag) is
  stored as a dataset attribute so the reader can reconstruct the exact Arrow
  type, including the distinctions HDF5 cannot represent (e.g. `list` vs
  `fixed_size_list`, signedness, dictionary).

A reader keys off `arrow.schema` and the per-dataset type tags to rebuild the
`arrow::Table` exactly.

## 7. Concurrency contract

libhdf5 is not internally thread-safe (unless built thread-safe). `arrow-hdf`
does **no internal locking**: callers must serialize access to a given file.
In Phlex, the output node's `concurrency::serial` provides this. Distinct files
(or distinct technology libraries) may be written concurrently.

## 8. Round-trip guarantee

`read(write(table)) == table` for: flat primitive tables, `fixed_size_list`
(dense), `list<T>`, `struct`, `list<struct<...>>`, `utf8`, and `large_binary`,
including schema/field metadata and null masks. This is verified by the
round-trip CTest suite (enabled when HDF5 is present).
