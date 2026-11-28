// pybind11 bindings: drive the C++ simulator from Python.
//
// A Python strategy subclasses tickforge.Strategy (and calls
// super().__init__()); the simulation itself runs in C++ and calls back into
// Python only for the strategy's own events. Context objects passed to the
// callbacks are owned by the simulator and valid only during simulate().

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <string>

#include "tickforge/sim/simulator.hpp"
#include "tickforge/sim/strategies.hpp"
#include "tickforge/version.hpp"

namespace py = pybind11;
using namespace tickforge;
using namespace tickforge::sim;

namespace {

// Forwards every callback to the Python override, if the subclass has one.
class PyStrategy : public Strategy {
public:
    using Strategy::Strategy;
    void on_start(Context& c) override { call("on_start", &c); }
    void on_market(Context& c, const MarketEvent& e) override { call("on_market", &c, e); }
    void on_order(Context& c, const OrderUpdate& u) override { call("on_order", &c, u); }
    void on_fill(Context& c, const FillReport& f) override { call("on_fill", &c, f); }
    void on_timer(Context& c, std::uint64_t id) override { call("on_timer", &c, id); }
    void on_end(Context& c) override { call("on_end", &c); }

private:
    template <typename... A>
    void call(const char* name, A&&... args) {
        py::gil_scoped_acquire gil;
        py::function f = py::get_override(static_cast<const Strategy*>(this), name);
        if (f) f(std::forward<A>(args)...);  // Context* is passed by reference, events by copy
    }
};

}  // namespace

PYBIND11_MODULE(_tickforge, m) {
    m.doc() = "Tickforge pybind11 bindings: queue-aware market simulator on replayed Nasdaq ITCH";
    m.attr("__version__") =
        py::str("{}.{}.{}").format(tickforge::kVersionMajor, tickforge::kVersionMinor, tickforge::kVersionPatch);

    py::enum_<Side>(m, "Side").value("buy", Side::buy).value("sell", Side::sell);
    py::enum_<FillModel>(m, "FillModel")
        .value("queue_aware", FillModel::queue_aware)
        .value("naive_touch", FillModel::naive_touch);
    py::enum_<Liquidity>(m, "Liquidity").value("maker", Liquidity::maker).value("taker", Liquidity::taker);
    py::enum_<OrderStatus>(m, "OrderStatus")
        .value("pending", OrderStatus::pending)
        .value("active", OrderStatus::active)
        .value("filled", OrderStatus::filled)
        .value("cancelled", OrderStatus::cancelled)
        .value("rejected", OrderStatus::rejected)
        .value("replaced", OrderStatus::replaced);
    py::enum_<Reject>(m, "Reject")
        .value("none", Reject::none)
        .value("bad_values", Reject::bad_values)
        .value("post_only_would_cross", Reject::post_only_would_cross)
        .value("self_cross", Reject::self_cross)
        .value("unknown_order", Reject::unknown_order)
        .value("too_late", Reject::too_late)
        .value("after_end", Reject::after_end);
    py::enum_<UpdateKind>(m, "UpdateKind")
        .value("accepted", UpdateKind::accepted)
        .value("rejected", UpdateKind::rejected)
        .value("cancelled", UpdateKind::cancelled)
        .value("replaced", UpdateKind::replaced)
        .value("cancel_rejected", UpdateKind::cancel_rejected);
    py::enum_<DecisionKind>(m, "DecisionKind")
        .value("place", DecisionKind::place)
        .value("cancel", DecisionKind::cancel)
        .value("replace", DecisionKind::replace);

    py::class_<FeeSchedule>(m, "FeeSchedule")
        .def(py::init<>())
        .def_readwrite("maker_rebate_per_share", &FeeSchedule::maker_rebate_per_share, "micro-dollars per share")
        .def_readwrite("taker_fee_per_share", &FeeSchedule::taker_fee_per_share, "micro-dollars per share");

    py::class_<SimConfig>(m, "SimConfig")
        .def(py::init<>())
        .def_readwrite("symbol", &SimConfig::symbol)
        .def_readwrite("model", &SimConfig::model)
        .def_readwrite("order_latency_ns", &SimConfig::order_latency_ns)
        .def_readwrite("market_data_latency_ns", &SimConfig::market_data_latency_ns)
        .def_readwrite("fees", &SimConfig::fees)
        .def_readwrite("start_ns", &SimConfig::start_ns)
        .def_readwrite("end_ns", &SimConfig::end_ns)
        .def_readwrite("pnl_sample_interval_ns", &SimConfig::pnl_sample_interval_ns)
        .def_readwrite("markout_horizons_ns", &SimConfig::markout_horizons_ns);

    py::class_<book::LevelView>(m, "LevelView")
        .def_readonly("price", &book::LevelView::price)
        .def_readonly("shares", &book::LevelView::shares)
        .def_readonly("orders", &book::LevelView::orders)
        .def("__repr__", [](const book::LevelView& l) {
            return "LevelView(price=" + std::to_string(l.price) + ", shares=" + std::to_string(l.shares) +
                   ", orders=" + std::to_string(l.orders) + ")";
        });

    py::class_<MarketEvent>(m, "MarketEvent")
        .def_readonly("ts", &MarketEvent::ts)
        .def_readonly("type", &MarketEvent::type)
        .def_readonly("side", &MarketEvent::side)
        .def_readonly("price", &MarketEvent::price)
        .def_readonly("shares", &MarketEvent::shares)
        .def_readonly("ref", &MarketEvent::ref)
        .def_readonly("new_ref", &MarketEvent::new_ref)
        .def_readonly("flag", &MarketEvent::flag);

    py::class_<OrderUpdate>(m, "OrderUpdate")
        .def_readonly("exchange_ts", &OrderUpdate::exchange_ts)
        .def_readonly("id", &OrderUpdate::id)
        .def_readonly("kind", &OrderUpdate::kind)
        .def_readonly("reason", &OrderUpdate::reason)
        .def_readonly("open_qty", &OrderUpdate::open_qty)
        .def_readonly("cancelled_qty", &OrderUpdate::cancelled_qty)
        .def_readonly("new_id", &OrderUpdate::new_id);

    py::class_<FillReport>(m, "FillReport")
        .def_readonly("exchange_ts", &FillReport::exchange_ts)
        .def_readonly("id", &FillReport::id)
        .def_readonly("side", &FillReport::side)
        .def_readonly("price", &FillReport::price)
        .def_readonly("qty", &FillReport::qty)
        .def_readonly("liquidity", &FillReport::liquidity)
        .def_readonly("fee", &FillReport::fee)
        .def_readonly("open_qty", &FillReport::open_qty);

    py::class_<OrderFlags>(m, "OrderFlags")
        .def(py::init<>())
        .def_readwrite("post_only", &OrderFlags::post_only)
        .def_readwrite("ioc", &OrderFlags::ioc);

    py::class_<KnownOrder>(m, "KnownOrder")
        .def_readonly("id", &KnownOrder::id)
        .def_readonly("side", &KnownOrder::side)
        .def_readonly("price", &KnownOrder::price)
        .def_readonly("qty", &KnownOrder::qty)
        .def_readonly("open", &KnownOrder::open)
        .def_readonly("filled", &KnownOrder::filled)
        .def_readonly("status", &KnownOrder::status)
        .def_readonly("flags", &KnownOrder::flags);

    py::class_<FillRecord>(m, "FillRecord")
        .def_readonly("report", &FillRecord::report)
        .def_readonly("trigger", &FillRecord::trigger)
        .def_readonly("mid2_before", &FillRecord::mid2_before)
        .def_readonly("mid2_after", &FillRecord::mid2_after)
        .def_readonly("markout_ok", &FillRecord::markout_ok)
        .def("markout", &FillRecord::markout, "side sign x (mid after horizon - mid before), $/share")
        .def("markout_vs_price", &FillRecord::markout_vs_price, "side sign x (mid after horizon - fill price), $/share");

    py::class_<PnlSample>(m, "PnlSample")
        .def_readonly("ts", &PnlSample::ts)
        .def_readonly("cash", &PnlSample::cash)
        .def_readonly("inventory", &PnlSample::inventory)
        .def_readonly("mid2", &PnlSample::mid2)
        .def_readonly("mtm", &PnlSample::mtm);

    py::class_<Decision>(m, "Decision")
        .def_readonly("ts", &Decision::ts)
        .def_readonly("kind", &Decision::kind)
        .def_readonly("id", &Decision::id)
        .def_readonly("new_id", &Decision::new_id)
        .def_readonly("side", &Decision::side)
        .def_readonly("price", &Decision::price)
        .def_readonly("qty", &Decision::qty)
        .def("__eq__", [](const Decision& a, const Decision& b) { return a == b; });

    py::class_<SimStats>(m, "SimStats")
        .def_readonly("orders_sent", &SimStats::orders_sent)
        .def_readonly("cancels_sent", &SimStats::cancels_sent)
        .def_readonly("accepted", &SimStats::accepted)
        .def_readonly("rejected", &SimStats::rejected, "indexed by Reject value")
        .def_readonly("fills", &SimStats::fills)
        .def_readonly("filled_shares", &SimStats::filled_shares)
        .def_readonly("placed_shares", &SimStats::placed_shares)
        .def_readonly("maker_shares", &SimStats::maker_shares)
        .def_readonly("taker_shares", &SimStats::taker_shares)
        .def_readonly("bought_shares", &SimStats::bought_shares)
        .def_readonly("sold_shares", &SimStats::sold_shares)
        .def_readonly("rebates", &SimStats::rebates)
        .def_readonly("fees_paid", &SimStats::fees_paid)
        .def_readonly("max_inventory", &SimStats::max_inventory)
        .def_readonly("min_inventory", &SimStats::min_inventory)
        .def_readonly("anomaly_exec_with_ahead", &SimStats::anomaly_exec_with_ahead)
        .def_readonly("anomaly_cross_add_with_ahead", &SimStats::anomaly_cross_add_with_ahead)
        .def_readonly("ahead_underflow", &SimStats::ahead_underflow)
        .def_property_readonly("markouts_unavailable",
                               [](const SimStats& s) {
                                   return std::vector<std::uint64_t>(std::begin(s.markouts_unavailable),
                                                                     std::end(s.markouts_unavailable));
                               })
        .def_readonly("strategy_callbacks", &SimStats::strategy_callbacks)
        .def_readonly("lookahead_violations", &SimStats::lookahead_violations)
        .def_readonly("messages", &SimStats::messages)
        .def_readonly("stock_messages", &SimStats::stock_messages);

    py::class_<SimResult>(m, "SimResult")
        .def_readonly("symbol_found", &SimResult::symbol_found)
        .def_readonly("locate", &SimResult::locate)
        .def_readonly("fills", &SimResult::fills)
        .def_readonly("pnl", &SimResult::pnl)
        .def_readonly("decisions", &SimResult::decisions)
        .def_readonly("stats", &SimResult::stats)
        .def_readonly("end_ts", &SimResult::end_ts)
        .def_readonly("final_cash", &SimResult::final_cash)
        .def_readonly("final_inventory", &SimResult::final_inventory)
        .def_readonly("final_mid2", &SimResult::final_mid2)
        .def_readonly("final_mtm", &SimResult::final_mtm);

    // Owned by the simulator: never deleted from Python.
    py::class_<Context, std::unique_ptr<Context, py::nodelete>>(m, "Context")
        .def("now", &Context::now)
        .def("best_bid", &Context::best_bid)
        .def("best_ask", &Context::best_ask)
        .def("depth", &Context::depth, py::arg("side"), py::arg("n"))
        .def("trading_state", &Context::trading_state)
        .def(
            "place",
            [](Context& c, Side side, Price price, Qty qty, bool post_only, bool ioc) {
                OrderFlags f;
                f.post_only = post_only;
                f.ioc = ioc;
                return c.place(side, price, qty, f);
            },
            py::arg("side"), py::arg("price"), py::arg("qty"), py::arg("post_only") = false, py::arg("ioc") = false)
        .def("cancel", &Context::cancel, py::arg("id"), py::arg("shares") = 0)
        .def("replace", &Context::replace, py::arg("id"), py::arg("price"), py::arg("qty"))
        .def("set_timer", &Context::set_timer, py::arg("at_ns"))
        .def("position", &Context::position)
        .def("cash", &Context::cash)
        .def(
            "order",
            [](const Context& c, ShadowId id) -> std::optional<KnownOrder> {
                const KnownOrder* k = c.order(id);
                if (k == nullptr) return std::nullopt;
                return *k;
            },
            py::arg("id"));

    py::class_<Strategy, PyStrategy>(m, "Strategy")
        .def(py::init<>())
        .def("on_start", &Strategy::on_start)
        .def("on_market", &Strategy::on_market)
        .def("on_order", &Strategy::on_order)
        .def("on_fill", &Strategy::on_fill)
        .def("on_timer", &Strategy::on_timer)
        .def("on_end", &Strategy::on_end);

    py::class_<JoinBestQuoter, Strategy>(m, "JoinBestQuoter")
        .def(py::init<Qty, std::int64_t>(), py::arg("size") = 100, py::arg("max_inventory") = 500);

    m.def(
        "simulate",
        [](const std::string& path, const SimConfig& cfg, Strategy& strategy) { return run_file(path, cfg, strategy); },
        py::arg("path"), py::arg("config"), py::arg("strategy"),
        "Replays a .tfc, .gz or raw ITCH file and runs the strategy on config.symbol.");
}
