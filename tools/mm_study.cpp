// mm_study: the Step 4 market-making study (PROGRESS.md, Rules M-Q).
//
//   mm_study <file.tfc> <out_dir> <trials.jsonl> <trial_number> <commit>
//
// Runs the pre-registered grid: strategy {symmetric, avellaneda_stoikov} x
// symbol {AAPL, MSFT, INTC, COLB} x fill model {naive, queue-aware} x latency
// {0, 10 us, 100 us, 1 ms, 10 ms} (both directions). Writes CSV files and a
// Markdown report to out_dir and appends one line per configuration to the
// trials log (Rule Q). It refuses to run if the trial number is already in
// the log, so a rerun can never overwrite an earlier trial. Every number
// written is measured from the runs.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "tickforge/sim/mm_strategies.hpp"
#include "tickforge/sim/simulator.hpp"

using namespace tickforge;
using namespace tickforge::sim;

namespace {

constexpr std::uint64_t kSec = 1'000'000'000ULL;
constexpr std::uint64_t kHour = 3'600 * kSec;
constexpr std::uint64_t kMinute = 60 * kSec;
constexpr std::uint64_t kSimStart = 9 * kHour + 30 * kMinute;
constexpr std::uint64_t kQuoteStart = 9 * kHour + 35 * kMinute;
constexpr std::uint64_t kEnd = 15 * kHour + 55 * kMinute;
const std::vector<std::uint64_t> kHorizons = {0, 1'000'000, 10'000'000, 100'000'000, kSec, 10 * kSec, 60 * kSec};
constexpr std::size_t kH1s = 4;  // index of the 1 s horizon
const char* kSymbols[] = {"AAPL", "MSFT", "INTC", "COLB"};
const char* kStrategies[] = {"symmetric", "avellaneda_stoikov"};
const std::uint64_t kLatencies[] = {0, 10'000, 100'000, 1'000'000, 10'000'000};
constexpr Qty kSize = 100;
constexpr std::int64_t kMaxInv = 500;
constexpr double kGamma = 4.56e-5;

struct Config {
    std::string strategy, symbol;
    FillModel model;
    std::uint64_t latency;
};

struct Markout {
    double mid = 0, px = 0;  // share-weighted sums, $/share x shares
    std::uint64_t shares = 0, fills = 0;
};

struct Run {
    Config cfg;
    SimResult r;
    QuoteStats q;
    std::size_t as_sigma_changes = 0, as_execs = 0;
    std::vector<Markout> mk;
    double mean_abs_inv = 0;
    std::uint64_t trig_e = 0, trig_c = 0, trig_add = 0, trig_pq = 0;
    double half_spread_cents = 0;
    bool accounting_ok = false;
    bool valid = false;
};

const char* model_name(FillModel m) { return m == FillModel::queue_aware ? "queue_aware" : "naive"; }
std::string lat_name(std::uint64_t ns) {
    if (ns == 0) return "0";
    if (ns >= 1'000'000) return std::to_string(ns / 1'000'000) + " ms";
    return std::to_string(ns / 1'000) + " us";
}
std::string fx(double v, int d) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", d, v);
    return b;
}
std::string usd(Micros m) { return fx(static_cast<double>(m) / 1e6, 2); }
std::string num(std::uint64_t v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ",");
    return s;
}
std::string inum(std::int64_t v) { return v < 0 ? "-" + num(static_cast<std::uint64_t>(-v)) : num(static_cast<std::uint64_t>(v)); }

double fill_rate(const SimStats& s) {
    return s.placed_shares ? static_cast<double>(s.filled_shares) / static_cast<double>(s.placed_shares) : 0.0;
}
std::int64_t max_abs_inv(const SimStats& s) { return std::max(s.max_inventory, -s.min_inventory); }
Micros gross(const Run& w) { return w.r.final_mtm - w.r.stats.rebates + w.r.stats.fees_paid; }
double per_share(const Run& w) {
    return w.r.stats.filled_shares ? static_cast<double>(w.r.final_mtm) / 1e6 / static_cast<double>(w.r.stats.filled_shares)
                                   : 0.0;
}
double mk_mid(const Run& w, std::size_t h) {
    return w.mk[h].shares ? 100.0 * w.mk[h].mid / static_cast<double>(w.mk[h].shares) : NAN;
}
double mk_px(const Run& w, std::size_t h) {
    return w.mk[h].shares ? 100.0 * w.mk[h].px / static_cast<double>(w.mk[h].shares) : NAN;
}
std::string opt(double v, int d) { return std::isnan(v) ? "" : fx(v, d); }
std::string optmd(double v, int d) { return std::isnan(v) ? "-" : fx(v, d); }

Run run_one(const std::string& path, const Config& c) {
    SimConfig sc;
    sc.symbol = c.symbol;
    sc.model = c.model;
    sc.order_latency_ns = c.latency;
    sc.market_data_latency_ns = c.latency;
    sc.start_ns = kSimStart;
    sc.end_ns = kEnd;
    sc.markout_horizons_ns = kHorizons;
    const QuoterParams qp{kSize, kMaxInv, kQuoteStart};
    Run w;
    w.cfg = c;
    if (c.strategy == "symmetric") {
        SymmetricInsideQuoter s(qp);
        w.r = run_file(path, sc, s);
        w.q = s.quote_stats();
    } else {
        AsParams ap;
        ap.gamma = kGamma;
        ap.horizon_end_ns = kEnd;
        AvellanedaStoikov s(qp, ap);
        w.r = run_file(path, sc, s);
        w.q = s.quote_stats();
        w.as_sigma_changes = s.sigma_changes();
        w.as_execs = s.executions();
    }
    const SimResult& r = w.r;
    // Markouts, triggers, half-spread captured.
    w.mk.assign(kHorizons.size(), {});
    double cap = 0;
    std::uint64_t cap_sh = 0;
    Micros cash = 0;
    std::int64_t inv = 0;
    for (const FillRecord& f : r.fills) {
        for (std::size_t h = 0; h < kHorizons.size(); ++h) {
            if (!f.markout_ok[h]) continue;
            w.mk[h].mid += f.markout(h) * f.report.qty;
            w.mk[h].px += f.markout_vs_price(h) * f.report.qty;
            w.mk[h].shares += f.report.qty;
            ++w.mk[h].fills;
        }
        switch (f.trigger) {
            case 'E': ++w.trig_e; break;
            case 'C': ++w.trig_c; break;
            case 'A': case 'F': case 'U': ++w.trig_add; break;
            case 'P': case 'Q': ++w.trig_pq; break;
            default: break;
        }
        const double sign = f.report.side == Side::buy ? 1.0 : -1.0;
        if (f.mid2_before != 0) {
            cap += sign * (static_cast<double>(f.mid2_before) / 2.0 - static_cast<double>(f.report.price)) / 1e4 *
                   f.report.qty;
            cap_sh += f.report.qty;
        }
        const Micros notional = static_cast<Micros>(f.report.price) * f.report.qty * kMicrosPerTick;
        cash += (f.report.side == Side::buy ? -notional : notional) - f.report.fee;
        inv += f.report.side == Side::buy ? static_cast<std::int64_t>(f.report.qty) : -static_cast<std::int64_t>(f.report.qty);
    }
    w.half_spread_cents = cap_sh ? 100.0 * cap / static_cast<double>(cap_sh) : NAN;
    w.accounting_ok = cash == r.final_cash && inv == r.final_inventory;
    // Time-weighted mean |inventory| over [09:35, 15:55]. Inventory only changes
    // at fills, and every fill is a P&L sample.
    double area = 0;
    for (std::size_t i = 0; i < r.pnl.size(); ++i) {
        const std::uint64_t a = std::max(r.pnl[i].ts, kQuoteStart);
        const std::uint64_t b = std::min(i + 1 < r.pnl.size() ? r.pnl[i + 1].ts : kEnd, kEnd);
        if (b > a) area += static_cast<double>(std::llabs(r.pnl[i].inventory)) * static_cast<double>(b - a);
    }
    w.mean_abs_inv = area / static_cast<double>(kEnd - kQuoteStart);
    w.valid = r.symbol_found && r.stats.lookahead_violations == 0 && r.stats.ahead_underflow == 0 && w.accounting_ok;
    return w;
}

std::string iso_now() {
    std::time_t t = std::time(nullptr);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d", std::localtime(&t));
    return b;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 6) {
        std::cerr << "usage: mm_study <file.tfc> <out_dir> <trials.jsonl> <trial_number> <commit>\n";
        return 2;
    }
    const std::string path = argv[1];
    const std::filesystem::path out = argv[2];
    const std::string trials = argv[3];
    const int trial = std::stoi(argv[4]);
    const std::string commit = argv[5];

    // Rule Q: never reuse a trial number.
    {
        std::ifstream in(trials);
        std::string line;
        const std::string key = "\"trial\": " + std::to_string(trial) + ",";
        while (std::getline(in, line)) {
            if (line.find(key) != std::string::npos) {
                std::cerr << "trial " << trial << " is already in " << trials << "; use a new trial number\n";
                return 3;
            }
        }
    }
    if (!std::filesystem::exists(path)) {
        std::cerr << "missing " << path << "\n";
        return 1;
    }
    std::filesystem::create_directories(out);

    std::vector<Config> grid;
    for (const char* st : kStrategies)
        for (const char* sym : kSymbols)
            for (FillModel m : {FillModel::naive_touch, FillModel::queue_aware})
                for (std::uint64_t lat : kLatencies) grid.push_back({st, sym, m, lat});

    std::vector<Run> runs(grid.size());
    std::atomic<std::size_t> next{0};
    std::mutex log_mu;
    std::string error;
    const unsigned n_threads = std::max(1u, std::min(std::thread::hardware_concurrency(), 12u));
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < n_threads; ++t) {
        pool.emplace_back([&] {
            for (std::size_t i; (i = next.fetch_add(1)) < grid.size();) {
                try {
                    runs[i] = run_one(path, grid[i]);
                    std::lock_guard<std::mutex> g(log_mu);
                    std::cerr << "done " << grid[i].strategy << " " << grid[i].symbol << " "
                              << model_name(grid[i].model) << " " << lat_name(grid[i].latency) << "\n";
                } catch (const std::exception& e) {
                    std::lock_guard<std::mutex> g(log_mu);
                    error = e.what();
                }
            }
        });
    }
    for (auto& th : pool) th.join();
    if (!error.empty()) {
        std::cerr << "error: " << error << "\n";
        return 1;
    }

    auto find = [&](const std::string& st, const std::string& sym, FillModel m, std::uint64_t lat) -> const Run& {
        for (const Run& w : runs)
            if (w.cfg.strategy == st && w.cfg.symbol == sym && w.cfg.model == m && w.cfg.latency == lat) return w;
        throw std::logic_error("missing run");
    };

    // ---- runs.csv ---------------------------------------------------------------
    {
        std::ofstream f(out / "runs.csv");
        f << "trial,config_id,strategy,symbol,fill_model,latency_ns,valid,orders_sent,accepted,rejected_post_only,"
             "rejected_self_cross,rejected_unknown,rejected_too_late,cancels_sent,fills,filled_shares,accepted_shares,"
             "fill_rate,bought_shares,sold_shares,max_abs_inventory,end_inventory,mean_abs_inventory,cash_usd,"
             "rebates_usd,fees_usd,pnl_usd,gross_pnl_usd,pnl_per_share_usd,end_mid_usd,half_spread_captured_cents,"
             "fills_by_exec_E,fills_by_exec_C,fills_by_crossing_add,fills_by_P_or_Q,quotes,quotes_improved,"
             "quotes_at_best,quotes_behind,as_sigma_changes,as_executions,anomaly_exec_with_ahead,"
             "anomaly_cross_add_with_ahead,lookahead_violations,ahead_underflow,accounting_ok";
        for (std::uint64_t h : kHorizons) f << ",markout_mid_cents_" << h << "ns";
        for (std::uint64_t h : kHorizons) f << ",markout_vs_fill_cents_" << h << "ns";
        f << "\n";
        for (std::size_t i = 0; i < runs.size(); ++i) {
            const Run& w = runs[i];
            const SimStats& s = w.r.stats;
            auto rej = [&](Reject x) { return s.rejected[static_cast<std::size_t>(x)]; };
            f << trial << "," << trial << "-" << (i + 1) << "," << w.cfg.strategy << "," << w.cfg.symbol << ","
              << model_name(w.cfg.model) << "," << w.cfg.latency << "," << (w.valid ? 1 : 0) << "," << s.orders_sent
              << "," << s.accepted << "," << rej(Reject::post_only_would_cross) << "," << rej(Reject::self_cross) << ","
              << rej(Reject::unknown_order) << "," << rej(Reject::too_late) << "," << s.cancels_sent << "," << s.fills
              << "," << s.filled_shares << "," << s.placed_shares << "," << fx(fill_rate(s), 6) << ","
              << s.bought_shares << "," << s.sold_shares << "," << max_abs_inv(s) << "," << w.r.final_inventory << ","
              << fx(w.mean_abs_inv, 2) << "," << usd(w.r.final_cash) << "," << usd(s.rebates) << ","
              << usd(s.fees_paid) << "," << usd(w.r.final_mtm) << "," << usd(gross(w)) << ","
              << fx(per_share(w), 6) << "," << fx(static_cast<double>(w.r.final_mid2) / 2e4, 4) << ","
              << opt(w.half_spread_cents, 4) << "," << w.trig_e << "," << w.trig_c << "," << w.trig_add << ","
              << w.trig_pq << "," << w.q.quotes << "," << w.q.improved << "," << w.q.at_best << "," << w.q.behind
              << "," << w.as_sigma_changes << "," << w.as_execs << "," << s.anomaly_exec_with_ahead << ","
              << s.anomaly_cross_add_with_ahead << "," << s.lookahead_violations << "," << s.ahead_underflow << ","
              << (w.accounting_ok ? 1 : 0);
            for (std::size_t h = 0; h < kHorizons.size(); ++h) f << "," << opt(mk_mid(w, h), 4);
            for (std::size_t h = 0; h < kHorizons.size(); ++h) f << "," << opt(mk_px(w, h), 4);
            f << "\n";
        }
    }
    // ---- markouts.csv -------------------------------------------------------------
    {
        std::ofstream f(out / "markouts.csv");
        f << "strategy,symbol,fill_model,latency_ns,horizon_ns,fills_measured,shares_measured,markout_mid_cents,"
             "markout_vs_fill_cents\n";
        for (const Run& w : runs)
            for (std::size_t h = 0; h < kHorizons.size(); ++h)
                f << w.cfg.strategy << "," << w.cfg.symbol << "," << model_name(w.cfg.model) << "," << w.cfg.latency
                  << "," << kHorizons[h] << "," << w.mk[h].fills << "," << w.mk[h].shares << ","
                  << opt(mk_mid(w, h), 4) << "," << opt(mk_px(w, h), 4) << "\n";
    }
    // ---- headline, invented profit, latency curve --------------------------------
    {
        std::ofstream f(out / "headline_latency0.csv");
        f << "strategy,symbol,fill_model,pnl_usd,fills,fill_rate,markout_1s_mid_cents,markout_1s_vs_fill_cents,"
             "max_abs_inventory,pnl_per_share_usd\n";
        for (const char* st : kStrategies)
            for (const char* sym : kSymbols)
                for (FillModel m : {FillModel::naive_touch, FillModel::queue_aware}) {
                    const Run& w = find(st, sym, m, 0);
                    f << st << "," << sym << "," << model_name(m) << "," << usd(w.r.final_mtm) << ","
                      << w.r.stats.fills << "," << fx(fill_rate(w.r.stats), 6) << "," << opt(mk_mid(w, kH1s), 4)
                      << "," << opt(mk_px(w, kH1s), 4) << "," << max_abs_inv(w.r.stats) << ","
                      << fx(per_share(w), 6) << "\n";
                }
    }
    {
        std::ofstream f(out / "invented_profit.csv");
        f << "strategy,symbol,latency_ns,pnl_naive_usd,pnl_queue_aware_usd,invented_profit_usd,filled_shares_naive,"
             "filled_shares_queue_aware\n";
        for (const char* st : kStrategies)
            for (const char* sym : kSymbols)
                for (std::uint64_t lat : kLatencies) {
                    const Run& n = find(st, sym, FillModel::naive_touch, lat);
                    const Run& q = find(st, sym, FillModel::queue_aware, lat);
                    f << st << "," << sym << "," << lat << "," << usd(n.r.final_mtm) << "," << usd(q.r.final_mtm) << ","
                      << usd(n.r.final_mtm - q.r.final_mtm) << "," << n.r.stats.filled_shares << ","
                      << q.r.stats.filled_shares << "\n";
                }
    }
    {
        std::ofstream f(out / "latency_curve.csv");
        f << "strategy,symbol,fill_model,latency_ns,pnl_usd,fill_rate,fills,filled_shares\n";
        for (const Run& w : runs)
            f << w.cfg.strategy << "," << w.cfg.symbol << "," << model_name(w.cfg.model) << "," << w.cfg.latency
              << "," << usd(w.r.final_mtm) << "," << fx(fill_rate(w.r.stats), 6) << "," << w.r.stats.fills << ","
              << w.r.stats.filled_shares << "\n";
    }

    // ---- report.md -----------------------------------------------------------------
    {
        std::ofstream md(out / "report.md");
        md << "# Step 4 market-making study, trial " << trial << "\n\n"
           << "Generated by `tools/mm_study` from `" << path.substr(path.find_last_of('/') + 1) << "` at commit `"
           << commit << "`. Rules M-Q in `PROGRESS.md`. Quotes 09:35:00-15:55:00 ET (observation from 09:30:00); "
              "latency L applies to order entry and to market data; maker rebate $0.0020/share, taker fee "
              "$0.0030/share; P&L marked at the real midpoint at 15:55:00, no flattening. Order size 100, "
              "inventory limit 500, gamma = 4.56e-5 per $. Every number is measured from the runs.\n\n";
        std::size_t invalid = 0;
        for (const Run& w : runs) invalid += w.valid ? 0 : 1;
        md << "Validity: " << runs.size() - invalid << " of " << runs.size()
           << " runs passed every check (0 look-ahead violations, 0 `ahead` underflows, cash and inventory rebuilt "
              "from the fills equal the simulator's).\n\n";
        auto sname = [](const std::string& s) { return s == "symmetric" ? "Symmetric" : "Avellaneda-Stoikov"; };
        auto mname = [](FillModel m) { return m == FillModel::queue_aware ? "queue-aware" : "naive"; };

        md << "## Headline (latency 0)\n\n"
           << "Fill rate = filled shares / shares of accepted orders. Markouts are share-weighted, cents per share: "
              "\"mid move\" = side x (mid 1 s later - mid just before the fill); \"vs fill\" = side x (mid 1 s later "
              "- fill price). Negative = the price moved against us.\n\n"
           << "| Strategy | Symbol | Model | P&L ($) | Fills | Fill rate | 1 s mid move (c) | 1 s vs fill (c) | Max "
              "abs inventory | P&L per share ($) |\n|---|---|---|---:|---:|---:|---:|---:|---:|---:|\n";
        for (const char* st : kStrategies)
            for (const char* sym : kSymbols)
                for (FillModel m : {FillModel::naive_touch, FillModel::queue_aware}) {
                    const Run& w = find(st, sym, m, 0);
                    md << "| " << sname(st) << " | " << sym << " | " << mname(m) << " | " << usd(w.r.final_mtm) << " | "
                       << num(w.r.stats.fills) << " | " << fx(100.0 * fill_rate(w.r.stats), 2) << "% | "
                       << optmd(mk_mid(w, kH1s), 3) << " | " << optmd(mk_px(w, kH1s), 3) << " | "
                       << max_abs_inv(w.r.stats) << " | " << fx(per_share(w), 5) << " |\n";
                }
        md << "\n## Invented profit = P&L(naive) - P&L(queue-aware), dollars\n\n| Strategy | Symbol |";
        for (std::uint64_t lat : kLatencies) md << " " << lat_name(lat) << " |";
        md << "\n|---|---|";
        for (std::size_t i = 0; i < std::size(kLatencies); ++i) md << "---:|";
        md << "\n";
        for (const char* st : kStrategies)
            for (const char* sym : kSymbols) {
                md << "| " << sname(st) << " | " << sym << " |";
                for (std::uint64_t lat : kLatencies)
                    md << " " << usd(find(st, sym, FillModel::naive_touch, lat).r.final_mtm -
                                     find(st, sym, FillModel::queue_aware, lat).r.final_mtm)
                       << " |";
                md << "\n";
            }
        md << "\n## Latency curve, queue-aware: P&L ($) / fill rate\n\n| Strategy | Symbol |";
        for (std::uint64_t lat : kLatencies) md << " " << lat_name(lat) << " |";
        md << "\n|---|---|";
        for (std::size_t i = 0; i < std::size(kLatencies); ++i) md << "---:|";
        md << "\n";
        for (const char* st : kStrategies)
            for (const char* sym : kSymbols) {
                md << "| " << sname(st) << " | " << sym << " |";
                for (std::uint64_t lat : kLatencies) {
                    const Run& w = find(st, sym, FillModel::queue_aware, lat);
                    md << " " << usd(w.r.final_mtm) << " / " << fx(100.0 * fill_rate(w.r.stats), 2) << "% |";
                }
                md << "\n";
            }
        md << "\n## Markout curves at latency 0 (mid move, cents per share)\n\n| Strategy | Symbol | Model |";
        for (std::uint64_t h : kHorizons) md << " " << (h == 0 ? std::string("0") : lat_name(h)) << " |";
        md << "\n|---|---|---|";
        for (std::size_t i = 0; i < kHorizons.size(); ++i) md << "---:|";
        md << "\n";
        for (const char* st : kStrategies)
            for (const char* sym : kSymbols)
                for (FillModel m : {FillModel::naive_touch, FillModel::queue_aware}) {
                    const Run& w = find(st, sym, m, 0);
                    md << "| " << sname(st) << " | " << sym << " | " << mname(m) << " |";
                    for (std::size_t h = 0; h < kHorizons.size(); ++h) md << " " << optmd(mk_mid(w, h), 3) << " |";
                    md << "\n";
                }
        md << "\n## Details (every run)\n\n| Strategy | Symbol | Model | Latency | Fills | Filled shares | Gross P&L "
              "($) | Rebates ($) | P&L ($) | End inv | Mean abs inv | Quotes improved / at best / behind | Fills "
              "by E / C / crossing add / P-Q | Half-spread captured (c) | Post-only rejects | Self-cross rejects | "
              "Queue anomalies |\n|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|---|---|---:|---:|---:|---:|\n";
        for (const Run& w : runs) {
            const SimStats& s = w.r.stats;
            md << "| " << sname(w.cfg.strategy) << " | " << w.cfg.symbol << " | " << mname(w.cfg.model) << " | "
               << lat_name(w.cfg.latency) << " | " << num(s.fills) << " | " << num(s.filled_shares) << " | "
               << usd(gross(w)) << " | " << usd(s.rebates) << " | " << usd(w.r.final_mtm) << " | "
               << inum(w.r.final_inventory) << " | " << fx(w.mean_abs_inv, 1) << " | " << num(w.q.improved) << " / "
               << num(w.q.at_best) << " / " << num(w.q.behind) << " | " << num(w.trig_e) << " / " << num(w.trig_c)
               << " / " << num(w.trig_add) << " / " << num(w.trig_pq) << " | " << optmd(w.half_spread_cents, 3)
               << " | " << num(s.rejected[static_cast<std::size_t>(Reject::post_only_would_cross)]) << " | "
               << num(s.rejected[static_cast<std::size_t>(Reject::self_cross)]) << " | "
               << num(s.anomaly_exec_with_ahead + s.anomaly_cross_add_with_ahead) << " |\n";
        }
    }

    // ---- trials log (Rule Q) ---------------------------------------------------------
    {
        std::ofstream f(trials, std::ios::app);
        const std::string date = iso_now();
        for (std::size_t i = 0; i < runs.size(); ++i) {
            const Run& w = runs[i];
            const bool as = w.cfg.strategy != "symmetric";
            std::ostringstream p;
            p << "{\"order_size\": " << kSize << ", \"max_inventory\": " << kMaxInv
              << ", \"post_only\": true, \"quote_start\": \"09:35:00\"";
            if (as)
                p << ", \"gamma_per_usd\": " << kGamma << ", \"T\": \"15:55:00\", \"sigma\": \"mean squared 1 s mid "
                     "change, last 1800 changes\", \"k\": \"1 / mean |exec price - mid| over E and printable C since "
                     "09:30\", \"min_sigma_changes\": 60, \"min_executions\": 20}";
            else
                p << ", \"pricing\": \"one tick inside when spread >= 3 ticks, else join\"}";
            f << "{\"trial\": " << trial << ", \"config_id\": \"" << trial << "-" << (i + 1) << "\", \"date\": \""
              << date << "\", \"step\": \"Step 4 market-making study (Rules M-Q)\", \"commit\": \"" << commit
              << "\", \"strategy\": \"" << (as ? "AvellanedaStoikov" : "SymmetricInsideQuoter") << "\", \"params\": "
              << p.str() << ", \"symbol\": \"" << w.cfg.symbol << "\", \"fill_model\": \""
              << (w.cfg.model == FillModel::queue_aware ? "queue_aware" : "naive_touch")
              << "\", \"order_latency_ns\": " << w.cfg.latency << ", \"market_data_latency_ns\": " << w.cfg.latency
              << ", \"fees\": {\"maker_rebate_per_share\": 0.002, \"taker_fee_per_share\": 0.003}, \"session\": "
                 "\"09:35:00-15:55:00 ET\", \"data\": \"data/01302019_AAPL_MSFT_INTC_COLB.tfc\", \"results_file\": "
                 "\"results/mm_study/runs.csv\", \"looked_at\": true, \"outcome\": {\"valid\": "
              << (w.valid ? "true" : "false") << ", \"fills\": " << w.r.stats.fills
              << ", \"filled_shares\": " << w.r.stats.filled_shares << ", \"pnl_usd\": " << usd(w.r.final_mtm)
              << ", \"fill_rate\": " << fx(fill_rate(w.r.stats), 6) << ", \"markout_1s_mid_cents\": "
              << (std::isnan(mk_mid(w, kH1s)) ? "null" : fx(mk_mid(w, kH1s), 4)) << ", \"max_abs_inventory\": "
              << max_abs_inv(w.r.stats) << "}}\n";
        }
    }
    std::cout << "wrote " << out.string() << " and appended " << runs.size() << " lines to " << trials << "\n";
    return 0;
}
