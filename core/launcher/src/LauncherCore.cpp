#include <LauncherCore.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace Launcher {
namespace {

enum class ImageKind {
    Elf,
    Self,
    Unknown
};

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

std::filesystem::path Canonical(const std::filesystem::path& path) {
    std::error_code error;
    auto result = std::filesystem::weakly_canonical(path, error);
    if (error) throw std::runtime_error("cannot resolve path: " + std::to_string(error.value()));
    return result;
}

bool IsInside(const std::filesystem::path& child, const std::filesystem::path& parent) {
    auto childPart = child.begin();
    for (auto parentPart = parent.begin(); parentPart != parent.end(); ++parentPart, ++childPart) {
        if (childPart == child.end() || *childPart != *parentPart) return false;
    }
    return true;
}

std::string InstallName(const std::filesystem::path& sourceExecutable) {
    std::string value = PathToUtf8(sourceExecutable.parent_path().filename());
    if (value.empty()) value = PathToUtf8(sourceExecutable.stem());
    for (char& character : value) {
        const bool allowed = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') || character == '-' || character == '_' || character == '.';
        if (!allowed) character = '_';
    }
    while (!value.empty() && (value.back() == '.' || value.back() == ' ')) value.pop_back();
    if (value.empty() || value == "." || value == "..") throw std::runtime_error("game directory has no usable name");
    return value;
}

bool IsModuleDirectory(const std::filesystem::path& relative) {
    if (relative.empty()) return false;
    const auto first = PathToUtf8(*relative.begin());
    return first == "sce_module" || first == "sce_modules" || first == "prx";
}

ImageKind ReadImageKind(const std::filesystem::path& path) {
    std::array<unsigned char, 4> magic{};
    std::ifstream stream(path, std::ios::binary);
    stream.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(magic.size()));
    if (stream.gcount() != static_cast<std::streamsize>(magic.size())) return ImageKind::Unknown;
    if (magic == std::array<unsigned char, 4>{0x7f, 'E', 'L', 'F'}) return ImageKind::Elf;
    if (magic == std::array<unsigned char, 4>{0x4f, 0x15, 0x3d, 0x1d} ||
        magic == std::array<unsigned char, 4>{0x54, 0x14, 0xf5, 0xee}) return ImageKind::Self;
    return ImageKind::Unknown;
}

void ValidatePlainSelf(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::array<unsigned char, 32> header{};
    stream.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (stream.gcount() != static_cast<std::streamsize>(header.size()))
        throw std::runtime_error("selected SELF header is truncated");
    const std::uint16_t entryCount = static_cast<std::uint16_t>(header[24]) |
        (static_cast<std::uint16_t>(header[25]) << 8);
    if (entryCount == 0) throw std::runtime_error("selected SELF contains no segment entries");
    for (std::uint16_t index = 0; index < entryCount; ++index) {
        stream.seekg(static_cast<std::streamoff>(32 + static_cast<std::size_t>(index) * 32));
        std::array<unsigned char, 8> encoded{};
        stream.read(reinterpret_cast<char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
        if (stream.gcount() != static_cast<std::streamsize>(encoded.size()))
            throw std::runtime_error("selected SELF segment table is truncated");
        std::uint64_t properties = 0;
        for (std::size_t byte = 0; byte < encoded.size(); ++byte)
            properties |= static_cast<std::uint64_t>(encoded[byte]) << (byte * 8);
        if ((properties & 2) != 0) throw std::runtime_error("encrypted SELF segments are unsupported");
        if ((properties & 8) != 0) throw std::runtime_error("compressed SELF segments are unsupported");
    }
    stream.seekg(static_cast<std::streamoff>(32 + static_cast<std::size_t>(entryCount) * 32));
    std::array<unsigned char, 4> elfMagic{};
    stream.read(reinterpret_cast<char*>(elfMagic.data()), static_cast<std::streamsize>(elfMagic.size()));
    if (stream.gcount() != static_cast<std::streamsize>(elfMagic.size()) ||
        elfMagic != std::array<unsigned char, 4>{0x7f, 'E', 'L', 'F'})
        throw std::runtime_error("selected SELF has no embedded ELF header");
}

void ValidateGameSource(const std::filesystem::path& sourceExecutable) {
    const auto executableKind = ReadImageKind(sourceExecutable);
    if (executableKind != ImageKind::Elf && executableKind != ImageKind::Self)
        throw std::runtime_error("selected .bin or .elf file is not a supported ELF or SELF image");
    if (executableKind == ImageKind::Self) ValidatePlainSelf(sourceExecutable);
    const auto root = sourceExecutable.parent_path();
    const bool singular = std::filesystem::exists(root / "sce_module");
    const bool plural = std::filesystem::exists(root / "sce_modules");
    if (singular && plural) throw std::runtime_error("both sce_module and sce_modules exist beside the game executable");
    std::size_t invalidCount = 0;
    for (const auto& directory : {root / "sce_module", root / "sce_modules", root / "prx"}) {
        if (!std::filesystem::exists(directory)) continue;
        if (!std::filesystem::is_directory(directory)) throw std::runtime_error("a guest module path is not a directory");
        for (const auto& item : std::filesystem::directory_iterator(directory)) {
            if (!item.is_regular_file() || LowerAscii(PathToUtf8(item.path().extension())) != ".prx") continue;
            const auto kind = ReadImageKind(item.path());
            if (kind == ImageKind::Self) ValidatePlainSelf(item.path());
            if (kind == ImageKind::Unknown) ++invalidCount;
        }
    }
    if (invalidCount != 0)
        throw std::runtime_error("source contains " + std::to_string(invalidCount) +
                                 " non-ELF guest module(s)");
}

std::vector<std::filesystem::path> FindEboots(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> matches;
    for (const auto& item : std::filesystem::directory_iterator(directory)) {
        if (!item.is_regular_file()) continue;
        const std::string name = LowerAscii(PathToUtf8(item.path().filename()));
        if (name == "eboot.bin" || name == "eboot.elf") matches.push_back(item.path());
    }
    return matches;
}

void CopyResources(const std::filesystem::path& sourceRoot, const std::filesystem::path& sourceExecutable,
                   const std::filesystem::path& destination) {
    for (std::filesystem::recursive_directory_iterator item(sourceRoot), end; item != end; ++item) {
        const auto relative = item->path().lexically_relative(sourceRoot);
        if (IsModuleDirectory(relative)) {
            if (item->is_directory()) item.disable_recursion_pending();
            continue;
        }
        if (item->is_symlink()) throw std::runtime_error("game resources contain a symbolic link");
        if (Canonical(item->path()) == sourceExecutable) continue;
        const auto output = destination / relative;
        if (item->is_directory()) {
            std::filesystem::create_directories(output);
        } else if (item->is_regular_file()) {
            std::filesystem::create_directories(output.parent_path());
            std::filesystem::copy_file(item->path(), output, std::filesystem::copy_options::none);
        }
    }
}

void CopyLibraries(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::size_t count = 0;
    std::filesystem::create_directories(destination);
    for (const auto& item : std::filesystem::directory_iterator(source)) {
        if (!item.is_regular_file()) continue;
        const auto extension = LowerAscii(PathToUtf8(item.path().extension()));
        if (extension != ".prx" && extension != ".dll") continue;
        std::filesystem::copy_file(item.path(), destination / item.path().filename(), std::filesystem::copy_options::none);
        if (extension == ".prx") ++count;
    }
    if (count == 0) throw std::runtime_error("runtime library directory contains no PRX files");
}

#ifdef _WIN32
std::wstring Quote(const std::filesystem::path& value) {
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : value.native()) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted.push_back(character);
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

DWORD RunProcess(const std::filesystem::path& executable, const std::vector<std::filesystem::path>& arguments,
                 const std::filesystem::path& workingDirectory, bool wait, std::string* capturedOutput = nullptr) {
    std::wstring command = Quote(executable);
    for (const auto& argument : arguments) command += L" " + Quote(argument);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    HANDLE outputRead = nullptr;
    HANDLE outputWrite = nullptr;
    HANDLE inputHandle = nullptr;
    if (wait && capturedOutput != nullptr) {
        SECURITY_ATTRIBUTES attributes{};
        attributes.nLength = sizeof(attributes);
        attributes.bInheritHandle = TRUE;
        inputHandle = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (!CreatePipe(&outputRead, &outputWrite, &attributes, 0) ||
            !SetHandleInformation(outputRead, HANDLE_FLAG_INHERIT, 0) || inputHandle == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (outputRead != nullptr) CloseHandle(outputRead);
            if (outputWrite != nullptr) CloseHandle(outputWrite);
            if (inputHandle != nullptr && inputHandle != INVALID_HANDLE_VALUE) CloseHandle(inputHandle);
            throw std::runtime_error("cannot create process output pipe: " + std::to_string(error));
        }
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = inputHandle;
        startup.hStdOutput = outputWrite;
        startup.hStdError = outputWrite;
    }
    PROCESS_INFORMATION process{};
    const DWORD flags = wait ? CREATE_NO_WINDOW : CREATE_NEW_PROCESS_GROUP;
    const BOOL inheritHandles = outputWrite != nullptr ? TRUE : FALSE;
    if (!CreateProcessW(executable.c_str(), mutableCommand.data(), nullptr, nullptr, inheritHandles, flags, nullptr,
                        workingDirectory.c_str(), &startup, &process)) {
        const DWORD error = GetLastError();
        if (outputRead != nullptr) CloseHandle(outputRead);
        if (outputWrite != nullptr) CloseHandle(outputWrite);
        if (inputHandle != nullptr) CloseHandle(inputHandle);
        throw std::runtime_error("cannot start process: " + std::to_string(error));
    }
    if (outputWrite != nullptr) CloseHandle(outputWrite);
    if (inputHandle != nullptr) CloseHandle(inputHandle);
    CloseHandle(process.hThread);
    if (!wait) {
        const DWORD startup = WaitForSingleObject(process.hProcess, 1500);
        if (startup == WAIT_TIMEOUT) {
            CloseHandle(process.hProcess);
            return 0;
        }
        if (startup != WAIT_OBJECT_0) {
            const DWORD error = GetLastError();
            CloseHandle(process.hProcess);
            throw std::runtime_error("cannot observe game startup: " + std::to_string(error));
        }
        DWORD exitCode = 0;
        if (!GetExitCodeProcess(process.hProcess, &exitCode)) {
            const DWORD error = GetLastError();
            CloseHandle(process.hProcess);
            throw std::runtime_error("cannot read game startup status: " + std::to_string(error));
        }
        CloseHandle(process.hProcess);
        std::ostringstream status;
        status << "game exited during startup with status 0x" << std::hex << std::setw(8) << std::setfill('0') << exitCode;
        throw std::runtime_error(status.str());
    }
    if (outputRead != nullptr) {
        std::array<char, 4096> buffer{};
        DWORD count = 0;
        while (ReadFile(outputRead, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) && count != 0)
            capturedOutput->append(buffer.data(), count);
        CloseHandle(outputRead);
    }
    if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0) {
        const DWORD error = GetLastError();
        CloseHandle(process.hProcess);
        throw std::runtime_error("cannot wait for process: " + std::to_string(error));
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(process.hProcess, &exitCode)) {
        const DWORD error = GetLastError();
        CloseHandle(process.hProcess);
        throw std::runtime_error("cannot read process exit code: " + std::to_string(error));
    }
    CloseHandle(process.hProcess);
    return exitCode;
}
#else
int RunProcess(const std::filesystem::path& executable, const std::vector<std::filesystem::path>& arguments,
               const std::filesystem::path& workingDirectory, bool wait, std::string* capturedOutput = nullptr) {
    (void)capturedOutput;
    const pid_t process = fork();
    if (process < 0) throw std::runtime_error("cannot start process");
    if (process == 0) {
        if (chdir(workingDirectory.c_str()) != 0) _exit(126);
        std::vector<std::string> values;
        values.reserve(arguments.size() + 1);
        values.push_back(executable.string());
        for (const auto& argument : arguments) values.push_back(argument.string());
        std::vector<char*> argv;
        argv.reserve(values.size() + 1);
        for (auto& value : values) argv.push_back(value.data());
        argv.push_back(nullptr);
        execv(executable.c_str(), argv.data());
        _exit(127);
    }
    if (!wait) return 0;
    int status = 0;
    if (waitpid(process, &status, 0) < 0) throw std::runtime_error("cannot wait for process");
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 125;
}
#endif

void ReplaceAll(std::string& value, const std::string& needle, const std::string& replacement) {
    if (needle.empty()) return;
    std::size_t position = 0;
    while ((position = value.find(needle, position)) != std::string::npos) {
        value.replace(position, needle.size(), replacement);
        position += replacement.size();
    }
}

std::string RelinkerFailure(std::string output, const std::filesystem::path& sourceRoot,
                            const std::filesystem::path& outputPath) {
    ReplaceAll(output, PathToUtf8(sourceRoot), "<GAME_DIR>");
    ReplaceAll(output, PathToUtf8(outputPath), "<OUTPUT>");
    std::size_t begin = output.find("FAIL:");
    if (begin == std::string::npos) return {};
    std::size_t end = output.find_first_of("\r\n", begin);
    std::string message = output.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    if (message.size() > 400) message.resize(400);
    return message;
}

}

std::filesystem::path DefaultLibraryRoot() {
#ifdef _WIN32
    const char* localData = std::getenv("LOCALAPPDATA");
    if (localData != nullptr && *localData != '\0') return PathFromUtf8(localData) / "AnyPS5" / "Games";
#else
    const char* dataHome = std::getenv("XDG_DATA_HOME");
    if (dataHome != nullptr && *dataHome != '\0') return PathFromUtf8(dataHome) / "anyps5" / "games";
    const char* home = std::getenv("HOME");
    if (home != nullptr && *home != '\0') return PathFromUtf8(home) / ".local" / "share" / "anyps5" / "games";
#endif
    return std::filesystem::current_path() / "games";
}

std::filesystem::path FindRelinker(const std::filesystem::path& launcherExecutable) {
#ifdef _WIN32
    constexpr const char* executableName = "relinker.exe";
#else
    constexpr const char* executableName = "relinker";
#endif
    const auto directory = Canonical(launcherExecutable).parent_path();
    for (const auto& candidate : {directory / executableName, directory / ".." / "relinker" / executableName}) {
        if (std::filesystem::is_regular_file(candidate)) return Canonical(candidate);
    }
    throw std::runtime_error("cannot find relinker beside the launcher");
}

std::filesystem::path FindLibraries(const std::filesystem::path& launcherExecutable) {
    const auto directory = Canonical(launcherExecutable).parent_path();
    for (const auto& candidate : {directory / "libs", directory / ".." / "libs" / "libs"}) {
        if (std::filesystem::is_directory(candidate)) return Canonical(candidate);
    }
    throw std::runtime_error("cannot find runtime libraries beside the launcher");
}

std::vector<Game> DiscoverGames(const std::filesystem::path& libraryRoot, bool windowsTarget) {
    std::vector<Game> games;
    if (!std::filesystem::exists(libraryRoot)) return games;
    if (!std::filesystem::is_directory(libraryRoot)) throw std::runtime_error("game library is not a directory");
    const std::filesystem::path executableName = windowsTarget ? "game.exe" : "game.elf";
    for (const auto& item : std::filesystem::directory_iterator(libraryRoot)) {
        if (!item.is_directory()) continue;
        const auto executable = item.path() / executableName;
        if (std::filesystem::is_regular_file(executable)) games.push_back({PathToUtf8(item.path().filename()), executable});
    }
    std::sort(games.begin(), games.end(), [](const Game& left, const Game& right) { return left.Name < right.Name; });
    return games;
}

std::vector<std::filesystem::path> DiscoverSourceExecutables(const std::filesystem::path& selection) {
    if (!std::filesystem::exists(selection)) throw std::runtime_error("game source does not exist");
    if (std::filesystem::is_regular_file(selection)) {
        const auto executable = Canonical(selection);
        ValidateGameSource(executable);
        return {executable};
    }
    if (!std::filesystem::is_directory(selection)) throw std::runtime_error("game source is not a file or directory");
    const auto root = Canonical(selection);
    auto matches = FindEboots(root);
    if (matches.empty()) {
        for (const auto& item : std::filesystem::directory_iterator(root)) {
            if (!item.is_directory() || item.is_symlink()) continue;
            auto childMatches = FindEboots(item.path());
            matches.insert(matches.end(), childMatches.begin(), childMatches.end());
        }
    }
    if (matches.empty()) throw std::runtime_error("no eboot.bin or eboot.elf was found in the selected directory");
    std::sort(matches.begin(), matches.end());
    for (const auto& executable : matches) ValidateGameSource(executable);
    return matches;
}

Game InstallGame(const InstallRequest& request) {
    const auto sourceExecutable = Canonical(request.SourceExecutable);
    const auto sourceRoot = sourceExecutable.parent_path();
    if (!std::filesystem::is_regular_file(sourceExecutable)) throw std::runtime_error("game executable is not a regular file");
    ValidateGameSource(sourceExecutable);
    if (!std::filesystem::is_regular_file(request.RelinkerExecutable)) throw std::runtime_error("relinker executable is missing");
    if (!std::filesystem::is_directory(request.LibrariesDirectory)) throw std::runtime_error("runtime library directory is missing");
    std::filesystem::create_directories(request.LibraryRoot);
    const auto libraryRoot = Canonical(request.LibraryRoot);
    if (IsInside(libraryRoot, sourceRoot)) throw std::runtime_error("game library cannot be inside the source game directory");
    const auto name = InstallName(sourceExecutable);
    const auto destination = libraryRoot / PathFromUtf8(name);
    if (std::filesystem::exists(destination)) throw std::runtime_error("a game with this directory name is already installed");
    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto staging = libraryRoot / PathFromUtf8("." + name + ".installing-" + suffix);
    const auto output = staging / (request.WindowsTarget ? "game.exe" : "game.elf");
    try {
        std::filesystem::create_directories(staging / "app0");
        CopyResources(sourceRoot, sourceExecutable, staging / "app0");
        CopyLibraries(Canonical(request.LibrariesDirectory), staging / "libs");
        std::vector<std::filesystem::path> arguments;
        if (request.WindowsTarget) {
            arguments.emplace_back("--windows");
            arguments.emplace_back("--windows-gui");
        }
        if (request.ToIntel) arguments.emplace_back("--to-intel");
        arguments.emplace_back("--registry");
        arguments.push_back(sourceExecutable);
        arguments.push_back(output);
        std::string relinkerOutput;
        const auto exitCode = RunProcess(Canonical(request.RelinkerExecutable), arguments, sourceRoot, true, &relinkerOutput);
        if (exitCode != 0) {
            const auto detail = RelinkerFailure(std::move(relinkerOutput), sourceRoot, output);
            throw std::runtime_error("relinker failed with exit code " + std::to_string(exitCode) +
                                     (detail.empty() ? std::string{} : ": " + detail));
        }
#ifndef _WIN32
        std::filesystem::permissions(output,
            std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec | std::filesystem::perms::others_exec,
            std::filesystem::perm_options::add);
#endif
        std::filesystem::rename(staging, destination);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        throw;
    }
    return {name, destination / output.filename()};
}

void LaunchGame(const Game& game) {
    if (!std::filesystem::is_regular_file(game.Executable)) throw std::runtime_error("installed game executable is missing");
    RunProcess(Canonical(game.Executable), {}, Canonical(game.Executable).parent_path(), false);
}

std::string PathToUtf8(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
#else
    return path.string();
#endif
}

std::filesystem::path PathFromUtf8(const std::string& value) {
#ifdef _WIN32
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
#else
    return std::filesystem::path(value);
#endif
}

}
