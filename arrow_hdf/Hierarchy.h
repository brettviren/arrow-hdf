#ifndef ARROW_HDF_HIERARCHY_H
#define ARROW_HDF_HIERARCHY_H

// A Phlex-neutral descriptor of the addresses stored in a file: the ordered
// layer names plus the cell tree of (layer, number), with the products located
// at each cell.  The HDF5 scan (Reader side) produces a flat list of stored
// Addresses; building the tree descriptor from that list is pure logic and
// lives here so it can be tested without HDF5.  This descriptor is consumed by
// the Phlex read side; it stays Phlex-neutral.

#include "arrow_hdf/Address.h"

#include <cstdint>
#include <string>
#include <vector>

namespace arrow_hdf {

/// A product stored at a particular cell.
struct ProductLoc {
    std::string creator;
    std::string product;
    friend bool operator==(const ProductLoc&, const ProductLoc&) = default;
};

/// A cell in the hierarchy: its layer/number, the products stored directly at
/// it, and its child cells (the next layer).  Children are kept sorted by
/// number (numeric, not lexicographic).
struct HierCell {
    std::string layer;
    std::int64_t number{0};
    std::vector<ProductLoc> products;
    std::vector<HierCell> children;
    friend bool operator==(const HierCell&, const HierCell&) = default;
};

/// The whole-file descriptor.
struct Hierarchy {
    std::vector<std::string> layer_names;     ///< layer name at each depth (root->leaf)
    std::vector<ProductLoc> root_products;    ///< products at the job root (empty cell path)
    std::vector<HierCell> cells;              ///< top-level cells, sorted by number

    /// Build a descriptor from a flat list of stored addresses.  Cells are
    /// matched by (layer, number); children are sorted numerically.  Throws
    /// std::invalid_argument if two addresses disagree on the layer name at a
    /// given depth.
    static Hierarchy from_addresses(const std::vector<Address>& addresses);

    friend bool operator==(const Hierarchy&, const Hierarchy&) = default;
};

}  // namespace arrow_hdf

#endif  // ARROW_HDF_HIERARCHY_H
