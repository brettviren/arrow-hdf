// Unit tests for arrow_hdf::Hierarchy::from_addresses (ddm-c3s.1, item 3).

#include "arrow_hdf/Hierarchy.h"
#include "arrow_hdf/Address.h"

#include <iostream>
#include <stdexcept>
#include <vector>

using namespace arrow_hdf;

static int fails = 0;
static void check(bool ok, const std::string& what)
{
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++fails; }
}

int main()
{
    // Addresses deliberately out of numeric order to exercise sorting.
    std::vector<Address> addrs{
        Address(std::vector<Cell>{{"run", 1}, {"event", 12}}, "sigproc", "frame"),
        Address(std::vector<Cell>{{"run", 1}, {"event", 2}},  "sigproc", "frame"),
        Address(std::vector<Cell>{{"run", 1}, {"event", 2}},  "sim",     "depos"),
        Address(std::vector<Cell>{{"run", 10}, {"event", 1}}, "sigproc", "frame"),
        Address({}, "job", "meta"),   // a job-root product
    };

    Hierarchy h = Hierarchy::from_addresses(addrs);

    check(h.layer_names == std::vector<std::string>{"run", "event"}, "layer names");
    check(h.root_products.size() == 1 && h.root_products[0] == ProductLoc{"job", "meta"},
          "root product");

    // Top-level cells sorted numerically: run 1 then run 10.
    check(h.cells.size() == 2, "two run cells");
    check(h.cells[0].number == 1 && h.cells[1].number == 10, "runs sorted numerically");
    check(h.cells[0].layer == "run", "run layer");

    // run 1 children: event 2 then event 12 (numeric, not lexicographic).
    const auto& run1 = h.cells[0];
    check(run1.children.size() == 2, "run1 has 2 events");
    check(run1.children[0].number == 2 && run1.children[1].number == 12,
          "events sorted numerically (2 before 12)");

    // event 2 holds two products.
    const auto& ev2 = run1.children[0];
    check(ev2.products.size() == 2, "event2 has 2 products");

    // event 12 holds one.
    check(run1.children[1].products.size() == 1, "event12 has 1 product");

    // Conflicting layer name at a depth must throw.
    try {
        Hierarchy::from_addresses({
            Address(std::vector<Cell>{{"run", 1}}, "c", "p"),
            Address(std::vector<Cell>{{"spill", 1}}, "c", "p"),
        });
        check(false, "conflicting layer name should throw");
    } catch (const std::invalid_argument&) {}

    // Empty input -> empty descriptor.
    Hierarchy empty = Hierarchy::from_addresses({});
    check(empty.cells.empty() && empty.root_products.empty() && empty.layer_names.empty(),
          "empty input");

    if (fails) { std::cerr << fails << " failures\n"; return 1; }
    std::cout << "hierarchy OK\n";
    return 0;
}
