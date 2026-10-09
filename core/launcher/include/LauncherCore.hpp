#ifndef CORE_LAUNCHER_INCLUDE_LAUNCHERCORE_HPP
#define CORE_LAUNCHER_INCLUDE_LAUNCHERCORE_HPP

#include <filesystem>
#include <string>
#include <vector>

namespace Launcher {

struct Game {
    std::string Name;
    std::filesystem::path Executable;
};

struct InstallRequest {
    std::filesystem::path SourceExecutable;
    std::filesystem::path LibraryRoot;
    std::filesystem::path RelinkerExecutable;
    std::filesystem::path LibrariesDirectory;
    bool ToIntel = false;
    bool WindowsTarget = false;
};

std::filesystem::path DefaultLibraryRoot();

std::filesystem::path FindRelinker(const std::filesystem::path& launcherExecutable);

std::filesystem::path FindLibraries(const std::filesystem::path& launcherExecutable);

std::vector<Game> DiscoverGames(const std::filesystem::path& libraryRoot, bool windowsTarget);

std::vector<std::filesystem::path> DiscoverSourceExecutables(const std::filesystem::path& selection);

Game InstallGame(const InstallRequest& request);

void LaunchGame(const Game& game);

std::string PathToUtf8(const std::filesystem::path& path);

std::filesystem::path PathFromUtf8(const std::string& value);

}

#endif
