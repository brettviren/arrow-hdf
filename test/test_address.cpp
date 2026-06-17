// Unit tests for arrow_hdf::Address (generic flat path) and path escaping.

#include "arrow_hdf/Address.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using arrow_hdf::Address;
using arrow_hdf::path_escape;
using arrow_hdf::path_unescape;

static int fails = 0;
static void check(bool ok, const std::string& what)
{
    if (!ok) { std::cerr << "FAIL: " << what << "\n"; ++fails; }
}

int main()
{
    // --- escaping is reversible and forbids '/' and NUL ---
    for (const std::string& s : {std::string("plain"), std::string("a/b"),
                                 std::string("with space"), std::string("dotted.name_-"),
                                 std::string("100%cpu"), std::string("\x01\x02")}) {
        std::string esc = path_escape(s);
        check(esc.find('/') == std::string::npos, "escaped has no '/': " + s);
        check(path_unescape(esc) == s, "escape round-trip: " + s);
    }
    {
        std::string s("a\0b", 3);
        check(path_escape(s) == "a%00b", "NUL escaped");
        check(path_unescape("a%00b") == s, "NUL round-trip");
    }
    try { path_unescape("bad%"); check(false, "truncated escape should throw"); }
    catch (const std::invalid_argument&) {}
    try { path_unescape("bad%ZZ"); check(false, "non-hex escape should throw"); }
    catch (const std::invalid_argument&) {}

    // --- Address from components: each escaped, joined with '/' ---
    {
        Address a(std::vector<std::string>{"run", "3", "event", "12", "sigproc", "frame"});
        check(a.path() == "/run/3/event/12/sigproc/frame", "components path: " + a.path());
        check(a.group_path() == "/run/3/event/12/sigproc", "group_path (parent): " + a.group_path());
    }
    // empty component list -> root
    {
        Address a(std::vector<std::string>{});
        check(a.path() == "/", "empty -> root: " + a.path());
        check(a.group_path() == "/", "root group_path: " + a.group_path());
    }
    // single component
    {
        Address a(std::vector<std::string>{"only"});
        check(a.path() == "/only", "single path");
        check(a.group_path() == "/", "single group_path -> root");
    }
    // components needing escaping
    {
        Address a(std::vector<std::string>{"a/b", "cr e"});
        check(a.path() == "/a%2Fb/cr%20e", "escaped components: " + a.path());
    }

    // --- Address from a verbatim path string ---
    {
        Address a(std::string("/run/3/x"));
        check(a.path() == "/run/3/x", "verbatim path");
        check(a.group_path() == "/run/3", "verbatim group_path");
    }
    {
        Address a(std::string("a/b"));          // missing leading slash -> normalized
        check(a.path() == "/a/b", "normalized leading slash: " + a.path());
    }
    {
        Address a(std::string(""));             // empty -> root
        check(a.path() == "/", "empty string -> root");
    }

    // --- equality ---
    check(Address(std::vector<std::string>{"a", "b"}) == Address(std::string("/a/b")),
          "component and verbatim addresses compare equal");

    if (fails) { std::cerr << fails << " failures\n"; return 1; }
    std::cout << "address OK\n";
    return 0;
}
