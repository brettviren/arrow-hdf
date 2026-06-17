// Unit tests for arrow_hdf::Hierarchy (generic path trie).

#include "arrow_hdf/Hierarchy.h"
#include "arrow_hdf/Address.h"

#include <iostream>
#include <string>
#include <vector>

using namespace arrow_hdf;

static int fails = 0;
static void check(bool ok, const std::string& what)
{
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++fails; }
}

// Find a child node by name, or nullptr.
static const HierNode* child(const HierNode& n, const std::string& name)
{
    for (const auto& c : n.children) if (c.name == name) return &c;
    return nullptr;
}

int main()
{
    std::vector<Address> addrs{
        Address(std::vector<std::string>{"run", "1", "event", "12", "sigproc", "frame"}),
        Address(std::vector<std::string>{"run", "1", "event", "2", "sigproc", "frame"}),
        Address(std::vector<std::string>{"run", "1", "event", "2", "sim", "depos"}),
        Address(std::vector<std::string>{"run", "10", "event", "1", "sigproc", "frame"}),
        Address(std::vector<std::string>{"job", "meta"}),
    };

    Hierarchy h = Hierarchy::from_addresses(addrs);

    // Root children sorted lexicographically: "job" before "run".
    check(h.root.children.size() == 2, "two top-level nodes");
    check(h.root.children[0].name == "job" && h.root.children[1].name == "run",
          "top-level sorted lexicographically");
    check(!h.root.product, "root not a product");

    // job/meta is a product leaf.
    const HierNode* job = child(h.root, "job");
    check(job && !job->product, "job is a group");
    const HierNode* meta = job ? child(*job, "meta") : nullptr;
    check(meta && meta->product, "job/meta is a product");

    // Under run: children "1" then "10" (lexicographic; numeric ordering is NOT
    // a concern of generic arrow-hdf).
    const HierNode* run = child(h.root, "run");
    check(run && run->children.size() == 2, "run has two children");
    check(run && run->children[0].name == "1" && run->children[1].name == "10",
          "run children lexicographic: 1 then 10");

    // run/1/event children "12" then "2" (lexicographic).
    const HierNode* run1 = run ? child(*run, "1") : nullptr;
    const HierNode* ev = run1 ? child(*run1, "event") : nullptr;
    check(ev && ev->children.size() == 2 && ev->children[0].name == "12"
          && ev->children[1].name == "2", "events lexicographic: 12 then 2");

    // product_paths(): sorted DFS, all leaves marked.
    std::vector<std::string> expected{
        "/job/meta",
        "/run/1/event/12/sigproc/frame",
        "/run/1/event/2/sigproc/frame",
        "/run/1/event/2/sim/depos",
        "/run/10/event/1/sigproc/frame",
    };
    check(h.product_paths() == expected, "product_paths sorted");

    // Empty input -> empty trie.
    Hierarchy empty = Hierarchy::from_addresses({});
    check(empty.root.children.empty() && !empty.root.product && empty.product_paths().empty(),
          "empty input");

    if (fails) { std::cerr << fails << " failures\n"; return 1; }
    std::cout << "hierarchy OK\n";
    return 0;
}
