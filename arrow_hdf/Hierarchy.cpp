#include "arrow_hdf/Hierarchy.h"

#include <algorithm>
#include <stdexcept>

namespace arrow_hdf {

namespace {

// Find the child cell with the given number, or create it (preserving the
// layer name).  Returns a reference into `children`.
HierCell& find_or_make(std::vector<HierCell>& children, const Cell& cell)
{
    for (auto& c : children) {
        if (c.number == cell.number) {
            if (c.layer != cell.layer) {
                throw std::invalid_argument(
                    "Hierarchy: conflicting layer names ('" + c.layer + "' vs '" + cell.layer
                    + "') at cell number " + std::to_string(cell.number));
            }
            return c;
        }
    }
    children.push_back(HierCell{cell.layer, cell.number, {}, {}});
    return children.back();
}

void sort_cells(std::vector<HierCell>& cells)
{
    std::sort(cells.begin(), cells.end(),
              [](const HierCell& a, const HierCell& b) { return a.number < b.number; });
    for (auto& c : cells) sort_cells(c.children);
}

}  // namespace

Hierarchy Hierarchy::from_addresses(const std::vector<Address>& addresses)
{
    Hierarchy h;
    for (const auto& addr : addresses) {
        const auto& cells = addr.cells();

        // Record/verify the layer name at each depth.
        for (std::size_t d = 0; d < cells.size(); ++d) {
            if (d == h.layer_names.size()) {
                h.layer_names.push_back(cells[d].layer);
            } else if (h.layer_names[d] != cells[d].layer) {
                throw std::invalid_argument(
                    "Hierarchy: depth " + std::to_string(d) + " has layer '" + cells[d].layer
                    + "' but expected '" + h.layer_names[d] + "'");
            }
        }

        ProductLoc loc{addr.creator(), addr.product()};
        if (cells.empty()) {
            h.root_products.push_back(loc);
            continue;
        }
        HierCell* node = &find_or_make(h.cells, cells[0]);
        for (std::size_t d = 1; d < cells.size(); ++d) {
            node = &find_or_make(node->children, cells[d]);
        }
        node->products.push_back(loc);
    }
    sort_cells(h.cells);
    return h;
}

}  // namespace arrow_hdf
