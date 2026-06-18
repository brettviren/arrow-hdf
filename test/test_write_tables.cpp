// Test the generic write_tables helper (a named group of tables under a base).
#include "arrow_hdf/Hdf5File.hpp"
#include "arrow_hdf/Address.hpp"

#include <arrow/api.h>

#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ah = arrow_hdf;
static int fails = 0;
static void check(bool ok, const std::string& w){ std::cout<<(ok?"ok   ":"FAIL ")<<w<<"\n"; if(!ok)++fails; }

static std::shared_ptr<arrow::Table> toy(int v)
{
    arrow::Int32Builder b; (void)b.Append(v); (void)b.Append(v + 1);
    std::shared_ptr<arrow::Array> a; (void)b.Finish(&a);
    return arrow::Table::Make(arrow::schema({arrow::field("x", arrow::int32(), false)}), {a});
}

int main()
{
    const std::string path = "/tmp/arrow_hdf_write_tables.h5";
    auto t1 = toy(10), t2 = toy(20);

    ah::Address base(std::vector<std::string>{"run", "1", "sim", "frame"});
    {
        auto f = ah::Hdf5File::create(path).ValueOrDie();
        auto st = f.write_tables(base, {{"traces", t1}, {"frame_tags", t2}}, "wc.frame");
        check(st.ok(), "write_tables ok");
    }

    auto f = ah::Hdf5File::open_readonly(path).ValueOrDie();
    auto r1 = f.read(ah::Address(std::string("/run/1/sim/frame/traces")));
    auto r2 = f.read(ah::Address(std::string("/run/1/sim/frame/frame_tags")));
    check(r1.ok() && t1->Equals(**&r1.ValueOrDie()), "member 'traces' round-trips");
    check(r2.ok() && t2->Equals(**&r2.ValueOrDie()), "member 'frame_tags' round-trips");

    auto h = f.scan().ValueOrDie();
    check(h.product_paths() == std::vector<std::string>{
              "/run/1/sim/frame/frame_tags", "/run/1/sim/frame/traces"},
          "scan finds both member tables");

    // read_tables: inverse of write_tables — recover the type label + members.
    auto nt = f.read_tables(base);
    check(nt.ok(), "read_tables ok");
    if (nt.ok()) {
        const auto& g = nt.ValueOrDie();
        check(g.type_label == "wc.frame", "read_tables recovers type label");
        check(g.tables.size() == 2 && g.tables.count("traces") && g.tables.count("frame_tags"),
              "read_tables recovers member names");
        check(g.tables.count("traces") && t1->Equals(*g.tables.at("traces")),
              "read_tables 'traces' equals original");
        check(g.tables.count("frame_tags") && t2->Equals(*g.tables.at("frame_tags")),
              "read_tables 'frame_tags' equals original");
    }

    if (fails) return 1;
    std::cout << "write_tables OK\n";
    return 0;
}
