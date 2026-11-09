#include "utils/cpu_pinner.hpp"
#include "utils/csv_writer.hpp"
#include "utils/warmup.hpp"
#include <benchmark/benchmark.h>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    std::cout << "========================================\n";
    std::cout << "LOB Benchmark Suite\n";
    std::cout << "========================================\n";
    std::cout << "CPU: " << bench::CPUPinner::cpu_info() << "\n";

    int target_core = 0;
    std::string csv_path = "results/benchmark_result_1.3.0.csv";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.find("--core=") == 0) {
            target_core = std::stoi(arg.substr(7));
        } else if (arg.find("--csv=") == 0) {
            csv_path = arg.substr(6);
        }
    }

    if (bench::CPUPinner::pin(target_core)) {
        std::cout << "CPU Pinned: Core " << target_core << "\n";
    } else {
        std::cout << "CPU Pinning: Not available\n";
    }

    bench::csv() = std::make_unique<bench::CSVWriter>(csv_path);
    std::cout << "CSV Output: " << csv_path << "\n";
    std::cout << "========================================\n\n";

    std::cout << "Warmup...\n";
    bench::warmup();
    std::cout << "Done.\n\n";

    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();

    bench::csv().reset();
    std::cout << "\nResults: " << csv_path << "\n";

    return 0;
}
