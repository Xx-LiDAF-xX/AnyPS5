#include <LauncherCore.hpp>
#include <SDL.h>
#include <SDL_syswm.h>
#include <algorithm>
#include <array>
#include <exception>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#endif

namespace {

constexpr SDL_Color Background{15, 18, 28, 255};
constexpr SDL_Color Panel{27, 32, 48, 255};
constexpr SDL_Color Field{37, 43, 61, 255};
constexpr SDL_Color Primary{104, 92, 255, 255};
constexpr SDL_Color Text{239, 241, 248, 255};
constexpr SDL_Color Muted{151, 158, 178, 255};
constexpr SDL_Color Success{73, 190, 132, 255};
constexpr SDL_Color Danger{232, 94, 112, 255};

struct UiState {
    std::string inputPath;
    std::string libraryPath;
    std::string status = "READY";
    bool statusError = false;
    bool toIntel = false;
    int focus = 0;
    int selected = -1;
    std::vector<Launcher::Game> games;
};

const char* Glyph(char character) {
    static constexpr std::array<const char*, 26> letters{
        "01110100011000111111100011000110001", "11110100011000111110100011000111110",
        "01111100001000010000100001000001111", "11110100011000110001100011000111110",
        "11111100001000011110100001000011111", "11111100001000011110100001000010000",
        "01111100001000010111100011000101111", "10001100011000111111100011000110001",
        "11111001000010000100001000010011111", "00111000100001000010100101001001100",
        "10001100101010011000101001001010001", "10000100001000010000100001000011111",
        "10001110111010110101100011000110001", "10001110011010110011100011000110001",
        "01110100011000110001100011000101110", "11110100011000111110100001000010000",
        "01110100011000110001101011001001101", "11110100011000111110101001001010001",
        "01111100001000001110000010000111110", "11111001000010000100001000010000100",
        "10001100011000110001100011000101110", "10001100011000110001100010101000100",
        "10001100011000110101101011101110001", "10001100010101000100010101000110001",
        "10001100010101000100001000010000100", "11111000010001000100010001000011111"
    };
    static constexpr std::array<const char*, 10> digits{
        "01110100011001110101110011000101110", "00100011000010000100001000010001110",
        "01110100010000100010001000100011111", "11110000010000101110000010000111110",
        "00010001100101010010111110001000010", "11111100001000011110000010000111110",
        "01110100001000011110100011000101110", "11111000010001000100010000100001000",
        "01110100011000101110100011000101110", "01110100011000101111000010000101110"
    };
    if (character >= 'a' && character <= 'z') character = static_cast<char>(character - 'a' + 'A');
    if (character >= 'A' && character <= 'Z') return letters[character - 'A'];
    if (character >= '0' && character <= '9') return digits[character - '0'];
    switch (character) {
        case '-': return "00000000000000011111000000000000000";
        case '_': return "00000000000000000000000000000011111";
        case '.': return "00000000000000000000000000011000110";
        case ':': return "00000001100011000000001100011000000";
        case '/': return "00001000100001000100010001000010000";
        case '\\': return "10000010000010000010000100000100001";
        case '(': return "00010001000100001000010000010000010";
        case ')': return "01000001000001000010000100010001000";
        case '+': return "00000001000010011111001000010000000";
        default: return "00000000000000000000000000000000000";
    }
}

void Fill(SDL_Surface* surface, const SDL_Rect& rectangle, SDL_Color color) {
    SDL_FillRect(surface, &rectangle, SDL_MapRGB(surface->format, color.r, color.g, color.b));
}

void DrawText(SDL_Surface* surface, int x, int y, std::string value, SDL_Color color, int scale = 2) {
    const std::uint32_t pixel = SDL_MapRGB(surface->format, color.r, color.g, color.b);
    for (char character : value) {
        const char* glyph = Glyph(character);
        for (int row = 0; row < 7; ++row) {
            for (int column = 0; column < 5; ++column) {
                if (glyph[row * 5 + column] != '1') continue;
                SDL_Rect point{x + column * scale, y + row * scale, scale, scale};
                SDL_FillRect(surface, &point, pixel);
            }
        }
        x += 6 * scale;
    }
}

std::string FitText(const std::string& value, std::size_t characters) {
    if (value.size() <= characters) return value;
    if (characters <= 3) return value.substr(0, characters);
    return "..." + value.substr(value.size() - characters + 3);
}

bool Contains(const SDL_Rect& rectangle, int x, int y) {
    return x >= rectangle.x && y >= rectangle.y && x < rectangle.x + rectangle.w && y < rectangle.y + rectangle.h;
}

void Refresh(UiState& state) {
    try {
#ifdef _WIN32
        constexpr bool windowsTarget = true;
#else
        constexpr bool windowsTarget = false;
#endif
        state.games = Launcher::DiscoverGames(Launcher::PathFromUtf8(state.libraryPath), windowsTarget);
        if (state.selected >= static_cast<int>(state.games.size())) state.selected = -1;
        state.status = state.games.empty() ? "NO INSTALLED GAMES" : std::to_string(state.games.size()) + " GAMES READY";
        state.statusError = false;
    } catch (const std::filesystem::filesystem_error& error) {
        state.status = "FILE OPERATION FAILED: " + std::to_string(error.code().value());
        state.statusError = true;
    } catch (const std::exception& error) {
        state.status = error.what();
        state.statusError = true;
    }
}

#ifdef _WIN32
std::string BrowseGame(SDL_Window* window) {
    std::array<wchar_t, 32768> buffer{};
    SDL_SysWMinfo windowInfo{};
    SDL_VERSION(&windowInfo.version);
    if (!SDL_GetWindowWMInfo(window, &windowInfo)) return {};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = windowInfo.info.win.window;
    dialog.lpstrFilter = L"PS5 executables\0*.bin;*.elf\0All files\0*.*\0";
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return {};
    return Launcher::PathToUtf8(std::filesystem::path(buffer.data()));
}

std::string BrowseDirectory(SDL_Window* window) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SDL_SysWMinfo windowInfo{};
    SDL_VERSION(&windowInfo.version);
    if (!SDL_GetWindowWMInfo(window, &windowInfo)) {
        if (SUCCEEDED(initialized)) CoUninitialize();
        return {};
    }
    BROWSEINFOW dialog{};
    dialog.hwndOwner = windowInfo.info.win.window;
    dialog.lpszTitle = L"Select an extracted game folder or a folder containing games";
    dialog.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&dialog);
    if (item == nullptr) {
        if (SUCCEEDED(initialized)) CoUninitialize();
        return {};
    }
    std::array<wchar_t, 32768> buffer{};
    const bool converted = SHGetPathFromIDListW(item, buffer.data()) != FALSE;
    CoTaskMemFree(item);
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (!converted) return {};
    return Launcher::PathToUtf8(std::filesystem::path(buffer.data()));
}
#endif

void Draw(SDL_Window* window, const UiState& state) {
    SDL_Surface* surface = SDL_GetWindowSurface(window);
    SDL_FillRect(surface, nullptr, SDL_MapRGB(surface->format, Background.r, Background.g, Background.b));
    DrawText(surface, 36, 28, "ANYPS5 LAUNCHER", Text, 4);
    DrawText(surface, 38, 65, "INSTALL A CLEAN GAME DUMP AND RUN NATIVE OUTPUTS", Muted, 2);
    Fill(surface, {28, 98, 844, 194}, Panel);
    DrawText(surface, 48, 116, "GAME SOURCE", Text, 2);
    Fill(surface, {48, 142, 580, 38}, state.focus == 1 ? Primary : Field);
    Fill(surface, {50, 144, 576, 34}, Field);
    DrawText(surface, 60, 154, state.inputPath.empty() ? "DROP A FILE OR FOLDER HERE OR PASTE A PATH" : FitText(state.inputPath, 43),
             state.inputPath.empty() ? Muted : Text, 2);
    Fill(surface, {640, 142, 88, 38}, Primary);
    DrawText(surface, 659, 154, "FILE", Text, 2);
    Fill(surface, {738, 142, 112, 38}, Primary);
    DrawText(surface, 751, 154, "FOLDER", Text, 2);
    DrawText(surface, 48, 198, "GAME LIBRARY", Text, 2);
    Fill(surface, {48, 224, 500, 38}, state.focus == 2 ? Primary : Field);
    Fill(surface, {50, 226, 496, 34}, Field);
    DrawText(surface, 60, 236, FitText(state.libraryPath, 36), Text, 2);
    Fill(surface, {560, 224, 110, 38}, Primary);
    DrawText(surface, 573, 236, "BROWSE", Text, 2);
    Fill(surface, {688, 230, 18, 18}, state.toIntel ? Primary : Field);
    if (state.toIntel) DrawText(surface, 692, 232, "X", Text, 2);
    DrawText(surface, 716, 232, "TO INTEL", Text, 2);
    Fill(surface, {704, 256, 146, 28}, Primary);
    DrawText(surface, 716, 264, "INSTALL GAME", Text, 2);
    DrawText(surface, 36, 318, "INSTALLED GAMES", Text, 3);
    Fill(surface, {28, 350, 844, 218}, Panel);
    if (state.games.empty()) DrawText(surface, 50, 378, "NO GAMES INSTALLED", Muted, 2);
    for (std::size_t index = 0; index < state.games.size() && index < 6; ++index) {
        SDL_Rect row{44, 366 + static_cast<int>(index) * 31, 812, 27};
        Fill(surface, row, static_cast<int>(index) == state.selected ? Primary : Field);
        DrawText(surface, 56, row.y + 7, FitText(state.games[index].Name, 60), Text, 2);
    }
    Fill(surface, {28, 586, 170, 38}, Primary);
    DrawText(surface, 45, 598, "RUN SELECTED", Text, 2);
    Fill(surface, {212, 586, 106, 38}, Field);
    DrawText(surface, 226, 598, "REFRESH", Text, 2);
    DrawText(surface, 344, 598, FitText(state.status, 42), state.statusError ? Danger : Success, 2);
    SDL_UpdateWindowSurface(window);
}

}

int main(int argc, char* argv[]) {
    const bool smokeTest = argc > 1 && std::string(argv[1]) == "--smoke-test";
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 1;
    if (!SDL_HasAVX2()) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Unsupported CPU", "AnyPS5 requires an x86-64 CPU with AVX2 support.", nullptr);
        SDL_Quit();
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("AnyPS5 Launcher", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          900, 650, SDL_WINDOW_ALLOW_HIGHDPI);
    if (window == nullptr) {
        SDL_Quit();
        return 1;
    }
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    SDL_StartTextInput();
    UiState state;
    state.libraryPath = Launcher::PathToUtf8(Launcher::DefaultLibraryRoot());
    if (argc > 1 && !smokeTest) state.inputPath = argv[1];
    Refresh(state);
    bool running = true;
    while (running) {
        Draw(window, state);
        if (smokeTest) break;
        SDL_Event event{};
        if (!SDL_WaitEvent(&event)) continue;
        if (event.type == SDL_QUIT) running = false;
        if (event.type == SDL_DROPFILE) {
            state.inputPath = event.drop.file;
            SDL_free(event.drop.file);
            state.focus = 1;
        }
        if (event.type == SDL_TEXTINPUT && state.focus != 0) {
            auto& value = state.focus == 1 ? state.inputPath : state.libraryPath;
            value += event.text.text;
        }
        if (event.type == SDL_KEYDOWN && state.focus != 0) {
            auto& value = state.focus == 1 ? state.inputPath : state.libraryPath;
            if (event.key.keysym.sym == SDLK_BACKSPACE && !value.empty()) value.pop_back();
            if (event.key.keysym.sym == SDLK_v && (event.key.keysym.mod & KMOD_CTRL) != 0 && SDL_HasClipboardText()) {
                char* clipboard = SDL_GetClipboardText();
                if (clipboard != nullptr) {
                    value = clipboard;
                    SDL_free(clipboard);
                }
            }
            if (event.key.keysym.sym == SDLK_TAB) state.focus = state.focus == 1 ? 2 : 1;
        }
        if (event.type != SDL_MOUSEBUTTONDOWN || event.button.button != SDL_BUTTON_LEFT) continue;
        const int x = event.button.x;
        const int y = event.button.y;
        if (Contains({48, 142, 580, 38}, x, y)) state.focus = 1;
        if (Contains({48, 224, 500, 38}, x, y)) state.focus = 2;
        if (Contains({688, 224, 162, 28}, x, y)) state.toIntel = !state.toIntel;
        if (Contains({640, 142, 88, 38}, x, y)) {
#ifdef _WIN32
            const auto selected = BrowseGame(window);
            if (!selected.empty()) state.inputPath = selected;
#else
            state.status = "DRAG A GAME .BIN OR .ELF HERE OR PASTE ITS PATH";
            state.statusError = false;
#endif
        }
        if (Contains({738, 142, 112, 38}, x, y)) {
#ifdef _WIN32
            const auto selected = BrowseDirectory(window);
            if (!selected.empty()) state.inputPath = selected;
#else
            state.status = "DRAG A GAME FOLDER HERE OR PASTE ITS PATH";
            state.statusError = false;
#endif
        }
        if (Contains({560, 224, 110, 38}, x, y)) {
#ifdef _WIN32
            const auto selected = BrowseDirectory(window);
            if (!selected.empty()) {
                state.libraryPath = selected;
                state.focus = 2;
                Refresh(state);
            }
#else
            state.status = "PASTE THE GAME LIBRARY PATH INTO THE FIELD";
            state.statusError = false;
#endif
        }
        if (Contains({704, 256, 146, 28}, x, y)) {
            try {
                state.status = "INSTALLING";
                state.statusError = false;
                Draw(window, state);
#ifdef _WIN32
                constexpr bool windowsTarget = true;
#else
                constexpr bool windowsTarget = false;
#endif
                const auto launcherExecutable = Launcher::PathFromUtf8(argv[0]);
                const auto sources = Launcher::DiscoverSourceExecutables(Launcher::PathFromUtf8(state.inputPath));
                const auto relinker = Launcher::FindRelinker(launcherExecutable);
                const auto libraries = Launcher::FindLibraries(launcherExecutable);
                for (const auto& source : sources) {
                    Launcher::InstallGame({source, Launcher::PathFromUtf8(state.libraryPath), relinker, libraries,
                        state.toIntel, windowsTarget});
                }
                Refresh(state);
                state.status = std::to_string(sources.size()) + (sources.size() == 1 ? " GAME INSTALLED" : " GAMES INSTALLED");
                state.statusError = false;
            } catch (const std::filesystem::filesystem_error& error) {
                state.status = "FILE OPERATION FAILED: " + std::to_string(error.code().value());
                state.statusError = true;
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Installation failed", state.status.c_str(), window);
            } catch (const std::exception& error) {
                state.status = error.what();
                state.statusError = true;
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Installation failed", error.what(), window);
            }
        }
        if (Contains({212, 586, 106, 38}, x, y)) Refresh(state);
        if (Contains({28, 586, 170, 38}, x, y) && state.selected >= 0) {
            try {
                Launcher::LaunchGame(state.games[static_cast<std::size_t>(state.selected)]);
                state.status = "GAME STARTED";
                state.statusError = false;
            } catch (const std::filesystem::filesystem_error& error) {
                state.status = "FILE OPERATION FAILED: " + std::to_string(error.code().value());
                state.statusError = true;
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Launch failed", state.status.c_str(), window);
            } catch (const std::exception& error) {
                state.status = error.what();
                state.statusError = true;
                SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Launch failed", error.what(), window);
            }
        }
        for (std::size_t index = 0; index < state.games.size() && index < 6; ++index) {
            if (Contains({44, 366 + static_cast<int>(index) * 31, 812, 27}, x, y)) state.selected = static_cast<int>(index);
        }
    }
    SDL_StopTextInput();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
