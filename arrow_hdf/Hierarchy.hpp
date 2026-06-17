#ifndef ARROW_HDF_HIERARCHY_H
#define ARROW_HDF_HIERARCHY_H

// A generic descriptor of the addresses stored in a file: a trie of path
// components.  Each node carries its link-name component and a flag marking
// whether an object (table) is stored at that node's path.  This supports
// Hdf5File::scan() and is intentionally free of any domain/framework cell
// semantics (no layer/number/creator/product) — a consumer that wants such
// structure interprets the components itself.

#include "arrow_hdf/Address.hpp"

#include <string>
#include <vector>

namespace arrow_hdf {

/// A node in the path trie.
struct HierNode {
    std::string name;                  ///< this node's path component (root: "")
    bool product{false};               ///< true if a stored object lives at this path
    std::vector<HierNode> children;    ///< child nodes, sorted by name (lexicographic)
    friend bool operator==(const HierNode&, const HierNode&) = default;
};

/// The whole-file descriptor (rooted trie).
struct Hierarchy {
    HierNode root;   ///< the root group, name == ""

    /// Build the trie from a list of stored object addresses.  Each address'
    /// path is split into components; the leaf node is marked as a product.
    /// Children are sorted lexicographically by name.
    static Hierarchy from_addresses(const std::vector<Address>& addresses);

    /// All product paths in the trie, in lexicographic (sorted-DFS) order.
    std::vector<std::string> product_paths() const;

    friend bool operator==(const Hierarchy&, const Hierarchy&) = default;
};

}  // namespace arrow_hdf

#endif  // ARROW_HDF_HIERARCHY_H
