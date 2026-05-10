/** @file
*****************************************************************************

Benchmark utilities: timing, CSV output, memory measurement, statistics.

*****************************************************************************
* @author     Heewon Chung
* @copyright  MIT license (see LICENSE file)
*****************************************************************************/

#ifndef BENCH_UTILS_HPP_
#define BENCH_UTILS_HPP_

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <numeric>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <mach/mach.h>
#elif defined(__linux__)
#include <fstream>
#endif

namespace bench {

/* ======================================================================== */
/* Timer                                                                     */
/* ======================================================================== */

using hrclock = std::chrono::high_resolution_clock;

class BenchTimer {
public:
    void start() { t0_ = hrclock::now(); }
    void stop()  { t1_ = hrclock::now(); }

    double elapsed_ms() const {
        return std::chrono::duration<double, std::milli>(t1_ - t0_).count();
    }
    double elapsed_us() const {
        return std::chrono::duration<double, std::micro>(t1_ - t0_).count();
    }
private:
    hrclock::time_point t0_, t1_;
};


/* ======================================================================== */
/* Statistics                                                                */
/* ======================================================================== */

struct BenchStats {
    double median;
    double mean;
    double stddev;
    double min_val;
    double max_val;
    size_t n;

    BenchStats() : median(0), mean(0), stddev(0), min_val(0), max_val(0), n(0) {}
};

inline BenchStats compute_stats(std::vector<double> samples)
{
    BenchStats s;
    s.n = samples.size();
    if (s.n == 0) return s;

    std::sort(samples.begin(), samples.end());
    s.min_val = samples.front();
    s.max_val = samples.back();

    if (s.n % 2 == 1) {
        s.median = samples[s.n / 2];
    } else {
        s.median = (samples[s.n / 2 - 1] + samples[s.n / 2]) / 2.0;
    }

    s.mean = std::accumulate(samples.begin(), samples.end(), 0.0) / s.n;

    if (s.n > 1) {
        double sq_sum = 0;
        for (double v : samples)
            sq_sum += (v - s.mean) * (v - s.mean);
        s.stddev = std::sqrt(sq_sum / (s.n - 1));
    }

    return s;
}

/**
 * Run a benchmark function with warmup and repetitions.
 * Returns statistics over the repetition timings (in ms).
 */
inline BenchStats run_benchmark(std::function<void()> func,
                                size_t warmup = 1, size_t reps = 5)
{
    for (size_t i = 0; i < warmup; ++i)
        func();

    std::vector<double> timings;
    BenchTimer timer;
    for (size_t i = 0; i < reps; ++i)
    {
        timer.start();
        func();
        timer.stop();
        timings.push_back(timer.elapsed_ms());
    }
    return compute_stats(timings);
}


/* ======================================================================== */
/* Memory                                                                    */
/* ======================================================================== */

inline size_t get_peak_memory_bytes()
{
#ifdef __APPLE__
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) == KERN_SUCCESS)
    {
        return info.resident_size_max;
    }
    return 0;
#elif defined(__linux__)
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.substr(0, 6) == "VmPeak") {
            size_t val = 0;
            for (char c : line)
                if (c >= '0' && c <= '9')
                    val = val * 10 + (c - '0');
            return val * 1024; /* VmPeak is in kB */
        }
    }
    return 0;
#else
    return 0;
#endif
}


/* ======================================================================== */
/* CSV Writer                                                                */
/* ======================================================================== */

class CSVWriter {
public:
    CSVWriter(const std::string &filepath) : filepath_(filepath), first_row_(true) {}

    void write_header(const std::vector<std::string> &columns)
    {
        ofs_.open(filepath_);
        for (size_t i = 0; i < columns.size(); ++i) {
            if (i > 0) ofs_ << ",";
            ofs_ << columns[i];
        }
        ofs_ << "\n";
        first_row_ = false;
    }

    void write_row(const std::vector<std::string> &values)
    {
        for (size_t i = 0; i < values.size(); ++i) {
            if (i > 0) ofs_ << ",";
            ofs_ << values[i];
        }
        ofs_ << "\n";
        ofs_.flush();
    }

    void close() { ofs_.close(); }

private:
    std::string filepath_;
    std::ofstream ofs_;
    bool first_row_;
};

inline std::string to_str(double v, int prec = 2)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", prec, v);
    return std::string(buf);
}

inline std::string to_str(size_t v)
{
    return std::to_string(v);
}

} // bench

#endif // BENCH_UTILS_HPP_
