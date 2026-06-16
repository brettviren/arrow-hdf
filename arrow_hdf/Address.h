#ifndef ARROW_HDF_ADDRESS_H
#define ARROW_HDF_ADDRESS_H

// A Phlex-neutral structured address for a stored product.
//
// An Address is a sequence of (layer_name, number) cells plus a creator name
// and a product name.  It maps to a file path such as
//   /run/3/event/12/<creator>/<product>
// The cell sequence may be empty (a product stored at the job root).
//
// This header has NO dependency on Arrow, HDF5, Phlex, or WCT.

#include <cstdint>
#include <string>
#include <vector>

namespace arrow_hdf {

/// One level of the structured address: a named layer and its cell number.
struct Cell {
    std::string layer;     ///< layer name, e.g. "run", "event"
    std::int64_t number;   ///< cell number within the layer (non-negative)

    friend bool operator==(const Cell&, const Cell&) = default;
};

/// Percent-style escaping of a single path component.  Characters outside the
/// portable set [A-Za-z0-9._-] (including '/', NUL, and '%' itself) are encoded
/// as %XX (uppercase hex).  Reversible via path_unescape().
std::string path_escape(const std::string& component);

/// Inverse of path_escape().  Throws std::invalid_argument on a malformed
/// escape sequence.
std::string path_unescape(const std::string& component);

class Address {
  public:
    Address() = default;
    Address(std::vector<Cell> cells, std::string creator, std::string product);

    /// Append a (layer, number) cell; returns *this for chaining.
    Address& push(std::string layer, std::int64_t number);

    const std::vector<Cell>& cells() const { return m_cells; }
    const std::string& creator() const { return m_creator; }
    const std::string& product() const { return m_product; }

    void set_creator(std::string c) { m_creator = std::move(c); }
    void set_product(std::string p) { m_product = std::move(p); }

    /// The full slash path with every component escaped:
    ///   /esc(layer0)/num0/.../esc(creator)/esc(product)
    /// Cell numbers are rendered as plain decimal (always path-safe).
    std::string path() const;

    /// The cell path only (no creator/product): /esc(layer0)/num0/...
    /// Empty cell sequence yields "/" (the job root).
    std::string group_path() const;

    /// Parse a full path() string back into an Address.  The trailing two
    /// components are creator and product; the preceding components must form
    /// (layer, number) pairs.  Throws std::invalid_argument if malformed.
    static Address from_path(const std::string& path);

    friend bool operator==(const Address&, const Address&) = default;

  private:
    std::vector<Cell> m_cells;
    std::string m_creator;
    std::string m_product;
};

}  // namespace arrow_hdf

#endif  // ARROW_HDF_ADDRESS_H
