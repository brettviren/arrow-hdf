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

Address::Address(const std::vector<std::string>& components)
{
    for (const auto& c : components) {
        m_path.push_back('/');
        m_path += path_escape(c);
    }
    if (m_path.empty()) m_path.push_back('/');
}

Address::Address(std::string path) : m_path(std::move(path))
{
    if (m_path.empty()) {
        m_path = "/";
    } else if (m_path.front() != '/') {
        m_path.insert(m_path.begin(), '/');
    }
}

}  // namespace arrow_hdf
