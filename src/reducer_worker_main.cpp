#include "file_manager.h"
#include "logger.h"
#include "phase3_config.h"
#include "reducer_dll.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

#ifdef _WIN32
#define LIB_HANDLE HMODULE
#define LOAD_LIB(path) LoadLibraryA(path)
#define GET_SYM(lib, name) GetProcAddress(lib, name)
#define CLOSE_LIB(lib) FreeLibrary(lib)
#define LIB_EXT ".dll"
#else
#define LIB_HANDLE void*
#define LOAD_LIB(path) nullptr
#define GET_SYM(lib, name) nullptr
#define CLOSE_LIB(lib)
#define LIB_EXT ""
#endif

int main(int argc, char* argv[]) {
    if (argc != 6) {
        std::cerr << "Usage: reducer_worker.exe <dll_directory> <temp_directory> <output_directory> <reducer_id> <mapper_count>" << std::endl;
        return 1;
    }

    const fs::path dll_directory = argv[1];
    const fs::path temp_directory = argv[2];
    const fs::path output_directory = argv[3];
    const int reducer_id = std::stoi(argv[4]);
    const int mapper_count = std::stoi(argv[5]);

    Phase3Config config;
    FileManager file_manager;

    file_manager.ensure_directory(output_directory);
    file_manager.ensure_directory(config.logs_directory(output_directory));
    Logger::instance().initialize(config.reducer_log_path(output_directory, reducer_id), false);
    Logger::instance().info("Reducer worker starting: " + std::to_string(reducer_id));

#ifdef _WIN32
    std::map<std::string, std::vector<int>> grouped_values;

    // Phase 3 requirement: sorting/grouping happens inside the reducer process.
    for (int mapper_id = 0; mapper_id < mapper_count; ++mapper_id) {
        const fs::path partition_path =
            config.mapper_partition_path(temp_directory, mapper_id, reducer_id);

        if (!fs::exists(partition_path)) {
            continue;
        }

        std::ifstream input_file(partition_path);
        std::string line;

        while (std::getline(input_file, line)) {
            const std::size_t tab_position = line.find('\t');
            if (tab_position == std::string::npos) {
                continue;
            }

            const std::string key = line.substr(0, tab_position);
            const int value = std::stoi(line.substr(tab_position + 1));
            grouped_values[key].push_back(value);
        }
    }

    const fs::path reducer_dll_path = dll_directory / ("reducer_dll" LIB_EXT);
    LIB_HANDLE reducer_module = LOAD_LIB(reducer_dll_path.string().c_str());

    if (!reducer_module) {
        Logger::instance().error("Failed to load reducer DLL: " + reducer_dll_path.string());
        return 1;
    }

    auto create_reducer = reinterpret_cast<decltype(&CreateReducer)>(GET_SYM(reducer_module, "CreateReducer"));
    auto destroy_reducer = reinterpret_cast<decltype(&DestroyReducer)>(GET_SYM(reducer_module, "DestroyReducer"));
    auto reducer_reduce = reinterpret_cast<decltype(&ReducerReduce)>(GET_SYM(reducer_module, "ReducerReduce"));
    auto reducer_finish = reinterpret_cast<decltype(&ReducerFinish)>(GET_SYM(reducer_module, "ReducerFinish"));

    if (!create_reducer || !destroy_reducer || !reducer_reduce || !reducer_finish) {
        Logger::instance().error("Reducer DLL is missing required exports.");
        CLOSE_LIB(reducer_module);
        return 1;
    }

    const fs::path reducer_output_path = config.reducer_output_path(output_directory, reducer_id);
    const std::string reducer_output_file_name = reducer_output_path.filename().string();

    file_manager.reset_final_output_file(output_directory, reducer_output_file_name);

    void* reducer = create_reducer(
        static_cast<void*>(&file_manager),
        output_directory.string().c_str(),
        reducer_output_file_name.c_str(),
        500
    );

    if (!reducer) {
        Logger::instance().error("Failed to create reducer instance.");
        CLOSE_LIB(reducer_module);
        return 1;
    }

    for (const auto& entry : grouped_values) {
        const std::string& key = entry.first;
        const std::vector<int>& values = entry.second;

        if (!reducer_reduce(reducer, key.c_str(), values.data(), values.size())) {
            Logger::instance().error("Reducer failed for key: " + key);
            destroy_reducer(reducer);
            CLOSE_LIB(reducer_module);
            return 1;
        }
    }

    if (!reducer_finish(reducer)) {
        Logger::instance().error("Reducer finish failed.");
        destroy_reducer(reducer);
        CLOSE_LIB(reducer_module);
        return 1;
    }

    destroy_reducer(reducer);
    CLOSE_LIB(reducer_module);
    Logger::instance().info("Reducer worker completed successfully.");
    return 0;
#else
    Logger::instance().fatal("Reducer worker currently supports Windows DLL loading only.");
    return 1;
#endif
}