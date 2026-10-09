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

}

int main(int argc, char* argv[]) {
    if (argc > 1) {
        const auto input = Launcher::PathFromUtf8(argv[argc - 2]);
        if (input.filename() == "failure.bin") {
            std::cerr << "FAIL: " << Launcher::PathToUtf8(input) << ": synthetic relinker failure\n";
            return 2;
        }
        const auto output = Launcher::PathFromUtf8(argv[argc - 1]);
        Write(output, "converted");
        for (int index = 1; index < argc - 2; ++index) {
            if (std::string(argv[index]) == "--registry") Write(output.parent_path() / (output.stem().string() + ".registry.json"), "[]");
        }
        return 0;
    }
    const auto root = std::filesystem::temp_directory_path() /
        ("anyps5-launcher-install-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        const auto source = root / "Source Game";
        const auto libraries = root / "runtime";
        const auto library = root / "installed";
        Write(source / "eboot.bin", std::string("\x7f" "ELF", 4));
        Write(source / "data" / "asset.bin", "asset");
        Write(source / "sce_module" / "original.prx", std::string("\x7f" "ELF", 4));
        Write(libraries / "libc.prx", "library");
        Write(libraries / "libstdc++-6.dll", "runtime");
#ifdef _WIN32
        constexpr bool windowsTarget = true;
#else
        constexpr bool windowsTarget = false;
#endif
        const auto game = Launcher::InstallGame({source / "eboot.bin", library,
            Launcher::PathFromUtf8(argv[0]), libraries, false, windowsTarget});
        Require(game.Name == "Source_Game", "installation name was not sanitized");
        Require(std::filesystem::is_regular_file(game.Executable), "converted executable is missing");
        Require(std::filesystem::is_regular_file(game.Executable.parent_path() / "game.registry.json"),
                "compatibility registry is missing");
        Require(std::filesystem::is_regular_file(game.Executable.parent_path() / "app0" / "data" / "asset.bin"),
                "game resource was not copied");
        Require(!std::filesystem::exists(game.Executable.parent_path() / "app0" / "eboot.bin"),
                "source executable was copied into app0");
        Require(!std::filesystem::exists(game.Executable.parent_path() / "app0" / "sce_module" / "original.prx"),
                "original module was copied into app0");
        Require(std::filesystem::is_regular_file(game.Executable.parent_path() / "libs" / "libc.prx"),
                "runtime library was not copied");
        Require(std::filesystem::is_regular_file(game.Executable.parent_path() / "libs" / "libstdc++-6.dll"),
                "runtime DLL was not copied");
        const auto games = Launcher::DiscoverGames(library, windowsTarget);
        Require(games.size() == 1 && games[0].Name == "Source_Game", "installed game was not discovered");
        Write(source / "invalid.bin", "not an elf");
        bool rejectedInvalidBin = false;
        try {
            Launcher::InstallGame({source / "invalid.bin", root / "invalid-library",
                Launcher::PathFromUtf8(argv[0]), libraries, false, windowsTarget});
        } catch (const std::runtime_error&) {
            rejectedInvalidBin = true;
        }
        Require(rejectedInvalidBin, "non-ELF bin file was accepted");
        Write(source / "failure.bin", std::string("\x7f" "ELF", 4));
        std::string relinkerFailure;
        try {
            Launcher::InstallGame({source / "failure.bin", root / "failure-library",
                Launcher::PathFromUtf8(argv[0]), libraries, false, windowsTarget});
        } catch (const std::runtime_error& error) {
            relinkerFailure = error.what();
        }
        Require(relinkerFailure.find("synthetic relinker failure") != std::string::npos,
                "relinker diagnostic was not captured");
        Require(relinkerFailure.find(Launcher::PathToUtf8(source)) == std::string::npos,
                "relinker diagnostic exposed the source path");
        std::filesystem::remove_all(root);
        std::cout << "Launcher install tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
