#ifndef ARROW_HDF_ADDRESS_H
#define ARROW_HDF_ADDRESS_H

// A generic location of a stored object: a '/'-delimited path.
//
// Address is intentionally free of any domain/framework structure (no
// run/event "cells", no creator/product split).  A client builds it either
// from a list of path components (each component is escaped to a safe link
// name) or from a pre-formed path string (used verbatim).  Mapping richer
// structured concepts onto these components is the caller's responsibility.
//
// This header has NO dependency on Arrow, HDF5, Phlex, or WCT.

#include <string>
#include <vector>

namespace arrow_hdf {

/// Percent-style escaping of a single path component.  Characters outside the
/// portable set [A-Za-z0-9._-] (including '/', NUL, and '%' itself) are encoded
/// as %XX (uppercase hex).  Reversible via path_unescape().
std::string path_escape(const std::string& component);

/// Inverse of path_escape().  Throws std::invalid_argument on a malformed
/// escape sequence.
std::string path_unescape(const std::string& component);

class Address {
  public:
    /// Build from path components.  Each component is path_escape()d and the
    /// results are joined with '/', yielding an absolute path
    /// (e.g. {"run","3","x"} -> "/run/3/x").  An empty list yields "/".
    explicit Address(const std::vector<std::string>& components);

    /// Build from a pre-formed '/'-delimited path, used verbatim (the caller is
    /// responsible for having escaped components).  A leading '/' is ensured;
    /// an empty string becomes "/".
    explicit Address(std::string path);

    /// The full '/'-delimited path.
    const std::string& path() const { return m_path; }

    friend bool operator==(const Address&, const Address&) = default;

  private:
    std::string m_path;
};

}  // namespace arrow_hdf

#endif  // ARROW_HDF_ADDRESS_H
