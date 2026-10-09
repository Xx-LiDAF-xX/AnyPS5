#include <LauncherCore.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void Write(const std::filesystem::path& path, const std::string& value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << value;
}

void WritePlainSelf(const std::filesystem::path& path) {
    std::string value(68, '\0');
    value[0] = static_cast<char>(0x54);
    value[1] = static_cast<char>(0x14);
    value[2] = static_cast<char>(0xf5);
    value[3] = static_cast<char>(0xee);
    value[24] = 1;
    value[32] = 4;
    value[33] = 0x28;
    value[64] = 0x7f;
    value[65] = 'E';
    value[66] = 'L';
    value[67] = 'F';
    Write(path, value);
}

}

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("anyps5-launcher-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        std::filesystem::create_directories(root / "Bravo");
        std::filesystem::create_directories(root / "Alpha");
        std::filesystem::create_directories(root / "Ignored");
#ifdef _WIN32
        std::ofstream(root / "Bravo" / "game.exe").put('\0');
        std::ofstream(root / "Alpha" / "game.exe").put('\0');
        const auto games = Launcher::DiscoverGames(root, true);
#else
        std::ofstream(root / "Bravo" / "game.elf").put('\0');
        std::ofstream(root / "Alpha" / "game.elf").put('\0');
        const auto games = Launcher::DiscoverGames(root, false);
#endif
        Require(games.size() == 2, "discovery returned the wrong game count");
        Require(games[0].Name == "Alpha", "discovery did not sort games");
        Require(games[1].Name == "Bravo", "discovery did not retain game names");
        const auto sourceRoot = root / "sources";
        Write(sourceRoot / "First" / "eboot.bin", std::string("\x7f" "ELF", 4));
        Write(sourceRoot / "Second" / "EBOOT.ELF", std::string("\x7f" "ELF", 4));
        const auto sources = Launcher::DiscoverSourceExecutables(sourceRoot);
        Require(sources.size() == 2, "source folder discovery returned the wrong game count");
        const auto direct = Launcher::DiscoverSourceExecutables(sourceRoot / "First");
        Require(direct.size() == 1, "single game directory was not discovered");
        WritePlainSelf(sourceRoot / "Self" / "eboot.bin");
        const auto sourcesWithSelf = Launcher::DiscoverSourceExecutables(sourceRoot);
        Require(sourcesWithSelf.size() == 3, "SELF executable was not discovered");
        const auto moduleSource = root / "ModuleSource";
        Write(moduleSource / "eboot.bin", std::string("\x7f" "ELF", 4));
        WritePlainSelf(moduleSource / "sce_module" / "module.prx");
        const auto sourceWithSelfModule = Launcher::DiscoverSourceExecutables(moduleSource);
        Require(sourceWithSelfModule.size() == 1, "game with SELF guest module was not discovered");
        const auto utf8 = Launcher::PathToUtf8(root / Launcher::PathFromUtf8("Library"));
        Require(!utf8.empty(), "path conversion returned an empty string");
        std::filesystem::remove_all(root);
        std::cout << "Launcher core tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
