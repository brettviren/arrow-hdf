#include "arrow_hdf/Hdf5File.h"

#include <arrow/array/util.h>
#include <arrow/io/memory.h>
#include <arrow/ipc/api.h>
#include <arrow/util/key_value_metadata.h>

#include <cstring>
#include <string>
#include <vector>

namespace arrow_hdf {

namespace {

constexpr const char* kSchemaDataset = "__arrow_schema__";
constexpr const char* kNumRowsAttr = "arrow_num_rows";
constexpr const char* kValidSuffix = ".__valid__";
constexpr const char* kOffsetsSuffix = ".__offsets__";

// --- RAII for HDF5 ids -----------------------------------------------------
struct Hid {
    hid_t id{H5I_INVALID_HID};
    herr_t (*closer)(hid_t){nullptr};
    Hid() = default;
    Hid(hid_t i, herr_t (*c)(hid_t)) : id(i), closer(c) {}
    Hid(Hid&& o) noexcept : id(o.id), closer(o.closer) { o.id = H5I_INVALID_HID; }
    Hid& operator=(Hid&&) = delete;
    Hid(const Hid&) = delete;
    ~Hid() { if (id >= 0 && closer) closer(id); }
    operator hid_t() const { return id; }
    bool ok() const { return id >= 0; }
};

arrow::Status io(const std::string& msg) { return arrow::Status::IOError("arrow-hdf: " + msg); }

// --- low-level dataset/attribute helpers -----------------------------------

arrow::Status write_dataset(hid_t grp, const std::string& name, hid_t h5type,
                            const std::vector<hsize_t>& dims, const void* data)
{
    Hid space(H5Screate_simple(static_cast<int>(dims.size()), dims.data(), nullptr), H5Sclose);
    if (!space.ok()) return io("H5Screate_simple " + name);
    Hid dset(H5Dcreate2(grp, name.c_str(), h5type, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Dclose);
    if (!dset.ok()) return io("H5Dcreate2 " + name);
    // A zero-length dataset must not be written (NULL selection); creation suffices.
    hsize_t total = 1;
    for (auto d : dims) total *= d;
    if (total > 0 && data) {
        if (H5Dwrite(dset, h5type, H5S_ALL, H5S_ALL, H5P_DEFAULT, data) < 0)
            return io("H5Dwrite " + name);
    }
    return arrow::Status::OK();
}

// Read a 1-D dataset's length (element count of dim 0).
arrow::Result<hsize_t> dataset_dim0(hid_t grp, const std::string& name)
{
    Hid dset(H5Dopen2(grp, name.c_str(), H5P_DEFAULT), H5Dclose);
    if (!dset.ok()) return io("H5Dopen2 " + name);
    Hid space(H5Dget_space(dset), H5Sclose);
    int rank = H5Sget_simple_extent_ndims(space);
    std::vector<hsize_t> dims(rank > 0 ? rank : 1, 0);
    H5Sget_simple_extent_dims(space, dims.data(), nullptr);
    return dims.empty() ? 0 : dims[0];
}

arrow::Status read_dataset(hid_t grp, const std::string& name, hid_t h5type, void* out)
{
    Hid dset(H5Dopen2(grp, name.c_str(), H5P_DEFAULT), H5Dclose);
    if (!dset.ok()) return io("H5Dopen2 " + name);
    Hid space(H5Dget_space(dset), H5Sclose);
    if (H5Sget_simple_extent_npoints(space) == 0) return arrow::Status::OK();
    if (H5Dread(dset, h5type, H5S_ALL, H5S_ALL, H5P_DEFAULT, out) < 0)
        return io("H5Dread " + name);
    return arrow::Status::OK();
}

bool has_link(hid_t loc, const std::string& name)
{
    return H5Lexists(loc, name.c_str(), H5P_DEFAULT) > 0;
}

arrow::Status write_string_attr(hid_t loc, const std::string& name, const std::string& value)
{
    Hid st(H5Tcopy(H5T_C_S1), H5Tclose);
    H5Tset_size(st, H5T_VARIABLE);
    H5Tset_cset(st, H5T_CSET_UTF8);
    Hid space(H5Screate(H5S_SCALAR), H5Sclose);
    Hid attr(H5Acreate2(loc, name.c_str(), st, space, H5P_DEFAULT, H5P_DEFAULT), H5Aclose);
    if (!attr.ok()) return io("H5Acreate2 " + name);
    const char* p = value.c_str();
    if (H5Awrite(attr, st, &p) < 0) return io("H5Awrite " + name);
    return arrow::Status::OK();
}

arrow::Status write_int64_attr(hid_t loc, const std::string& name, std::int64_t value)
{
    Hid space(H5Screate(H5S_SCALAR), H5Sclose);
    Hid attr(H5Acreate2(loc, name.c_str(), H5T_NATIVE_INT64, space, H5P_DEFAULT, H5P_DEFAULT), H5Aclose);
    if (!attr.ok()) return io("H5Acreate2 " + name);
    if (H5Awrite(attr, H5T_NATIVE_INT64, &value) < 0) return io("H5Awrite " + name);
    return arrow::Status::OK();
}

arrow::Result<std::int64_t> read_int64_attr(hid_t loc, const std::string& name)
{
    Hid attr(H5Aopen(loc, name.c_str(), H5P_DEFAULT), H5Aclose);
    if (!attr.ok()) return io("H5Aopen " + name);
    std::int64_t v = 0;
    if (H5Aread(attr, H5T_NATIVE_INT64, &v) < 0) return io("H5Aread " + name);
    return v;
}

// vlen UTF-8 strings (one per row); nulls stored as "" + validity dataset.
arrow::Status write_vlen_strings(hid_t grp, const std::string& name,
                                 const std::vector<std::string>& vals)
{
    Hid st(H5Tcopy(H5T_C_S1), H5Tclose);
    H5Tset_size(st, H5T_VARIABLE);
    H5Tset_cset(st, H5T_CSET_UTF8);
    std::vector<const char*> ptrs(vals.size());
    for (std::size_t i = 0; i < vals.size(); ++i) ptrs[i] = vals[i].c_str();
    hsize_t dim = vals.size();
    Hid space(H5Screate_simple(1, &dim, nullptr), H5Sclose);
    Hid dset(H5Dcreate2(grp, name.c_str(), st, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Dclose);
    if (!dset.ok()) return io("H5Dcreate2 " + name);
    if (dim > 0 && H5Dwrite(dset, st, H5S_ALL, H5S_ALL, H5P_DEFAULT, ptrs.data()) < 0)
        return io("H5Dwrite " + name);
    return arrow::Status::OK();
}

arrow::Status read_vlen_strings(hid_t grp, const std::string& name, std::int64_t n,
                                std::vector<std::string>& out)
{
    out.assign(n, std::string());
    if (n == 0) return arrow::Status::OK();
    Hid dset(H5Dopen2(grp, name.c_str(), H5P_DEFAULT), H5Dclose);
    if (!dset.ok()) return io("H5Dopen2 " + name);
    Hid st(H5Tcopy(H5T_C_S1), H5Tclose);
    H5Tset_size(st, H5T_VARIABLE);
    H5Tset_cset(st, H5T_CSET_UTF8);
    std::vector<char*> ptrs(n, nullptr);
    if (H5Dread(dset, st, H5S_ALL, H5S_ALL, H5P_DEFAULT, ptrs.data()) < 0) return io("H5Dread " + name);
    for (std::int64_t i = 0; i < n; ++i) out[i] = ptrs[i] ? std::string(ptrs[i]) : std::string();
    Hid space(H5Dget_space(dset), H5Sclose);
    H5Dvlen_reclaim(st, space, H5P_DEFAULT, ptrs.data());
    return arrow::Status::OK();
}

// vlen byte blobs (one per row).
arrow::Status write_vlen_bytes(hid_t grp, const std::string& name,
                               const std::vector<std::pair<const std::uint8_t*, std::size_t>>& blobs)
{
    Hid vt(H5Tvlen_create(H5T_NATIVE_UINT8), H5Tclose);
    std::vector<hvl_t> data(blobs.size());
    for (std::size_t i = 0; i < blobs.size(); ++i) {
        data[i].len = blobs[i].second;
        data[i].p = const_cast<std::uint8_t*>(blobs[i].first);
    }
    hsize_t dim = blobs.size();
    Hid space(H5Screate_simple(1, &dim, nullptr), H5Sclose);
    Hid dset(H5Dcreate2(grp, name.c_str(), vt, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Dclose);
    if (!dset.ok()) return io("H5Dcreate2 " + name);
    if (dim > 0 && H5Dwrite(dset, vt, H5S_ALL, H5S_ALL, H5P_DEFAULT, data.data()) < 0)
        return io("H5Dwrite " + name);
    return arrow::Status::OK();
}

arrow::Status read_vlen_bytes(hid_t grp, const std::string& name, std::int64_t n,
                              std::vector<std::vector<std::uint8_t>>& out)
{
    out.assign(n, {});
    if (n == 0) return arrow::Status::OK();
    Hid dset(H5Dopen2(grp, name.c_str(), H5P_DEFAULT), H5Dclose);
    if (!dset.ok()) return io("H5Dopen2 " + name);
    Hid vt(H5Tvlen_create(H5T_NATIVE_UINT8), H5Tclose);
    std::vector<hvl_t> data(n);
    if (H5Dread(dset, vt, H5S_ALL, H5S_ALL, H5P_DEFAULT, data.data()) < 0) return io("H5Dread " + name);
    for (std::int64_t i = 0; i < n; ++i) {
        const auto* p = static_cast<const std::uint8_t*>(data[i].p);
        out[i].assign(p, p + data[i].len);
    }
    Hid space(H5Dget_space(dset), H5Sclose);
    H5Dvlen_reclaim(vt, space, H5P_DEFAULT, data.data());
    return arrow::Status::OK();
}

// --- validity (null mask) as a companion uint8 dataset ---------------------

arrow::Status write_validity(hid_t grp, const std::string& name, const arrow::Array& arr)
{
    if (arr.null_count() == 0) return arrow::Status::OK();
    std::vector<std::uint8_t> valid(arr.length());
    for (std::int64_t i = 0; i < arr.length(); ++i) valid[i] = arr.IsNull(i) ? 0 : 1;
    return write_dataset(grp, name + kValidSuffix, H5T_NATIVE_UINT8,
                         {static_cast<hsize_t>(arr.length())}, valid.data());
}

// Returns true and fills `valid` if a validity dataset exists for `name`.
arrow::Result<bool> read_validity(hid_t grp, const std::string& name, std::int64_t n,
                                  std::vector<std::uint8_t>& valid)
{
    const std::string vname = name + kValidSuffix;
    if (!has_link(grp, vname)) return false;
    valid.assign(n, 1);
    ARROW_RETURN_NOT_OK(read_dataset(grp, vname, H5T_NATIVE_UINT8, valid.data()));
    return true;
}

// Build a packed validity bitmap Buffer from valid bytes (or null if all set).
arrow::Result<std::shared_ptr<arrow::Buffer>> bits_from_valid(const std::vector<std::uint8_t>& valid,
                                                              std::int64_t& null_count)
{
    null_count = 0;
    for (auto v : valid) if (!v) ++null_count;
    if (null_count == 0) return std::shared_ptr<arrow::Buffer>(nullptr);
    arrow::BooleanBuilder bb;
    ARROW_RETURN_NOT_OK(bb.AppendValues(valid.data(), static_cast<int64_t>(valid.size())));
    std::shared_ptr<arrow::BooleanArray> ba;
    ARROW_RETURN_NOT_OK(bb.Finish(&ba));
    return ba->values();  // bit-packed buffer
}

arrow::Result<std::shared_ptr<arrow::Buffer>> buffer_from_i32(const std::vector<std::int32_t>& v)
{
    ARROW_ASSIGN_OR_RAISE(auto buf, arrow::AllocateBuffer(static_cast<int64_t>(v.size()) * 4));
    std::memcpy(buf->mutable_data(), v.data(), v.size() * 4);
    return std::shared_ptr<arrow::Buffer>(std::move(buf));
}
arrow::Result<std::shared_ptr<arrow::Buffer>> buffer_from_i64(const std::vector<std::int64_t>& v)
{
    ARROW_ASSIGN_OR_RAISE(auto buf, arrow::AllocateBuffer(static_cast<int64_t>(v.size()) * 8));
    std::memcpy(buf->mutable_data(), v.data(), v.size() * 8);
    return std::shared_ptr<arrow::Buffer>(std::move(buf));
}

hid_t h5_native(arrow::Type::type id)
{
    using T = arrow::Type;
    switch (id) {
        case T::INT8:   return H5T_NATIVE_INT8;
        case T::INT16:  return H5T_NATIVE_INT16;
        case T::INT32:  return H5T_NATIVE_INT32;
        case T::INT64:  return H5T_NATIVE_INT64;
        case T::UINT8:  return H5T_NATIVE_UINT8;
        case T::UINT16: return H5T_NATIVE_UINT16;
        case T::UINT32: return H5T_NATIVE_UINT32;
        case T::UINT64: return H5T_NATIVE_UINT64;
        case T::FLOAT:  return H5T_NATIVE_FLOAT;
        case T::DOUBLE: return H5T_NATIVE_DOUBLE;
        default:        return -1;
    }
}

// Write a primitive child array as a 2-D [n, k] dataset (rectangular, h5py-friendly).
template <typename ArrayT, typename CType>
arrow::Status write_2d(hid_t grp, const std::string& name, const arrow::Array& child,
                       std::int64_t n, std::int32_t k, hid_t h5type)
{
    const auto& a = static_cast<const ArrayT&>(child);
    std::vector<CType> vals(static_cast<std::size_t>(n) * k);
    for (std::int64_t i = 0; i < n * k; ++i) vals[i] = a.IsNull(i) ? CType{} : static_cast<CType>(a.Value(i));
    return write_dataset(grp, name, h5type, {static_cast<hsize_t>(n), static_cast<hsize_t>(k)}, vals.data());
}

// Read a primitive dataset of `count` elements (any shape) into an Array (no nulls).
template <typename BuilderT, typename CType>
arrow::Result<std::shared_ptr<arrow::Array>> decode_flat(hid_t grp, const std::string& name,
                                                         hid_t h5type, std::int64_t count)
{
    std::vector<CType> vals(count);
    ARROW_RETURN_NOT_OK(read_dataset(grp, name, h5type, vals.data()));
    BuilderT b;
    ARROW_RETURN_NOT_OK(b.AppendValues(vals.data(), count));
    std::shared_ptr<arrow::Array> out;
    ARROW_RETURN_NOT_OK(b.Finish(&out));
    return out;
}

// --- numeric encode/decode -------------------------------------------------

template <typename ArrayT, typename CType>
arrow::Status encode_numeric(hid_t grp, const std::string& name, const arrow::Array& arr, hid_t h5type)
{
    const auto& a = static_cast<const ArrayT&>(arr);
    std::vector<CType> vals(a.length());
    for (std::int64_t i = 0; i < a.length(); ++i) vals[i] = a.IsNull(i) ? CType{} : static_cast<CType>(a.Value(i));
    ARROW_RETURN_NOT_OK(write_dataset(grp, name, h5type, {static_cast<hsize_t>(a.length())}, vals.data()));
    return write_validity(grp, name, arr);
}

template <typename BuilderT, typename CType>
arrow::Result<std::shared_ptr<arrow::Array>> decode_numeric(hid_t grp, const std::string& name,
                                                            hid_t h5type, std::int64_t n)
{
    std::vector<CType> vals(n);
    ARROW_RETURN_NOT_OK(read_dataset(grp, name, h5type, vals.data()));
    std::vector<std::uint8_t> valid;
    ARROW_ASSIGN_OR_RAISE(bool hasnull, read_validity(grp, name, n, valid));
    BuilderT b;
    if (hasnull) ARROW_RETURN_NOT_OK(b.AppendValues(vals.data(), n, valid.data()));
    else ARROW_RETURN_NOT_OK(b.AppendValues(vals.data(), n));
    std::shared_ptr<arrow::Array> out;
    ARROW_RETURN_NOT_OK(b.Finish(&out));
    return out;
}

// --- forward decls for recursion -------------------------------------------
arrow::Status encode_array(hid_t grp, const std::string& name, const arrow::Array& arr);
arrow::Result<std::shared_ptr<arrow::Array>> decode_array(
    hid_t grp, const std::string& name, const std::shared_ptr<arrow::DataType>& type, std::int64_t n);

// --- encode ----------------------------------------------------------------

arrow::Status encode_array(hid_t grp, const std::string& name, const arrow::Array& arr)
{
    using T = arrow::Type;
    switch (arr.type_id()) {
        case T::INT8:   return encode_numeric<arrow::Int8Array, std::int8_t>(grp, name, arr, H5T_NATIVE_INT8);
        case T::INT16:  return encode_numeric<arrow::Int16Array, std::int16_t>(grp, name, arr, H5T_NATIVE_INT16);
        case T::INT32:  return encode_numeric<arrow::Int32Array, std::int32_t>(grp, name, arr, H5T_NATIVE_INT32);
        case T::INT64:  return encode_numeric<arrow::Int64Array, std::int64_t>(grp, name, arr, H5T_NATIVE_INT64);
        case T::UINT8:  return encode_numeric<arrow::UInt8Array, std::uint8_t>(grp, name, arr, H5T_NATIVE_UINT8);
        case T::UINT16: return encode_numeric<arrow::UInt16Array, std::uint16_t>(grp, name, arr, H5T_NATIVE_UINT16);
        case T::UINT32: return encode_numeric<arrow::UInt32Array, std::uint32_t>(grp, name, arr, H5T_NATIVE_UINT32);
        case T::UINT64: return encode_numeric<arrow::UInt64Array, std::uint64_t>(grp, name, arr, H5T_NATIVE_UINT64);
        case T::FLOAT:  return encode_numeric<arrow::FloatArray, float>(grp, name, arr, H5T_NATIVE_FLOAT);
        case T::DOUBLE: return encode_numeric<arrow::DoubleArray, double>(grp, name, arr, H5T_NATIVE_DOUBLE);
        case T::BOOL:   return encode_numeric<arrow::BooleanArray, std::uint8_t>(grp, name, arr, H5T_NATIVE_UINT8);

        case T::STRING:
        case T::LARGE_STRING: {
            std::vector<std::string> vals(arr.length());
            for (std::int64_t i = 0; i < arr.length(); ++i) {
                if (arr.IsNull(i)) continue;
                std::string_view sv = (arr.type_id() == T::STRING)
                    ? static_cast<const arrow::StringArray&>(arr).GetView(i)
                    : static_cast<const arrow::LargeStringArray&>(arr).GetView(i);
                vals[i].assign(sv.data(), sv.size());
            }
            ARROW_RETURN_NOT_OK(write_vlen_strings(grp, name, vals));
            return write_validity(grp, name, arr);
        }

        case T::BINARY:
        case T::LARGE_BINARY: {
            std::vector<std::pair<const std::uint8_t*, std::size_t>> blobs(arr.length());
            // Keep views alive via the array itself.
            for (std::int64_t i = 0; i < arr.length(); ++i) {
                if (arr.IsNull(i)) { blobs[i] = {nullptr, 0}; continue; }
                if (arr.type_id() == T::BINARY) {
                    auto sv = static_cast<const arrow::BinaryArray&>(arr).GetView(i);
                    blobs[i] = {reinterpret_cast<const std::uint8_t*>(sv.data()), sv.size()};
                } else {
                    auto sv = static_cast<const arrow::LargeBinaryArray&>(arr).GetView(i);
                    blobs[i] = {reinterpret_cast<const std::uint8_t*>(sv.data()), sv.size()};
                }
            }
            ARROW_RETURN_NOT_OK(write_vlen_bytes(grp, name, blobs));
            return write_validity(grp, name, arr);
        }

        case T::FIXED_SIZE_LIST: {
            const auto& a = static_cast<const arrow::FixedSizeListArray&>(arr);
            const int32_t k = a.list_type()->list_size();
            const std::int64_t n = a.length();
            auto child = a.values()->Slice(a.value_offset(0), n * k);
            const hid_t h5t = h5_native(child->type_id());
            if (h5t < 0)
                return io("fixed_size_list with non-primitive child not supported: " + name);
            // True 2-D [n, k] rectangular dataset (h5py-loadable as a NumPy array).
            switch (child->type_id()) {
                case T::INT8:   ARROW_RETURN_NOT_OK((write_2d<arrow::Int8Array, std::int8_t>(grp, name, *child, n, k, h5t))); break;
                case T::INT16:  ARROW_RETURN_NOT_OK((write_2d<arrow::Int16Array, std::int16_t>(grp, name, *child, n, k, h5t))); break;
                case T::INT32:  ARROW_RETURN_NOT_OK((write_2d<arrow::Int32Array, std::int32_t>(grp, name, *child, n, k, h5t))); break;
                case T::INT64:  ARROW_RETURN_NOT_OK((write_2d<arrow::Int64Array, std::int64_t>(grp, name, *child, n, k, h5t))); break;
                case T::UINT8:  ARROW_RETURN_NOT_OK((write_2d<arrow::UInt8Array, std::uint8_t>(grp, name, *child, n, k, h5t))); break;
                case T::UINT16: ARROW_RETURN_NOT_OK((write_2d<arrow::UInt16Array, std::uint16_t>(grp, name, *child, n, k, h5t))); break;
                case T::UINT32: ARROW_RETURN_NOT_OK((write_2d<arrow::UInt32Array, std::uint32_t>(grp, name, *child, n, k, h5t))); break;
                case T::UINT64: ARROW_RETURN_NOT_OK((write_2d<arrow::UInt64Array, std::uint64_t>(grp, name, *child, n, k, h5t))); break;
                case T::FLOAT:  ARROW_RETURN_NOT_OK((write_2d<arrow::FloatArray, float>(grp, name, *child, n, k, h5t))); break;
                case T::DOUBLE: ARROW_RETURN_NOT_OK((write_2d<arrow::DoubleArray, double>(grp, name, *child, n, k, h5t))); break;
                default: return io("fixed_size_list child type unsupported: " + name);
            }
            return write_validity(grp, name, arr);
        }

        case T::LIST:
        case T::LARGE_LIST: {
            const std::int64_t n = arr.length();
            std::shared_ptr<arrow::Array> child;
            std::vector<std::int64_t> offs(n + 1);
            if (arr.type_id() == T::LIST) {
                const auto& a = static_cast<const arrow::ListArray&>(arr);
                const std::int64_t o0 = a.value_offset(0);
                for (std::int64_t i = 0; i <= n; ++i) offs[i] = a.value_offset(i) - o0;
                child = a.values()->Slice(o0, offs[n]);
            } else {
                const auto& a = static_cast<const arrow::LargeListArray&>(arr);
                const std::int64_t o0 = a.value_offset(0);
                for (std::int64_t i = 0; i <= n; ++i) offs[i] = a.value_offset(i) - o0;
                child = a.values()->Slice(o0, offs[n]);
            }
            ARROW_RETURN_NOT_OK(write_dataset(grp, name + kOffsetsSuffix, H5T_NATIVE_INT64,
                                              {static_cast<hsize_t>(n + 1)}, offs.data()));
            ARROW_RETURN_NOT_OK(encode_array(grp, name, *child));
            return write_validity(grp, name, arr);
        }

        case T::STRUCT: {
            const auto& a = static_cast<const arrow::StructArray&>(arr);
            Hid sub(H5Gcreate2(grp, name.c_str(), H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT), H5Gclose);
            if (!sub.ok()) return io("H5Gcreate2 struct " + name);
            for (int f = 0; f < a.num_fields(); ++f) {
                ARROW_RETURN_NOT_OK(encode_array(sub, path_escape(a.struct_type()->field(f)->name()),
                                                 *a.field(f)));
            }
            return write_validity(grp, name, arr);
        }

        default:
            return io("unsupported Arrow type for HDF5: " + arr.type()->ToString());
    }
}

// --- decode ----------------------------------------------------------------

arrow::Result<std::shared_ptr<arrow::Array>> decode_array(
    hid_t grp, const std::string& name, const std::shared_ptr<arrow::DataType>& type, std::int64_t n)
{
    using T = arrow::Type;
    switch (type->id()) {
        case T::INT8:   return decode_numeric<arrow::Int8Builder, std::int8_t>(grp, name, H5T_NATIVE_INT8, n);
        case T::INT16:  return decode_numeric<arrow::Int16Builder, std::int16_t>(grp, name, H5T_NATIVE_INT16, n);
        case T::INT32:  return decode_numeric<arrow::Int32Builder, std::int32_t>(grp, name, H5T_NATIVE_INT32, n);
        case T::INT64:  return decode_numeric<arrow::Int64Builder, std::int64_t>(grp, name, H5T_NATIVE_INT64, n);
        case T::UINT8:  return decode_numeric<arrow::UInt8Builder, std::uint8_t>(grp, name, H5T_NATIVE_UINT8, n);
        case T::UINT16: return decode_numeric<arrow::UInt16Builder, std::uint16_t>(grp, name, H5T_NATIVE_UINT16, n);
        case T::UINT32: return decode_numeric<arrow::UInt32Builder, std::uint32_t>(grp, name, H5T_NATIVE_UINT32, n);
        case T::UINT64: return decode_numeric<arrow::UInt64Builder, std::uint64_t>(grp, name, H5T_NATIVE_UINT64, n);
        case T::FLOAT:  return decode_numeric<arrow::FloatBuilder, float>(grp, name, H5T_NATIVE_FLOAT, n);
        case T::DOUBLE: return decode_numeric<arrow::DoubleBuilder, double>(grp, name, H5T_NATIVE_DOUBLE, n);
        case T::BOOL: {
            std::vector<std::uint8_t> vals(n);
            ARROW_RETURN_NOT_OK(read_dataset(grp, name, H5T_NATIVE_UINT8, vals.data()));
            std::vector<std::uint8_t> valid;
            ARROW_ASSIGN_OR_RAISE(bool hasnull, read_validity(grp, name, n, valid));
            arrow::BooleanBuilder b;
            if (hasnull) ARROW_RETURN_NOT_OK(b.AppendValues(vals.data(), n, valid.data()));
            else ARROW_RETURN_NOT_OK(b.AppendValues(vals.data(), n));
            std::shared_ptr<arrow::Array> out; ARROW_RETURN_NOT_OK(b.Finish(&out)); return out;
        }

        case T::STRING:
        case T::LARGE_STRING: {
            std::vector<std::string> vals;
            ARROW_RETURN_NOT_OK(read_vlen_strings(grp, name, n, vals));
            std::vector<std::uint8_t> valid;
            ARROW_ASSIGN_OR_RAISE(bool hasnull, read_validity(grp, name, n, valid));
            std::shared_ptr<arrow::Array> out;
            if (type->id() == T::STRING) {
                arrow::StringBuilder b;
                for (std::int64_t i = 0; i < n; ++i) {
                    if (hasnull && !valid[i]) ARROW_RETURN_NOT_OK(b.AppendNull());
                    else ARROW_RETURN_NOT_OK(b.Append(vals[i]));
                }
                ARROW_RETURN_NOT_OK(b.Finish(&out));
            } else {
                arrow::LargeStringBuilder b;
                for (std::int64_t i = 0; i < n; ++i) {
                    if (hasnull && !valid[i]) ARROW_RETURN_NOT_OK(b.AppendNull());
                    else ARROW_RETURN_NOT_OK(b.Append(vals[i]));
                }
                ARROW_RETURN_NOT_OK(b.Finish(&out));
            }
            return out;
        }

        case T::BINARY:
        case T::LARGE_BINARY: {
            std::vector<std::vector<std::uint8_t>> blobs;
            ARROW_RETURN_NOT_OK(read_vlen_bytes(grp, name, n, blobs));
            std::vector<std::uint8_t> valid;
            ARROW_ASSIGN_OR_RAISE(bool hasnull, read_validity(grp, name, n, valid));
            std::shared_ptr<arrow::Array> out;
            if (type->id() == T::BINARY) {
                arrow::BinaryBuilder b;
                for (std::int64_t i = 0; i < n; ++i) {
                    if (hasnull && !valid[i]) ARROW_RETURN_NOT_OK(b.AppendNull());
                    else ARROW_RETURN_NOT_OK(b.Append(blobs[i].data(), static_cast<int32_t>(blobs[i].size())));
                }
                ARROW_RETURN_NOT_OK(b.Finish(&out));
            } else {
                arrow::LargeBinaryBuilder b;
                for (std::int64_t i = 0; i < n; ++i) {
                    if (hasnull && !valid[i]) ARROW_RETURN_NOT_OK(b.AppendNull());
                    else ARROW_RETURN_NOT_OK(b.Append(blobs[i].data(), static_cast<int64_t>(blobs[i].size())));
                }
                ARROW_RETURN_NOT_OK(b.Finish(&out));
            }
            return out;
        }

        case T::FIXED_SIZE_LIST: {
            auto flt = std::static_pointer_cast<arrow::FixedSizeListType>(type);
            const int32_t k = flt->list_size();
            const hid_t h5t = h5_native(flt->value_type()->id());
            if (h5t < 0) return io("fixed_size_list decode: non-primitive child " + name);
            // Read the rectangular [n, k] dataset (n*k flat) into the child array.
            std::shared_ptr<arrow::Array> child;
            const std::int64_t count = n * k;
            switch (flt->value_type()->id()) {
                case T::INT8:   { ARROW_ASSIGN_OR_RAISE(child, (decode_flat<arrow::Int8Builder, std::int8_t>(grp, name, h5t, count))); break; }
                case T::INT16:  { ARROW_ASSIGN_OR_RAISE(child, (decode_flat<arrow::Int16Builder, std::int16_t>(grp, name, h5t, count))); break; }
                case T::INT32:  { ARROW_ASSIGN_OR_RAISE(child, (decode_flat<arrow::Int32Builder, std::int32_t>(grp, name, h5t, count))); break; }
                case T::INT64:  { ARROW_ASSIGN_OR_RAISE(child, (decode_flat<arrow::Int64Builder, std::int64_t>(grp, name, h5t, count))); break; }
                case T::UINT8:  { ARROW_ASSIGN_OR_RAISE(child, (decode_flat<arrow::UInt8Builder, std::uint8_t>(grp, name, h5t, count))); break; }
                case T::UINT16: { ARROW_ASSIGN_OR_RAISE(child, (decode_flat<arrow::UInt16Builder, std::uint16_t>(grp, name, h5t, count))); break; }
                case T::UINT32: { ARROW_ASSIGN_OR_RAISE(child, (decode_flat<arrow::UInt32Builder, std::uint32_t>(grp, name, h5t, count))); break; }
                case T::UINT64: { ARROW_ASSIGN_OR_RAISE(child, (decode_flat<arrow::UInt64Builder, std::uint64_t>(grp, name, h5t, count))); break; }
                case T::FLOAT:  { ARROW_ASSIGN_OR_RAISE(child, (decode_flat<arrow::FloatBuilder, float>(grp, name, h5t, count))); break; }
                case T::DOUBLE: { ARROW_ASSIGN_OR_RAISE(child, (decode_flat<arrow::DoubleBuilder, double>(grp, name, h5t, count))); break; }
                default: return io("fixed_size_list decode child unsupported " + name);
            }
            std::vector<std::uint8_t> valid;
            ARROW_ASSIGN_OR_RAISE(bool hasnull, read_validity(grp, name, n, valid));
            std::int64_t null_count = 0;
            std::shared_ptr<arrow::Buffer> validbuf;
            if (hasnull) { ARROW_ASSIGN_OR_RAISE(validbuf, bits_from_valid(valid, null_count)); }
            auto data = arrow::ArrayData::Make(type, n, {validbuf}, {child->data()}, null_count, 0);
            return arrow::MakeArray(data);
        }

        case T::LIST:
        case T::LARGE_LIST: {
            std::vector<std::int64_t> offs(n + 1);
            ARROW_RETURN_NOT_OK(read_dataset(grp, name + kOffsetsSuffix, H5T_NATIVE_INT64, offs.data()));
            const std::int64_t total = (n >= 0) ? offs[n] : 0;
            std::shared_ptr<arrow::DataType> child_type =
                std::static_pointer_cast<arrow::BaseListType>(type)->value_type();
            auto child = decode_array(grp, name, child_type, total).ValueOrDie();
            std::vector<std::uint8_t> valid;
            ARROW_ASSIGN_OR_RAISE(bool hasnull, read_validity(grp, name, n, valid));
            std::int64_t null_count = 0;
            std::shared_ptr<arrow::Buffer> validbuf;
            if (hasnull) { ARROW_ASSIGN_OR_RAISE(validbuf, bits_from_valid(valid, null_count)); }
            std::shared_ptr<arrow::Buffer> offbuf;
            if (type->id() == T::LIST) {
                std::vector<std::int32_t> o32(offs.begin(), offs.end());
                ARROW_ASSIGN_OR_RAISE(offbuf, buffer_from_i32(o32));
            } else {
                ARROW_ASSIGN_OR_RAISE(offbuf, buffer_from_i64(offs));
            }
            auto data = arrow::ArrayData::Make(type, n, {validbuf, offbuf}, {child->data()}, null_count, 0);
            return arrow::MakeArray(data);
        }

        case T::STRUCT: {
            auto stype = std::static_pointer_cast<arrow::StructType>(type);
            Hid sub(H5Gopen2(grp, name.c_str(), H5P_DEFAULT), H5Gclose);
            if (!sub.ok()) return io("H5Gopen2 struct " + name);
            std::vector<std::shared_ptr<arrow::ArrayData>> children;
            for (int f = 0; f < stype->num_fields(); ++f) {
                auto child = decode_array(sub, path_escape(stype->field(f)->name()), stype->field(f)->type(), n).ValueOrDie();
                children.push_back(child->data());
            }
            std::vector<std::uint8_t> valid;
            ARROW_ASSIGN_OR_RAISE(bool hasnull, read_validity(grp, name, n, valid));
            std::int64_t null_count = 0;
            std::shared_ptr<arrow::Buffer> validbuf;
            if (hasnull) { ARROW_ASSIGN_OR_RAISE(validbuf, bits_from_valid(valid, null_count)); }
            auto data = arrow::ArrayData::Make(type, n, {validbuf}, null_count, 0);
            data->child_data = children;
            return arrow::MakeArray(data);
        }

        default:
            return io("unsupported Arrow type for HDF5 decode: " + type->ToString());
    }
}

}  // namespace

// --- Hdf5File --------------------------------------------------------------

arrow::Result<Hdf5File> Hdf5File::create(const std::string& path)
{
    hid_t f = H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (f < 0) return io("H5Fcreate " + path);
    return Hdf5File(f);
}
arrow::Result<Hdf5File> Hdf5File::open_readonly(const std::string& path)
{
    hid_t f = H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    if (f < 0) return io("H5Fopen(ro) " + path);
    return Hdf5File(f);
}
arrow::Result<Hdf5File> Hdf5File::open_readwrite(const std::string& path)
{
    hid_t f = H5Fopen(path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT);
    if (f < 0) {
        f = H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
        if (f < 0) return io("H5Fopen/create(rw) " + path);
    }
    return Hdf5File(f);
}

Hdf5File::Hdf5File(Hdf5File&& o) noexcept : m_file(o.m_file) { o.m_file = H5I_INVALID_HID; }
Hdf5File& Hdf5File::operator=(Hdf5File&& o) noexcept
{
    if (this != &o) {
        if (m_file >= 0) H5Fclose(m_file);
        m_file = o.m_file;
        o.m_file = H5I_INVALID_HID;
    }
    return *this;
}
Hdf5File::~Hdf5File() { if (m_file >= 0) H5Fclose(m_file); }

arrow::Status Hdf5File::write(const Address& addr, const std::shared_ptr<arrow::Table>& table)
{
    if (m_file < 0) return io("write: file not open");
    // Combine to a single chunk per column so each column is one Array.
    ARROW_ASSIGN_OR_RAISE(auto combined, table->CombineChunks());

    // Create the product group (with intermediate groups).
    Hid lcpl(H5Pcreate(H5P_LINK_CREATE), H5Pclose);
    H5Pset_create_intermediate_group(lcpl, 1);
    Hid grp(H5Gcreate2(m_file, addr.path().c_str(), lcpl, H5P_DEFAULT, H5P_DEFAULT), H5Gclose);
    if (!grp.ok()) return io("create product group " + addr.path());

    // Faithful schema (IPC) for exact reconstruction.
    ARROW_ASSIGN_OR_RAISE(auto schemabuf, arrow::ipc::SerializeSchema(*combined->schema()));
    ARROW_RETURN_NOT_OK(write_dataset(grp, kSchemaDataset, H5T_NATIVE_UINT8,
                                      {static_cast<hsize_t>(schemabuf->size())}, schemabuf->data()));
    ARROW_RETURN_NOT_OK(write_int64_attr(grp, kNumRowsAttr, combined->num_rows()));

    // Friendly: schema-level metadata -> group attributes (in-place readability).
    if (auto md = combined->schema()->metadata()) {
        for (int i = 0; i < md->size(); ++i) {
            ARROW_RETURN_NOT_OK(write_string_attr(grp, md->key(i), md->value(i)));
        }
    }

    // Each column -> dataset(s).  After CombineChunks each column has one chunk
    // (or zero, for a 0-row table).
    for (int c = 0; c < combined->num_columns(); ++c) {
        auto chunked = combined->column(c);
        std::shared_ptr<arrow::Array> arr;
        if (chunked->num_chunks() == 1) {
            arr = chunked->chunk(0);
        } else {
            ARROW_ASSIGN_OR_RAISE(arr, arrow::MakeArrayOfNull(combined->field(c)->type(), 0));
        }
        ARROW_RETURN_NOT_OK(encode_array(grp, path_escape(combined->field(c)->name()), *arr));
    }
    return arrow::Status::OK();
}

arrow::Result<std::shared_ptr<arrow::Table>> Hdf5File::read(const Address& addr)
{
    if (m_file < 0) return io("read: file not open");
    Hid grp(H5Gopen2(m_file, addr.path().c_str(), H5P_DEFAULT), H5Gclose);
    if (!grp.ok()) return io("open product group " + addr.path());

    // Read the schema.
    ARROW_ASSIGN_OR_RAISE(hsize_t slen, dataset_dim0(grp, kSchemaDataset));
    std::vector<std::uint8_t> sbytes(slen);
    ARROW_RETURN_NOT_OK(read_dataset(grp, kSchemaDataset, H5T_NATIVE_UINT8, sbytes.data()));
    auto sbuf = std::make_shared<arrow::Buffer>(sbytes.data(), static_cast<int64_t>(slen));
    arrow::io::BufferReader reader(sbuf);
    arrow::ipc::DictionaryMemo memo;
    ARROW_ASSIGN_OR_RAISE(auto schema, arrow::ipc::ReadSchema(&reader, &memo));

    ARROW_ASSIGN_OR_RAISE(std::int64_t nrows, read_int64_attr(grp, kNumRowsAttr));

    std::vector<std::shared_ptr<arrow::Array>> columns;
    for (int c = 0; c < schema->num_fields(); ++c) {
        ARROW_ASSIGN_OR_RAISE(auto arr, decode_array(grp, path_escape(schema->field(c)->name()),
                                                     schema->field(c)->type(), nrows));
        columns.push_back(arr);
    }
    return arrow::Table::Make(schema, columns, nrows);
}

namespace {
struct ScanState { std::vector<std::string> product_paths; };

herr_t scan_cb(hid_t /*obj*/, const char* name, const H5O_info2_t* info, void* op_data)
{
    if (info->type != H5O_TYPE_DATASET) return 0;
    std::string n(name);
    const std::string suffix = std::string("/") + kSchemaDataset;
    if (n.size() > suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0) {
        auto* st = static_cast<ScanState*>(op_data);
        st->product_paths.push_back("/" + n.substr(0, n.size() - suffix.size()));
    }
    return 0;
}
}  // namespace

arrow::Result<Hierarchy> Hdf5File::scan()
{
    if (m_file < 0) return io("scan: file not open");
    ScanState st;
    if (H5Ovisit(m_file, H5_INDEX_NAME, H5_ITER_NATIVE, scan_cb, &st, H5O_INFO_BASIC) < 0)
        return io("H5Ovisit");
    std::vector<Address> addrs;
    addrs.reserve(st.product_paths.size());
    for (const auto& p : st.product_paths) addrs.push_back(Address(p));
    return Hierarchy::from_addresses(addrs);
}

}  // namespace arrow_hdf
