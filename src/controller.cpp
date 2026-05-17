#include "controller.h"

#include "file_manager.h"
#include "logger.h"
#include "phase3_config.h"

#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

#ifdef _WIN32
static std::wstring widen(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

static std::wstring quoted(const std::string& s) {
    return L"\"" + widen(s) + L"\"";
}

struct ChildProcess {
    PROCESS_INFORMATION process_info{};
};

static bool launch_process(const fs::path& exe_path, const std::vector<std::string>& arguments, ChildProcess& child) {
    std::wstring command_line = quoted(exe_path.string());

    for (const auto& arg : arguments) {
        command_line += L" " + quoted(arg);
    }

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);

    std::wstring mutable_command = command_line;

    const BOOL ok = CreateProcessW(
        widen(exe_path.string()).c_str(),
        mutable_command.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        nullptr,
        &startup_info,
        &child.process_info
    );

    return ok == TRUE;
}

static bool wait_for_all(std::vector<ChildProcess>& children) {
    for (auto& child : children) {
        WaitForSingleObject(child.process_info.hProcess, INFINITE);

        DWORD exit_code = 1;
        GetExitCodeProcess(child.process_info.hProcess, &exit_code);

        CloseHandle(child.process_info.hThread);
        CloseHandle(child.process_info.hProcess);

        if (exit_code != 0) {
            return false;
        }
    }

    return true;
}
#endif

bool Controller::run(
    const fs::path& input_directory,
    const fs::path& dll_directory,
    const fs::path& output_directory,
    const fs::path& temp_directory,
    int reducer_count,
    const fs::path& mapper_worker_exe,
    const fs::path& reducer_worker_exe) {

    FileManager file_manager;
    Phase3Config config;

    if (!file_manager.directory_exists(input_directory)) {
        std::cerr << "Input directory does not exist: " << input_directory << std::endl;
        return false;
    }

    if (!file_manager.directory_exists(dll_directory)) {
        std::cerr << "DLL directory does not exist: " << dll_directory << std::endl;
        return false;
    }

    if (!file_manager.ensure_directory(output_directory) ||
        !file_manager.ensure_directory(temp_directory)) {
        std::cerr << "Could not create/access output or temp directory." << std::endl;
        return false;
    }

    file_manager.clear_directory_contents(output_directory);
    file_manager.clear_directory_contents(temp_directory);
    file_manager.ensure_directory(output_directory);
    file_manager.ensure_directory(temp_directory);
    file_manager.ensure_directory(config.logs_directory(output_directory));

    Logger::instance().initialize(config.controller_log_path(output_directory), false);
    Logger::instance().info("Phase 3 controller starting.");

    const std::vector<fs::path> input_files = file_manager.get_input_files(input_directory);
    if (input_files.empty()) {
        Logger::instance().error("No input files found.");
        return false;
    }

    // File-based split: one mapper process per input file.
    const int mapper_count = static_cast<int>(input_files.size());

    Logger::instance().info("Mapper process count: " + std::to_string(mapper_count));
    Logger::instance().info("Reducer process count: " + std::to_string(reducer_count));

#ifdef _WIN32
    std::vector<ChildProcess> mapper_children;
    mapper_children.reserve(input_files.size());

    for (int mapper_id = 0; mapper_id < mapper_count; ++mapper_id) {
        ChildProcess child{};
        const auto& input_file = input_files[static_cast<std::size_t>(mapper_id)];

        const bool launched = launch_process(
            mapper_worker_exe,
            {
                dll_directory.string(),
                input_file.string(),
                temp_directory.string(),
                output_directory.string(),
                std::to_string(mapper_id),
                std::to_string(reducer_count)
            },
            child
        );

        if (!launched) {
            Logger::instance().error("Failed to start mapper worker for file: " + input_file.string());
            return false;
        }

        mapper_children.push_back(child);
    }

    if (!wait_for_all(mapper_children)) {
        Logger::instance().error("One or more mapper workers failed.");
        return false;
    }

    Logger::instance().info("All mapper workers completed successfully.");

    std::vector<ChildProcess> reducer_children;
    reducer_children.reserve(static_cast<std::size_t>(reducer_count));

    for (int reducer_id = 0; reducer_id < reducer_count; ++reducer_id) {
        ChildProcess child{};

        const bool launched = launch_process(
            reducer_worker_exe,
            {
                dll_directory.string(),
                temp_directory.string(),
                output_directory.string(),
                std::to_string(reducer_id),
                std::to_string(mapper_count)
            },
            child
        );

        if (!launched) {
            Logger::instance().error("Failed to start reducer worker: " + std::to_string(reducer_id));
            return false;
        }

        reducer_children.push_back(child);
    }

    if (!wait_for_all(reducer_children)) {
        Logger::instance().error("One or more reducer workers failed.");
        return false;
    }

    Logger::instance().info("All reducer workers completed successfully.");
#else
    Logger::instance().fatal("This Phase 3 starter currently supports Windows process launching only.");
    return false;
#endif

    // Final merge step.
    std::map<std::string, int> final_counts;

    for (int reducer_id = 0; reducer_id < reducer_count; ++reducer_id) {
        const fs::path reducer_output = config.reducer_output_path(output_directory, reducer_id);

        if (!fs::exists(reducer_output)) {
            continue;
        }

        std::ifstream input_file(reducer_output);
        std::string line;

        while (std::getline(input_file, line)) {
            const std::size_t tab_position = line.find('\t');
            if (tab_position == std::string::npos) {
                continue;
            }

            const std::string key = line.substr(0, tab_position);
            const int value = std::stoi(line.substr(tab_position + 1));
            final_counts[key] += value;
        }
    }

    std::vector<std::pair<std::string, int>> merged_records;
    merged_records.reserve(final_counts.size());

    for (const auto& entry : final_counts) {
        merged_records.push_back(entry);
    }

    if (!file_manager.reset_final_output_file(output_directory, config.final_output_file_name)) {
        Logger::instance().error("Failed to reset final output file.");
        return false;
    }

    if (!file_manager.append_final_results(output_directory, config.final_output_file_name, merged_records)) {
        Logger::instance().error("Failed to write final merged output.");
        return false;
    }

    if (!file_manager.create_success_file(output_directory)) {
        Logger::instance().error("Failed to create SUCCESS file.");
        return false;
    }

    Logger::instance().info("Phase 3 controller completed successfully.");
    return true;
}