#pragma once

#include "stats.hpp"

#include <fstream>
#include <iomanip>
#include <memory>
#include <set>
#include <string>

namespace bench {

class CSVWriter {
public:
    explicit CSVWriter(const std::string& filename) : file_(filename) {
        if (file_.is_open()) {
            file_ << "Benchmark,Samples,Mean_ns,P50_ns,P95_ns,P99_ns,P99.9_ns,P99.99_ns,"
                     "Min_ns,Max_ns,StdDev_ns,Throughput_ops_per_sec\n";
        }
    }

    void write(const std::string& name, const Stats& s) {
        if (file_.is_open() && !written_.count(name)) {
            written_.insert(name);
            file_ << name << "," << s.count << "," << std::fixed << std::setprecision(2)
                  << s.mean << "," << s.p50 << "," << s.p95 << "," << s.p99 << "," << s.p999 << ","
                  << s.p9999 << "," << s.min_val << "," << s.max_val << "," << s.stddev
                  << "," << std::setprecision(0) << s.throughput << "\n";
        }
    }

private:
    std::ofstream file_;
    std::set<std::string> written_;
};

inline std::unique_ptr<CSVWriter>& csv() {
    static std::unique_ptr<CSVWriter> instance;
    return instance;
}

} 
