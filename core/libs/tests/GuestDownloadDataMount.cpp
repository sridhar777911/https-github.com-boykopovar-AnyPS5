#include "prx/libkernel/AppMetadata/include/AppMetadata.hpp"
#include <cstdlib>
#include <cstring>
#include <filesystem>

static void Require(bool value) { if (!value) std::abort(); }

int main(int argc, char** argv) {
    Require(argc == 2);
    const bool declared = std::strcmp(argv[1], "declared") == 0;
    Require(std::filesystem::is_directory("download0") == declared);
    Require(std::strcmp(GetAppTitleId_nid_postfix().value, "PPSA00000") == 0);
    std::filesystem::remove_all("download0");
}
