#ifndef ARROW_HDF_HDF5FILE_H
#define ARROW_HDF_HDF5FILE_H

// Generic, schema-driven Arrow <-> HDF5 serialization (the HDF5 M-axis).
// Depends only on Apache Arrow and the HDF5 C library (plus arrow_hdf_core).
// No Phlex, no WCT.
//
// Layout convention: see docs/layout.md.

#include "arrow_hdf/Address.h"
#include "arrow_hdf/Hierarchy.h"

#include <arrow/api.h>
#include <hdf5.h>

#include <memory>
#include <string>

namespace arrow_hdf {

/// An open HDF5 file that stores Arrow tables at structured addresses.
///
/// Thread-safety: this class does NO internal locking.  libhdf5 is not
/// thread-safe (unless built so); callers must serialize access to a given
/// file (in Phlex, the output node's serial concurrency provides this).
class Hdf5File {
  public:
    /// Create (truncate) a file for writing.
    static arrow::Result<Hdf5File> create(const std::string& path);
    /// Open an existing file read-only.
    static arrow::Result<Hdf5File> open_readonly(const std::string& path);
    /// Open an existing file for read/write (created if absent).
    static arrow::Result<Hdf5File> open_readwrite(const std::string& path);

    Hdf5File(Hdf5File&&) noexcept;
    Hdf5File& operator=(Hdf5File&&) noexcept;
    Hdf5File(const Hdf5File&) = delete;
    Hdf5File& operator=(const Hdf5File&) = delete;
    ~Hdf5File();

    /// Write a table at the address's product group path.
    arrow::Status write(const Address& addr, const std::shared_ptr<arrow::Table>& table);

    /// Read the table previously written at the address.
    arrow::Result<std::shared_ptr<arrow::Table>> read(const Address& addr);

    /// Enumerate all stored products into a hierarchy descriptor.
    arrow::Result<Hierarchy> scan();

  private:
    explicit Hdf5File(hid_t file) : m_file(file) {}
    hid_t m_file{H5I_INVALID_HID};
};

}  // namespace arrow_hdf

#endif  // ARROW_HDF_HDF5FILE_H
