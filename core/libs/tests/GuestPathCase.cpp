#include "prx/libc/include/General.hpp"
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
static void Require(bool value) { if (!value) std::abort(); }
static void Touch(const std::filesystem::path& path) { std::ofstream file(path); file << "x"; }
int main() {
    const auto host = std::filesystem::canonical(std::filesystem::current_path());
    const auto name = "anyps5-case-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto directory = host / name;
    std::filesystem::create_directories(directory / "Media" / "Movies");
    Touch(directory / "PackageCmdLineArgs.txt");
    Touch(directory / "Media" / "Movies" / "main_menu.bk2");
    Touch(directory / "Dup.txt");
    Touch(directory / "dup.txt");
    const auto guest = "/" + name + "/";
    Require(ResolvePath_nid_no_patch((guest + "PackageCmdLineArgs.txt").c_str()) == directory / "PackageCmdLineArgs.txt");
    Require(ResolvePath_nid_no_patch((guest + "packagecmdlineargs.txt").c_str()) == directory / "PackageCmdLineArgs.txt");
    Require(ResolvePath_nid_no_patch((guest + "MEDIA/movies/Main_Menu.bk2").c_str()) == directory / "Media" / "Movies" / "main_menu.bk2");
    Require(ResolvePath_nid_no_patch((guest + "media\\MOVIES\\MAIN_MENU.BK2").c_str()) == directory / "Media" / "Movies" / "main_menu.bk2");
    Require(ResolvePath_nid_no_patch((guest + "Dup.txt").c_str()) == directory / "Dup.txt");
    Require(ResolvePath_nid_no_patch((guest + "dup.txt").c_str()) == directory / "dup.txt");
    Require(ResolvePath_nid_no_patch((guest + "DUP.TXT").c_str()) == directory / "DUP.TXT");
    Require(ResolvePath_nid_no_patch((guest + "new.txt").c_str()) == directory / "new.txt");
    Require(ResolvePath_nid_no_patch((guest + "MEDIA/new/file.bin").c_str()) == directory / "Media" / "new" / "file.bin");
    std::filesystem::remove_all(directory);
}
