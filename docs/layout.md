# arrow-hdf: schema → HDF5 layout convention

This document defines how `arrow-hdf` maps an `arrow::Table` (and its schema) to
HDF5 structure, and the inverse. A guiding, **best-effort** goal is that the
HDF5 be useful *in place* — loadable with `h5py` as NumPy-like arrays — without
going back through Arrow, for the common rectangular cases. Faithful round-trip
is the hard requirement; in-place usability drives the *default* layout where
the schema allows it.

## 1. Addressing: groups

A table is written at an `Address` — a generic `/`-delimited path. An `Address`
is built either from a list of path **components** (each is escaped to a safe
HDF5 link name and the results joined with `/`) or from a pre-formed path string
(used verbatim). The leaf group holds the table's datasets and schema
attributes (below); arbitrary depth is supported. `arrow-hdf` attaches **no**
domain meaning to the components — a caller that wants structured addressing
(e.g. `(layer, number)` cells plus `creator`/`product`, as in Phlex) builds the
component list itself and interprets it on read.

Example: components `{"run","3","event","12","sigproc","frame"}` →
`/run/3/event/12/sigproc/frame`.

**Component sanitization.** When building from components, each is
percent-escaped: characters outside `[A-Za-z0-9._-]` (including `/`, NUL, and
`%`) become `%XX`. This is reversible (`path_unescape`) and forbids the HDF5
link separator and NUL. (Decimal integers escape to themselves.)

**Ordering.** `scan()` returns a generic path trie whose children are sorted
**lexicographically** by link name (so `"12"` sorts before `"2"`). `arrow-hdf`
does not impose numeric ordering — that is a consumer concern: a structured
reader that knows certain components are integers re-sorts numerically. (Plain
decimal names are used as-is; zero-padding was rejected as it needs an arbitrary
fixed width for unbounded numbers.)

## 2. Group-per-table vs rows

`arrow-hdf` writes one table at one address (a leaf group). A caller is free to
choose how finely it spreads tables across the path hierarchy. A group per fine
-grained node mirrors the hierarchy for browsing, but at large N it bloats HDF5
metadata and fights columnar efficiency; collapsing a high-cardinality level
into a row dimension of a shared table is more efficient. That trade-off is a
**caller** decision (it picks the addresses and table granularity); `arrow-hdf`
serves whatever addresses it is given.

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

## 6. Metadata and exact type reconstruction

Exact round-trip and in-place readability are served by two complementary
stores in the `product` group:

- **Faithful types.** The complete `arrow::Schema` (IPC-serialized) is stored as
  a `uint8` dataset `__arrow_schema__`. The reader deserializes it to recover
  every field's exact Arrow type — including the distinctions HDF5 cannot
  represent (`list` vs `fixed_size_list`, signedness, field nullability,
  field/schema metadata) — then reads each column's dataset(s) per that type.
  `arrow_num_rows` is stored as a group attribute.
- **In-place readability.** Schema-level metadata (e.g. `arrow.schema =
  "wc.frame"`, `wc.frame.ident`, `wc.frame.tbin`) is additionally mirrored as
  string attributes on the `product` group, so a plain `h5py`/`h5dump` user sees
  the semantic tags without parsing the IPC blob.

(Per-dataset Arrow-type attributes are a possible future in-place aid; they are
not needed for round-trip since `__arrow_schema__` is authoritative.)

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
