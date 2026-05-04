// BuRR-VL (Variable-Length Ribbon) benchmark.
// Stores VLR-encoded values directly (no prefix-free coding layer).
// Interface matches the bench framework: --geometric / --zipf K / --uniform K,
// build with -Q -w, query with -r.

#include "ribbon.hpp"
#include "serialization.hpp"
#include "rocksdb/stop_watch.h"

#include <tlx/cmdline_parser.hpp>
#include <tlx/logger.hpp>

#include <algorithm>
#include <cstdlib>
#include <numeric>
#include <random>
#include <vector>

using namespace ribbon;

bool no_queries = false;

struct BuRRConfig
        : public RConfig<128, 1, ThreshMode::twobit, false, true, false, 0, uint64_t> {
    static constexpr bool kUseVLR = true;
    static constexpr Index kBucketSize = 128;
};

static inline uint64_t vlr_encode(uint64_t v) {
    if (v == 0) return 1;
    unsigned bw = 64 - __builtin_clzll(v);
    return v | (uint64_t(1) << bw);
}

template <uint8_t depth>
void run(size_t num_items, double eps, size_t seed,
         std::string ifile, std::string ofile,
         const uint64_t *values, unsigned max_bits) {
    IMPORT_RIBBON_CONFIG(BuRRConfig);

    const double slots_per_item = eps + 1.0;

    static_assert(kUseVLR);
    ribbon_filter<depth, BuRRConfig> r;
    rocksdb::StopWatchNano timer(true);

    if (ifile.length() == 0) {
        auto input = std::make_unique<std::pair<Key, ResultRowVLR>[]>(num_items);
        for (size_t i = 0; i < num_items; ++i) {
            input.get()[i].first = static_cast<Key>(i);
            input.get()[i].second = static_cast<ResultRowVLR>(vlr_encode(values[i]));
        }
        LOG1 << "Input generation took " << timer.ElapsedNanos(true) / 1e6 << "ms";
        const Index num_ribbons = max_bits + 1;
        rocksdb::StopWatchNano timer_constr(true);
        r = ribbon_filter<depth, BuRRConfig>(slots_per_item, seed, num_ribbons);
        LOG1 << "Allocation took " << timer.ElapsedNanos(true) / 1e6 << "ms\n";
        LOG1 << "Adding rows to filter....";
        r.AddRange(input.get(), input.get() + num_items);
        LOG1 << "Insertion took " << timer.ElapsedNanos(true) / 1e6 << "ms in total\n";

        input.reset();

        r.BackSubst();
        LOG1 << "Backsubstitution took " << timer.ElapsedNanos(true) / 1e6
             << "ms in total\n";
        uint64_t constr_time = timer_constr.ElapsedNanos(true);
        const size_t bytes = r.Size();
        LOG1 << "Ribbon size: " << bytes << " Bytes = " << (bytes * 1.0) / num_items
             << " Bytes per item\n";
        LOG1 << "Completed in " << constr_time / 1e6 << "ms, "
             << num_items << " keys, " << (double)constr_time / num_items << " ns/key";
    } else {
        r.Deserialize(ifile);
        LOG1 << "Deserialization took " << timer.ElapsedNanos(true) / 1e6 << "ms\n";
        const size_t bytes = r.Size();
        LOG1 << "Ribbon size: " << bytes << " Bytes = " << (bytes * 1.0) / num_items
             << " Bytes per item\n";
    }
    // Correctness check (always run before serialization).
    // The VLR stores the data bits of the value but NOT the sentinel.
    // With kVLRFlipOutputBits=true (default), data bits are at the LSB.
    // For value v, bit_width(v) data bits are stored starting from the
    // first ribbon.
    if (values != nullptr) {
        unsigned errors = 0;
        size_t check = std::min(num_items, size_t{10000});
        for (size_t i = 0; i < check; ++i) {
            uint64_t got = r.QueryRetrieval(static_cast<Key>(i));
            uint64_t v = values[i];
            if (v == 0) continue;
            unsigned bw = 64 - __builtin_clzll(v);
            uint64_t mask = (uint64_t(1) << bw) - 1;
            if ((got & mask) != v) {
                if (errors < 10)
                    LOG1 << "MISMATCH key=" << i << " value=" << v
                         << " expected=0x" << std::hex << v
                         << " got=0x" << (got & mask) << std::dec;
                errors++;
            }
        }
        if (errors > 0) {
            LOG1 << "CORRECTNESS FAILED: " << errors << " / " << check << " mismatches";
            return;
        }
    }

    if (ofile.length() != 0) {
        r.Serialize(ofile);
        LOG1 << "Serialization took " << timer.ElapsedNanos(true) / 1e6 << "ms\n";
    }

    if (no_queries)
        return;

    const uint64_t N = 100000000;
    const int REPEATS = 5;
    std::vector<double> timings;
    for (int k = 0; k < REPEATS; k++) {
        rocksdb::StopWatchNano timer_query(true);
        uint64_t key = 0;
        uint64_t acc = 0;
        for (size_t v = 0; v < N; v++) {
            key += 0x9e3779b97f4a7c15;
            acc ^= r.QueryRetrieval(key);
        }
        const volatile uint64_t _sink = acc;
        double timing = (double)timer_query.ElapsedNanos(true) / N;
        LOG1 << timing << " ns/key";
        timings.push_back(timing);
    }

    sort(timings.begin(), timings.end());
    LOG1 << "Min: " << timings[0] << " Median: " << timings[timings.size() / 2]
         << " Max: " << timings[timings.size() - 1] << " Average: "
         << reduce(timings.begin(), timings.end(), 0.0) / timings.size();
}

// Value generators

void generate_geometric(size_t n, uint64_t *values, unsigned &max_bits) {
    std::mt19937_64 rng(42);
    max_bits = 0;
    for (size_t i = 0; i < n; ++i) {
        uint64_t x = rng();
        uint64_t v = (x == 0) ? 63 : __builtin_ctzll(x);
        values[i] = v;
        unsigned bw = (v == 0) ? 0 : (64 - __builtin_clzll(v));
        if (bw > max_bits) max_bits = bw;
    }
}

void generate_zipf(size_t n, uint64_t *values, size_t K, unsigned &max_bits) {
    std::mt19937_64 rng(42);
    std::uniform_real_distribution<double> unif(0.0, 1.0);

    double H = 0.0;
    for (size_t j = 1; j <= K; ++j) H += 1.0 / j;

    std::vector<double> cdf(K);
    double cumsum = 0.0;
    for (size_t j = 0; j < K; ++j) {
        cumsum += (1.0 / (j + 1)) / H;
        cdf[j] = cumsum;
    }

    max_bits = 0;
    for (size_t i = 0; i < n; ++i) {
        double u = unif(rng);
        auto it = std::lower_bound(cdf.begin(), cdf.end(), u);
        uint64_t v = static_cast<uint64_t>(it - cdf.begin());
        values[i] = v;
        unsigned bw = (v == 0) ? 0 : (64 - __builtin_clzll(v));
        if (bw > max_bits) max_bits = bw;
    }
}

void generate_uniform(size_t n, uint64_t *values, size_t K, unsigned &max_bits) {
    std::mt19937_64 rng(42);
    max_bits = 0;
    for (size_t i = 0; i < n; ++i) {
        uint64_t v = rng() % K;
        values[i] = v;
        unsigned bw = (v == 0) ? 0 : (64 - __builtin_clzll(v));
        if (bw > max_bits) max_bits = bw;
    }
}

int main(int argc, char** argv) {
    tlx::CmdlineParser cmd;
    size_t seed = 42, num_items = 1024 * 1024;
    unsigned depth = 2;
    double eps = -0.03125;
    bool geometric = false;
    size_t zipf_k = 0, uniform_k = 0;
    std::string ifile, ofile;

    // Accepted for compatibility with the bench framework's command lines.
    unsigned ignored_u = 0;
    int ignored_i = 0;
    bool ignored_b = false;
    cmd.add_size_t('s', "seed", seed, "random seed");
    cmd.add_size_t('n', "items", num_items, "number of items");
    cmd.add_unsigned('L', "ribbon_width", ignored_u, "(ignored, always 128)");
    cmd.add_unsigned('d', "depth", depth, "recursion depth");
    cmd.add_double('e', "epsilon", eps, "epsilon");
    cmd.add_unsigned('t', "threads", ignored_u, "(ignored)");
    cmd.add_int('b', "bsshift", ignored_i, "(ignored)");
    cmd.add_bool('1', "onebit", ignored_b, "(ignored)");
    cmd.add_bool('2', "twobit", ignored_b, "(ignored)");
    cmd.add_bool('S', "sparsecoeffs", ignored_b, "(ignored)");
    cmd.add_bool('C', "cls", ignored_b, "(ignored)");
    cmd.add_bool('I', "interleaved", ignored_b, "(ignored)");
    cmd.add_bool('Q', "noqueries", no_queries, "skip queries");
    cmd.add_string('r', "read", ifile, "deserialize from file");
    cmd.add_string('w', "write", ofile, "serialize to file");
    cmd.add_bool('g', "geometric", geometric, "geometric distribution");
    cmd.add_size_t('z', "zipf", zipf_k, "Zipf(1) over [0..K)");
    cmd.add_size_t('u', "uniform", uniform_k, "uniform over [0..K)");

    if (!cmd.process(argc, argv)) {
        return 1;
    }

    int dist_count = (geometric ? 1 : 0) + (zipf_k > 0 ? 1 : 0) + (uniform_k > 0 ? 1 : 0);
    if (ifile.length() == 0 && dist_count != 1) {
        std::cerr << "Exactly one of --geometric, --zipf K, --uniform K must be specified.\n";
        return 1;
    }

    if (seed == 0)
        seed = std::random_device{}();
    cmd.print_result();

    auto values = std::make_unique<uint64_t[]>(num_items);
    unsigned max_bits = 0;

    if (geometric) {
        generate_geometric(num_items, values.get(), max_bits);
        LOG1 << "Distribution: geometric (trailing zeros), max " << max_bits << " bits";
    } else if (zipf_k > 0) {
        generate_zipf(num_items, values.get(), zipf_k, max_bits);
        LOG1 << "Distribution: Zipf(1) over [0.." << zipf_k << "), max " << max_bits << " bits";
    } else if (uniform_k > 0) {
        generate_uniform(num_items, values.get(), uniform_k, max_bits);
        LOG1 << "Distribution: uniform over [0.." << uniform_k << "), max " << max_bits << " bits";
    }

    const uint64_t *vals = values.get();

    if (depth == 2)
        run<2>(num_items, eps, seed, ifile, ofile, vals, max_bits);
    else if (depth == 3)
        run<3>(num_items, eps, seed, ifile, ofile, vals, max_bits);
    else {
        LOG1 << "Unsupported depth: " << depth;
        return 1;
    }
}
