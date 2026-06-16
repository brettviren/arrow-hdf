// Arrow -> HDF5 -> Arrow round-trip tests (ddm-c3s.1, item 5).
//
// Covers the acceptance shapes: a flat table (tensor-like: large_binary + utf8
// + list<int64> + nullable utf8), a list-column table (trace charge), a nested
// list<struct> table (depo priors), and a dense fixed_size_list table (frame
// waveform).  Also exercises scan().  Tables are built directly with Arrow
// (no WCT) to keep this package WCT-free.

#include "arrow_hdf/Hdf5File.h"
#include "arrow_hdf/Address.h"

#include <arrow/api.h>

#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace arrow_hdf;
namespace ar = arrow;

static int fails = 0;
static void check(bool ok, const std::string& what)
{
    std::cout << (ok ? "ok   " : "FAIL ") << what << "\n";
    if (!ok) ++fails;
}

template <typename Builder, typename... Args>
std::shared_ptr<ar::Array> finish(Builder& b)
{
    std::shared_ptr<ar::Array> a;
    if (!b.Finish(&a).ok()) { std::cerr << "builder finish failed\n"; }
    return a;
}

// --- table builders -------------------------------------------------------

std::shared_ptr<ar::Table> make_flat_tensor_table()
{
    // data: large_binary
    ar::LargeBinaryBuilder data_b;
    std::vector<uint8_t> b0{1, 2, 3}, b1{4, 5, 6, 7};
    (void)data_b.Append(b0.data(), b0.size());
    (void)data_b.Append(b1.data(), b1.size());
    auto data = finish(data_b);
    // dtype: utf8
    ar::StringBuilder dtype_b; (void)dtype_b.Append("f4"); (void)dtype_b.Append("i4");
    auto dtype = finish(dtype_b);
    // shape: list<int64>
    auto sv = std::make_shared<ar::Int64Builder>();
    ar::ListBuilder shape_b(ar::default_memory_pool(), sv);
    (void)shape_b.Append(); (void)sv->Append(2); (void)sv->Append(3);
    (void)shape_b.Append(); (void)sv->Append(4);
    auto shape = finish(shape_b);
    // metadata: utf8 nullable (one null)
    ar::StringBuilder md_b; (void)md_b.Append("info"); (void)md_b.AppendNull();
    auto md = finish(md_b);

    auto schema = ar::schema(
        {ar::field("data", ar::large_binary(), false),
         ar::field("dtype", ar::utf8(), false),
         ar::field("shape", ar::list(ar::int64()), false),
         ar::field("metadata", ar::utf8(), true)},
        ar::key_value_metadata({{"arrow.schema", "wc.tensor"}}));
    return ar::Table::Make(schema, {data, dtype, shape, md});
}

std::shared_ptr<ar::Table> make_trace_table()
{
    ar::Int32Builder ch_b; (void)ch_b.Append(10); (void)ch_b.Append(20);
    ar::Int32Builder tb_b; (void)tb_b.Append(0); (void)tb_b.Append(5);
    auto cv = std::make_shared<ar::FloatBuilder>();
    ar::ListBuilder cg_b(ar::default_memory_pool(), cv);
    (void)cg_b.Append(); (void)cv->Append(1); (void)cv->Append(2); (void)cv->Append(3);
    (void)cg_b.Append(); (void)cv->Append(4); (void)cv->Append(5);
    auto ch = finish(ch_b); auto tb = finish(tb_b); auto cg = finish(cg_b);
    auto schema = ar::schema(
        {ar::field("wc.trace.channel", ar::int32(), false),
         ar::field("wc.trace.tbin", ar::int32(), false),
         ar::field("wc.trace.charge", ar::list(ar::float32()), false)},
        ar::key_value_metadata({{"arrow.schema", "wc.frame"}}));
    return ar::Table::Make(schema, {ch, tb, cg});
}

std::shared_ptr<ar::Table> make_depo_priors_table()
{
    auto stype = ar::struct_({ar::field("time", ar::float64(), false),
                              ar::field("x", ar::float64(), false),
                              ar::field("id", ar::int32(), false)});
    auto vt = std::make_shared<ar::DoubleBuilder>();
    auto vx = std::make_shared<ar::DoubleBuilder>();
    auto vid = std::make_shared<ar::Int32Builder>();
    auto sb = std::make_shared<ar::StructBuilder>(
        stype, ar::default_memory_pool(),
        std::vector<std::shared_ptr<ar::ArrayBuilder>>{vt, vx, vid});
    ar::ListBuilder priors_b(ar::default_memory_pool(), sb);

    // row 0: one prior struct
    (void)priors_b.Append(); (void)sb->Append();
    (void)vt->Append(5.0); (void)vx->Append(51.0); (void)vid->Append(70);
    // row 1: empty (non-null) list
    (void)priors_b.Append();
    auto priors = finish(priors_b);

    ar::DoubleBuilder time_b; (void)time_b.Append(1.0); (void)time_b.Append(2.0);
    auto time = finish(time_b);

    auto schema = ar::schema(
        {ar::field("wc.depo.time", ar::float64(), false),
         ar::field("wc.depo.priors", priors->type(), false)},
        ar::key_value_metadata({{"arrow.schema", "wc.deposet"}}));
    return ar::Table::Make(schema, {time, priors});
}

std::shared_ptr<ar::Table> make_dense_frame_table()
{
    ar::Int32Builder ch_b; (void)ch_b.Append(10); (void)ch_b.Append(20); (void)ch_b.Append(30);
    auto ch = finish(ch_b);
    auto cv = std::make_shared<ar::FloatBuilder>();
    ar::FixedSizeListBuilder fb(ar::default_memory_pool(), cv, 4);
    for (int r = 0; r < 3; ++r) { (void)fb.Append(); for (int k = 0; k < 4; ++k) (void)cv->Append(r * 10 + k); }
    auto charge = finish(fb);
    auto schema = ar::schema(
        {ar::field("wc.trace.channel", ar::int32(), false),
         ar::field("wc.trace.charge", ar::fixed_size_list(ar::float32(), 4), false)},
        ar::key_value_metadata({{"arrow.schema", "wc.frame.dense"}, {"wc.frame.tbin", "0"}}));
    return ar::Table::Make(schema, {ch, charge});
}

// --- round-trip one table at an address -----------------------------------
static bool roundtrip(Hdf5File& f, const Address& addr, const std::shared_ptr<ar::Table>& t,
                      const std::string& name)
{
    auto st = f.write(addr, t);
    if (!st.ok()) { std::cerr << "write " << name << ": " << st.ToString() << "\n"; return false; }
    auto r = f.read(addr);
    if (!r.ok()) { std::cerr << "read " << name << ": " << r.status().ToString() << "\n"; return false; }
    auto back = *r;
    if (!t->Equals(*back, /*check_metadata=*/true)) {
        std::cerr << "MISMATCH " << name << "\n  orig:\n" << t->ToString()
                  << "\n  back:\n" << back->ToString() << "\n";
        return false;
    }
    return true;
}

int main()
{
    const std::string path = "/tmp/arrow_hdf_roundtrip.h5";

    Address a_tensor(std::vector<Cell>{{"run", 1}}, "sim", "tensors");
    Address a_frame(std::vector<Cell>{{"run", 1}, {"event", 2}}, "sigproc", "frame");
    Address a_depo(std::vector<Cell>{{"run", 1}, {"event", 2}}, "sim", "depos");
    Address a_dense(std::vector<Cell>{{"run", 1}, {"event", 3}}, "sim", "frame_dense");

    {
        auto f = Hdf5File::create(path).ValueOrDie();
        check(roundtrip(f, a_tensor, make_flat_tensor_table(), "flat/tensor"), "flat tensor round-trip");
        check(roundtrip(f, a_frame, make_trace_table(), "list/trace"), "list-column round-trip");
        check(roundtrip(f, a_depo, make_depo_priors_table(), "nested/depo"), "list<struct> round-trip");
        check(roundtrip(f, a_dense, make_dense_frame_table(), "dense/frame"), "dense fixed_size_list round-trip");
    }  // writer closed

    // scan
    {
        auto f = Hdf5File::open_readonly(path).ValueOrDie();
        auto h = f.scan().ValueOrDie();
        check(h.layer_names == std::vector<std::string>{"run", "event"}, "scan layer names");
        check(h.cells.size() == 1 && h.cells[0].number == 1, "scan one run cell");
        const auto& run1 = h.cells[0];
        // run/1 holds the tensor product and has two event children.
        check(run1.products.size() == 1 && run1.products[0] == ProductLoc{"sim", "tensors"},
              "run/1 product");
        check(run1.children.size() == 2 && run1.children[0].number == 2 && run1.children[1].number == 3,
              "run/1 events sorted");
        check(run1.children[0].products.size() == 2, "event 2 has two products");
    }

    if (fails) { std::cerr << fails << " failures\n"; return 1; }
    std::cout << "roundtrip OK\n";
    return 0;
}
