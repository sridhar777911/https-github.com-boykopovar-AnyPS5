#include "prx/libSceAgcDriver/Execution/include/WriteWatchCoverage.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        using AgcDriver::GuestMemory::WriteWatchCoverage;
        constexpr std::uint64_t base = 0x200000000;
        constexpr std::size_t block = 65536;
        WriteWatchCoverage coverage;
        require(!coverage.Covers(base, 1), "an uninitialized arena is watched");
        coverage.Initialize(base, 4 * block + 1);
        require(coverage.Covers(base, 4 * block + 1), "the initialized arena is not watched");
        require(!coverage.Covers(base - 1, 1) && !coverage.Covers(base + 4 * block + 1, 1), "outside memory is watched");
        require(!coverage.Covers(base, 0) && !coverage.Covers(base, std::numeric_limits<std::size_t>::max()), "an empty or overflowing range is watched");
        require(coverage.Exclude(base + block + 4096, 1), "an imported page was not excluded");
        require(!coverage.Covers(base + block, block), "the imported block is still watched");
        require(!coverage.Covers(base + block - 1, 2), "a range crossing into the import is watched");
        require(coverage.Covers(base, block) && coverage.Covers(base + 2 * block, 2 * block + 1), "unrelated blocks lost write watching");
        require(!coverage.Exclude(base + block, block), "excluding an imported block again reports a change");
        require(coverage.Exclude(base + 3 * block - 1, 2), "an import across a block boundary was not excluded");
        require(!coverage.Covers(base + 2 * block, block) && !coverage.Covers(base + 3 * block, block), "a boundary import left one block watched");
        require(coverage.Covers(base, block) && coverage.Covers(base + 4 * block, 1), "a boundary import excluded adjacent blocks");
        require(!coverage.Exclude(base - block, block) && !coverage.Exclude(base + 5 * block, block), "an outside import changed coverage");
        require(coverage.Exclude(base + 4 * block, std::numeric_limits<std::size_t>::max()), "the last partial block was not excluded");
        require(!coverage.Covers(base + 4 * block, 1), "the last partial block is still watched");
        coverage.Initialize(base, 2 * block);
        require(coverage.Exclude(base - 1, 2) && !coverage.Covers(base, block), "an import crossing the arena start was not clipped");
        require(coverage.Covers(base + block, block), "clipping an import excluded the rest of the arena");
        std::cout << "write watch coverage passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
