#include "executive.h"
#include "controller.h"
#include "phase3_config.h"

#include <filesystem>
#include <iostream>

int Executive::run(int argc, char* argv[]) {
    Phase3Config config;

    if (argc < 3 || argc > 8) {
        std::cerr
            << "Usage:\n"
            << "  Phase3.exe <input_directory> <dll_directory> [output_directory] [temp_directory] [reducer_count] [mapper_worker_exe] [reducer_worker_exe]\n";
        return 1;
    }

    const std::filesystem::path input_directory = argv[1];
    const std::filesystem::path dll_directory = argv[2];

    const std::filesystem::path output_directory =
        (argc >= 4) ? std::filesystem::path(argv[3]) : std::filesystem::path("output");

    const std::filesystem::path temp_directory =
        (argc >= 5) ? std::filesystem::path(argv[4]) : std::filesystem::path("temp");

    const int reducer_count =
        (argc >= 6) ? std::stoi(argv[5]) : config.default_reducer_count;

    const std::filesystem::path mapper_worker_exe =
        (argc >= 7) ? std::filesystem::path(argv[6]) : std::filesystem::path(config.mapper_worker_default_name);

    const std::filesystem::path reducer_worker_exe =
        (argc >= 8) ? std::filesystem::path(argv[7]) : std::filesystem::path(config.reducer_worker_default_name);

    if (reducer_count <= 0) {
        std::cerr << "Reducer count must be greater than 0." << std::endl;
        return 1;
    }

    Controller controller;
    const bool success = controller.run(
        input_directory,
        dll_directory,
        output_directory,
        temp_directory,
        reducer_count,
        mapper_worker_exe,
        reducer_worker_exe
    );

    return success ? 0 : 1;
}