// Unit tests for arrow_hdf::Address and path escaping (ddm-c3s.1, item 1).

#include "arrow_hdf/Address.h"

#include <iostream>
#include <stdexcept>
#include <string>

using arrow_hdf::Address;
using arrow_hdf::Cell;
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
    // NUL byte
    {
        std::string s("a\0b", 3);
        std::string esc = path_escape(s);
        check(esc == "a%00b", "NUL escaped");
        check(path_unescape(esc) == s, "NUL round-trip");
    }
    // malformed escapes throw
    try { path_unescape("bad%"); check(false, "truncated escape should throw"); }
    catch (const std::invalid_argument&) {}
    try { path_unescape("bad%ZZ"); check(false, "non-hex escape should throw"); }
    catch (const std::invalid_argument&) {}

    // --- path construction ---
    {
        Address a;
        a.push("run", 3).push("event", 12);
        a.set_creator("sigproc");
        a.set_product("frame");
        check(a.path() == "/run/3/event/12/sigproc/frame", "path build: got " + a.path());
        check(a.group_path() == "/run/3/event/12", "group_path: got " + a.group_path());
    }
    // job-root address (no cells)
    {
        Address a({}, "job", "summary");
        check(a.path() == "/job/summary", "root path: got " + a.path());
        check(a.group_path() == "/", "root group_path: got " + a.group_path());
    }
    // components needing escaping
    {
        Address a;
        a.push("a/b", 5);
        a.set_creator("cr e");
        a.set_product("p/q");
        check(a.path() == "/a%2Fb/5/cr%20e/p%2Fq", "escaped path: got " + a.path());
    }

    // --- from_path is the inverse of path() ---
    for (const Address& a : {
             Address(std::vector<Cell>{{"run", 3}, {"event", 12}}, "sigproc", "frame"),
             Address({}, "job", "summary"),
             Address(std::vector<Cell>{{"a/b", 5}}, "cr e", "p/q"),
             Address(std::vector<Cell>{{"run", 1000000000000LL}}, "c", "p"),
         }) {
        Address back = Address::from_path(a.path());
        check(back == a, "from_path round-trip for " + a.path());
    }

    // --- from_path error cases ---
    try { Address::from_path("no-leading-slash"); check(false, "missing leading / should throw"); }
    catch (const std::invalid_argument&) {}
    try { Address::from_path("/onlyone"); check(false, "need creator+product should throw"); }
    catch (const std::invalid_argument&) {}
    try { Address::from_path("/run/3/event/creator/product"); check(false, "unpaired cells should throw"); }
    catch (const std::invalid_argument&) {}
    try { Address::from_path("/run/notanum/creator/product"); check(false, "bad number should throw"); }
    catch (const std::invalid_argument&) {}

    if (fails) { std::cerr << fails << " failures\n"; return 1; }
    std::cout << "address OK\n";
    return 0;
}
