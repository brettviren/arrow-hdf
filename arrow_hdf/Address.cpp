#include "arrow_hdf/Address.h"

#include <cctype>
#include <stdexcept>

namespace arrow_hdf {

namespace {
bool is_safe(unsigned char c)
{
    return std::isalnum(c) || c == '.' || c == '_' || c == '-';
}
char hex_digit(int v) { return static_cast<char>(v < 10 ? '0' + v : 'A' + (v - 10)); }
int  hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
}  // namespace

std::string path_escape(const std::string& component)
{
    std::string out;
    out.reserve(component.size());
    for (unsigned char c : component) {
        if (is_safe(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex_digit(c >> 4));
            out.push_back(hex_digit(c & 0xF));
        }
    }
    return out;
}

std::string path_unescape(const std::string& component)
{
    std::string out;
    out.reserve(component.size());
    for (std::size_t i = 0; i < component.size(); ++i) {
        char c = component[i];
        if (c != '%') {
            out.push_back(c);
            continue;
        }
        if (i + 2 >= component.size()) {
            throw std::invalid_argument("path_unescape: truncated % escape in '" + component + "'");
        }
        int hi = hex_value(component[i + 1]);
        int lo = hex_value(component[i + 2]);
        if (hi < 0 || lo < 0) {
            throw std::invalid_argument("path_unescape: bad % escape in '" + component + "'");
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    return out;
}

Address::Address(std::vector<Cell> cells, std::string creator, std::string product)
  : m_cells(std::move(cells)), m_creator(std::move(creator)), m_product(std::move(product))
{
}

Address& Address::push(std::string layer, std::int64_t number)
{
    m_cells.push_back(Cell{std::move(layer), number});
    return *this;
}

std::string Address::group_path() const
{
    std::string p;
    for (const auto& cell : m_cells) {
        p.push_back('/');
        p += path_escape(cell.layer);
        p.push_back('/');
        p += std::to_string(cell.number);
    }
    if (p.empty()) p.push_back('/');   // job root
    return p;
}

std::string Address::path() const
{
    std::string p;
    for (const auto& cell : m_cells) {
        p.push_back('/');
        p += path_escape(cell.layer);
        p.push_back('/');
        p += std::to_string(cell.number);
    }
    p.push_back('/');
    p += path_escape(m_creator);
    p.push_back('/');
    p += path_escape(m_product);
    return p;
}

Address Address::from_path(const std::string& path)
{
    if (path.empty() || path.front() != '/') {
        throw std::invalid_argument("Address::from_path: must start with '/': '" + path + "'");
    }
    // Split on '/' dropping the leading empty token.
    std::vector<std::string> parts;
    std::size_t start = 1;
    while (start <= path.size()) {
        std::size_t slash = path.find('/', start);
        if (slash == std::string::npos) {
            parts.push_back(path.substr(start));
            break;
        }
        parts.push_back(path.substr(start, slash - start));
        start = slash + 1;
    }
    // Need at least creator + product; cell part must be even.
    if (parts.size() < 2) {
        throw std::invalid_argument("Address::from_path: need creator and product: '" + path + "'");
    }
    const std::size_t ncells = parts.size() - 2;
    if (ncells % 2 != 0) {
        throw std::invalid_argument("Address::from_path: cell components not paired: '" + path + "'");
    }
    Address addr;
    for (std::size_t i = 0; i < ncells; i += 2) {
        const std::string layer = path_unescape(parts[i]);
        std::int64_t number = 0;
        try {
            std::size_t consumed = 0;
            number = std::stoll(parts[i + 1], &consumed);
            if (consumed != parts[i + 1].size()) throw std::invalid_argument("trailing");
        } catch (const std::exception&) {
            throw std::invalid_argument("Address::from_path: bad cell number '" + parts[i + 1] + "'");
        }
        addr.push(layer, number);
    }
    addr.set_creator(path_unescape(parts[parts.size() - 2]));
    addr.set_product(path_unescape(parts[parts.size() - 1]));
    return addr;
}

}  // namespace arrow_hdf
