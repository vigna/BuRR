//  Copyright (c) Lorenz Hübschle-Schneider
//  All Rights Reserved.  This source code is licensed under the Apache 2.0
//  License (found in the LICENSE file in the root directory).

#include "ribbon.hpp"
#include "rocksdb/stop_watch.h"

#include <tlx/cmdline_parser.hpp>
#include <tlx/logger.hpp>

#include <atomic>
#include <cstdlib>
#include <numeric>
#include <thread>
#include <vector>

#define DO_EXPAND(VAL) VAL##1
#define EXPAND(VAL)    DO_EXPAND(VAL)

#if !defined(RIBBON_BITS) || (EXPAND(RIBBON_BITS) == 1)
#undef RIBBON_BITS
#define RIBBON_BITS 8
#endif

using namespace ribbon;

bool no_queries = false;

template <uint8_t depth, typename Config>
void run(size_t num_items, double eps, size_t seed, unsigned num_threads) {
    IMPORT_RIBBON_CONFIG(Config);

    const double slots_per_item = eps + 1.0;
    const size_t num_slots = num_items * slots_per_item;
    LOG1 << "Running simple test with " << num_slots << " slots, eps=" << eps
         << " -> " << num_items << " items, seed=" << seed
         << " config: L=" << kCoeffBits << " B=" << kBucketSize
         << " r=" << kResultBits;

    rocksdb::StopWatchNano timer(true);

    auto input = std::make_unique<int[]>(num_items);
    std::iota(input.get(), input.get() + num_items, 0);
    LOG1 << "Input generation took " << timer.ElapsedNanos(true) / 1e6 << "ms";

    rocksdb::StopWatchNano timer_constr(true);

    ribbon_filter<depth, Config> r(num_slots, slots_per_item, seed);

    LOG1 << "Allocation took " << timer.ElapsedNanos(true) / 1e6 << "ms\n";

    LOG1 << "Adding rows to filter (" << num_threads << " threads)....";
    r.AddRange(input.get(), input.get() + num_items, num_threads);
    uint64_t insertionTime = timer.ElapsedNanos(true);
    LOG1 << "Insertion took " << insertionTime / 1e6 << "ms in total\n";

    input.reset();

    r.BackSubst(num_threads);
    uint64_t backsubstTime = timer.ElapsedNanos(true);
    LOG1 << "Backsubstitution took " << backsubstTime / 1e6
         << "ms in total\n";

    const size_t bytes = r.Size();
    const double relsize = (bytes * 8 * 100.0) / (num_items * Config::kResultBits);
    LOG1 << "Ribbon size: " << bytes << " Bytes = " << (bytes * 1.0) / num_items
         << " Bytes per item = " << relsize << "%\n";

	uint64_t constr_time = timer_constr.ElapsedNanos(true);
	LOG1 << "Completed in " << constr_time / 1e6 << "ms, " << num_items << " keys, " << (double)constr_time / num_items << " ns/key\n";

	const uint64_t N = 10000000;
	bool found = false;

	rocksdb::StopWatchNano timer_query_indep(true);
	int key = 0;
    for (size_t v = 0; v < N; v++) {
		key += 0x9e3779b97f4a7c15;
       	found ^= r.QueryFilter((int)key);
    }
	uint64_t indep_time = timer_query_indep.ElapsedNanos(true);
	LOG1 << "Independent queries (" << found << "): " << indep_time / 1e6 << "ms, " << num_items << " keys, " << (double)indep_time / N << " ns/key\n";


	rocksdb::StopWatchNano timer_query_dep(true);	
	key = 0;
    for (size_t v = 0; v < N; v++) {
		key += 0x9e3779b97f4a7c15;
       	found = r.QueryFilter((int)key ^ (found * 0xdeadbeefdeadf00dULL * v)); 
    }
	uint64_t dep_time = timer_query_dep.ElapsedNanos(true);
	LOG1 << "Dependent queries (" << found << "): " << dep_time / 1e6 << "ms, " << num_items << " keys, "<< (double)dep_time / N << " ns/key\n";
}


template <ThreshMode mode, uint8_t depth, size_t L, size_t r, bool interleaved,
          bool cls, bool sparse, typename... Args>
void dispatch_shift(int shift, Args&... args) {
    switch (shift) {
        case 0:
            run<depth, RConfig<L, r, mode, sparse, interleaved, cls, 0>>(args...);
            break;
        case -1:
            run<depth, RConfig<L, r, mode, sparse, interleaved, cls, -1>>(args...);
            break;
        case 1:
            run<depth, RConfig<L, r, mode, sparse, interleaved, cls, 1>>(args...);
            break;
        default: LOG1 << "Unsupported bucket size shift: " << shift;
    }
}


template <ThreshMode mode, uint8_t depth, size_t L, size_t r, bool interleaved,
          bool cls, typename... Args>
void dispatch_sparse(bool sparse, Args&... args) {
    if (sparse) {
        if constexpr (interleaved) {
            LOG1 << "Sparse coefficients + interleaved sol doesn't make sense";
        } else {
            dispatch_shift<mode, depth, L, r, interleaved, cls, true>(args...);
        }
    } else {
        dispatch_shift<mode, depth, L, r, interleaved, cls, false>(args...);
    }
}

template <ThreshMode mode, uint8_t depth, size_t L, size_t r, typename... Args>
void dispatch_storage(bool cls, bool interleaved, Args&... args) {
    assert(!cls || !interleaved);
    if (cls) {
        // dispatch_sparse<mode, depth, L, r, false, true>(args...);
        LOG1 << "Cache-Line Storage is currently disabled";
    } else if (interleaved) {
        dispatch_sparse<mode, depth, L, r, true, false>(args...);
    } else {
        dispatch_sparse<mode, depth, L, r, false, false>(args...);
    }
}

template <ThreshMode mode, uint8_t depth, typename... Args>
void dispatch_width(size_t band_width, Args&... args) {
    static constexpr size_t r = RIBBON_BITS;
    switch (band_width) {
        // case 16: dispatch_storage<mode, depth, 16, r>(args...); break;
        case 32: dispatch_storage<mode, depth, 32, r>(args...); break;
        case 64: dispatch_storage<mode, depth, 64, r>(args...); break;
        case 128: dispatch_storage<mode, depth, 128, r>(args...); break;
        default: LOG1 << "Unsupported band width: " << band_width;
    }
}

template <ThreshMode mode, typename... Args>
void dispatch_depth(unsigned depth, Args&... args) {
    switch (depth) {
        case 0: dispatch_width<mode, 0>(args...); break;
        case 1: dispatch_width<mode, 1>(args...); break;
        case 2: dispatch_width<mode, 2>(args...); break;
        case 3: dispatch_width<mode, 3>(args...); break;
        case 4: dispatch_width<mode, 4>(args...); break;
        default: LOG1 << "Unsupported recursion depth: " << depth;
    }
}

template <typename... Args>
void dispatch(ThreshMode mode, Args&... args) {
    switch (mode) {
        case ThreshMode::onebit:
            dispatch_depth<ThreshMode::onebit>(args...);
            break;
        case ThreshMode::twobit:
            dispatch_depth<ThreshMode::twobit>(args...);
            break;
        case ThreshMode::normal:
            dispatch_depth<ThreshMode::normal>(args...);
            break;
        default:
            LOG1 << "Unsupported threshold compression mode: " << (int)mode;
    }
}

int main(int argc, char** argv) {
    tlx::CmdlineParser cmd;
    size_t seed = 42, num_items = 1024 * 1024;
    unsigned ribbon_width = 32, depth = 3;
    double eps = -1;
    unsigned num_threads = std::thread::hardware_concurrency();
    bool onebit = false, twobit = false, sparsecoeffs = false, cls = false,
         interleaved = false;
    int shift = 0;
    cmd.add_size_t('s', "seed", seed, "random seed");
    // Ideally, both settings would be allowed (but mutually exclusive), but it
    // doesn't seem to be // possible to check if an option was set with tlx::CmdlineParser
    //cmd.add_size_t('m', "slots", num_slots, "number of slots in the filter");
    cmd.add_size_t('n', "items", num_items, "number of items in the filter");
    cmd.add_unsigned('L', "ribbon_width", ribbon_width, "ribbon width (16/32/64)");
    cmd.add_unsigned('d', "depth", depth, "number of recursive filters");
    cmd.add_double('e', "epsilon", eps, "epsilon, #items = filtersize/(1+epsilon)");
    cmd.add_unsigned('t', "threads", num_threads, "number of query threads");
    cmd.add_int('b', "bsshift", shift,
                "whether to shift bucket size one way or another");
    cmd.add_bool('1', "onebit", onebit,
                 "use one-plus-a-little-bit threshold compression");
    cmd.add_bool('2', "twobit", twobit, "use two-bit threshold compression");
    cmd.add_bool('S', "sparsecoeffs", sparsecoeffs,
                 "use sparse coefficient vectors");
    cmd.add_bool('C', "cls", cls, "use cache-line solution storage");
    cmd.add_bool('I', "interleaved", interleaved,
                 "use interleaved solution storage");
    cmd.add_bool('Q', "noqueries", no_queries,
                 "don't run any queries (for scripting)");

    if (!cmd.process(argc, argv) || (onebit && twobit)) {
        cmd.print_usage();
        return 1;
    }
    if (eps == -1) {
        if (onebit) {
            size_t bucket_size = 1ul << tlx::integer_log2_floor(
                                     ribbon_width * ribbon_width /
                                     (4 * tlx::integer_log2_ceil(ribbon_width)));
            if (shift < 0)
                bucket_size >>= -shift;
            else
                bucket_size <<= shift;
            eps = -0.666 * ribbon_width / (4 * bucket_size + ribbon_width);
        } else {
            // for small ribbon widths, don't go too far from 0
            const double fct = ribbon_width <= 32 ? 3.0 : 4.0;
            eps = -fct / ribbon_width;
        }
    }
    if (seed == 0)
        seed = std::random_device{}();
    cmd.print_result();

    ThreshMode mode = onebit ? ThreshMode::onebit
                             : (twobit ? ThreshMode::twobit : ThreshMode::normal);

    dispatch(mode, depth, ribbon_width, cls, interleaved, sparsecoeffs, shift,
             num_items, eps, seed, num_threads);
}
