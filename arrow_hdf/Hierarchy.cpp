#include "arrow_hdf/Hierarchy.hpp"

#include <algorithm>

namespace arrow_hdf {

namespace {

// Split "/a/b/c" into {"a","b","c"} (a leading '/' is dropped; "/" -> {}).
std::vector<std::string> split_path(const std::string& path)
{
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start < path.size()) {
        if (path[start] == '/') { ++start; continue; }
        std::size_t slash = path.find('/', start);
        if (slash == std::string::npos) { parts.push_back(path.substr(start)); break; }
        parts.push_back(path.substr(start, slash - start));
        start = slash + 1;
    }
    return parts;
}

HierNode& find_or_make(std::vector<HierNode>& children, const std::string& name)
{
    for (auto& c : children) if (c.name == name) return c;
    children.push_back(HierNode{name, false, {}});
    return children.back();
}

void sort_children(HierNode& node)
{
    std::sort(node.children.begin(), node.children.end(),
              [](const HierNode& a, const HierNode& b) { return a.name < b.name; });
    for (auto& c : node.children) sort_children(c);
}

void collect(const HierNode& node, std::string prefix, std::vector<std::string>& out)
{
    // prefix is the path of `node` (already includes node.name except for root).
    if (node.product) out.push_back(prefix.empty() ? "/" : prefix);
    for (const auto& c : node.children) collect(c, prefix + "/" + c.name, out);
}

}  // namespace

Hierarchy Hierarchy::from_addresses(const std::vector<Address>& addresses)
{
    Hierarchy h;
    for (const auto& addr : addresses) {
        auto parts = split_path(addr.path());
        HierNode* node = &h.root;
        for (const auto& p : parts) node = &find_or_make(node->children, p);
        node->product = true;
    }
    sort_children(h.root);
    return h;
}

std::vector<std::string> Hierarchy::product_paths() const
{
    std::vector<std::string> out;
    collect(root, "", out);
    return out;
}

}  // namespace arrow_hdf
