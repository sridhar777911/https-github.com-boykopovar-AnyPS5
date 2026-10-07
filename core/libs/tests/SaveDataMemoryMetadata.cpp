#include "SceTypes.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

extern "C" {
int APS5_VABI sceSaveDataInitialize3(const void*);
int APS5_VABI sceSaveDataTerminate();
int APS5_VABI sceSaveDataSetSaveDataMemory2(const SaveDataMemorySet2*);
int APS5_VABI sceSaveDataDirNameSearch(const SaveDataDirNameSearchCond*, SaveDataDirNameSearchResult*);
}

static void Require(bool value) {
    if (!value) std::abort();
}

static std::vector<char> Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    Require(file.is_open());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

int main() {
    const auto previous = std::filesystem::current_path();
    const auto root = std::filesystem::temp_directory_path() /
        ("anyps5-metadata-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Require(std::filesystem::create_directory(root));
    std::filesystem::current_path(root);
    const auto expected_mtime = std::chrono::system_clock::time_point{std::chrono::seconds{1700000000}};
    const auto save_directory = std::filesystem::path("_sd/mtime");
    Require(std::filesystem::create_directories(save_directory));
    std::filesystem::last_write_time(save_directory,
            std::filesystem::file_time_type::clock::from_sys(expected_mtime));
    SceSaveDataDirName dir_name{};
    SaveDataParam searched_param{};
    SaveDataDirNameSearchCond search_cond{};
    SaveDataDirNameSearchResult search_result{};
    search_result.dir_names = &dir_name;
    search_result.dir_names_num = 1;
    search_result.params = &searched_param;
    Require(sceSaveDataDirNameSearch(&search_cond, &search_result) == 0);
    Require(search_result.hit_num == 1 && search_result.set_num == 1);
    Require(std::string(dir_name.data) == "mtime");
    Require(searched_param.mtime == std::chrono::duration_cast<std::chrono::seconds>(expected_mtime.time_since_epoch()).count());

    const auto path = std::filesystem::path("_sd_mem/u7531/slot0.param");
    std::filesystem::create_directories(path.parent_path());
    { std::ofstream file("_sd_mem/u7531/slot0.bin"); file << "save"; Require(static_cast<bool>(file)); }
    Require(sceSaveDataInitialize3(nullptr) == 0);
    Require(sceSaveDataInitialize3(nullptr) == 0);
    Require(sceSaveDataTerminate() == 0);
    SaveDataParam param{};
    SaveDataMemorySet2 set{};
    set.user_id = 7531;
    set.param = &param;
    Require(sceSaveDataSetSaveDataMemory2(&set) == 0);
    const auto original = Read(path);
    Require(original.size() == sizeof(param));
    param.user_param = 42;
    const auto temporary = path.string() + ".tmp";
    Require(std::filesystem::create_directory(temporary));
    const int status = sceSaveDataSetSaveDataMemory2(&set);
    Require(Read(path) == original);
    Require(status == static_cast<int>(0x809F000Bu));
    Require(!std::filesystem::exists(temporary));
    Require(sceSaveDataSetSaveDataMemory2(&set) == 0);
    const auto* bytes = reinterpret_cast<const char*>(&param);
    Require(Read(path) == std::vector<char>(bytes, bytes + sizeof(param)));
    set.param = nullptr;
    Require(sceSaveDataSetSaveDataMemory2(&set) == 0);
    Require(sceSaveDataTerminate() == 0);
    std::filesystem::current_path(previous);
    std::filesystem::remove_all(root);
}
