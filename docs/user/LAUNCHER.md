# Launcher

The AnyPS5 launcher installs a prepared game dump into a local library and starts the converted native executable. It does not decrypt packages, bypass signatures, download system software or contact a network service.

Build the launcher, relinker and patched runtime libraries together:

```text
cmake --build build --target launcher_bundle
```

The bundle is written to `build/launcher/`. Start `anyps5_launcher.exe` on Windows or `anyps5_launcher` on Linux.

## Install a game

Select a clean `.bin` or `.elf` executable from a dump you own, its extracted game directory, or a parent directory whose immediate child directories contain games. A typical main executable is named `eboot.bin`; its filename extension does not matter. Plain ELF and already-decrypted, uncompressed SELF wrappers are supported. On Windows, use **File** or **Folder**. On either platform, drag the file or directory onto the launcher or paste its path into **Game source**. Directory discovery looks for `eboot.bin` or `eboot.elf` in the selected directory and its immediate children. The source directory must contain the corresponding `sce_module/`, `sce_modules/` or `prx/` directory described in the [relinker usage guide](USAGE.md).

The launcher validates all discovered executables before installing any of them. It also checks `.prx` files directly inside the standard guest-module directories. The relinker reconstructs ELF images from plaintext SELF segment records. Encrypted or compressed SELF segments are rejected; no keys, signatures or DRM mechanisms are used. Package files are not extracted or decrypted.

The default game library is the platform's per-user data directory. On Windows, press **Browse** beside **Game library** to select another directory. You can also paste a directory path into the field. Selecting a library refreshes the installed-game list immediately. Do not place the library inside the source dump directory.

Enable **Intel compatibility** on an Intel host or when AMD-only instructions must be lowered. Press **Install game**. If a parent directory is selected, each discovered game is installed. The launcher:

1. Copies game resources into a private staging directory, excluding the original executable and bundled module directories.
2. Copies the built AnyPS5 PRX libraries and required MinGW runtime DLLs on Windows.
3. Runs the relinker and converts bundled modules.
4. Writes `game.registry.json` for repeatable import compatibility checks.
5. Publishes the completed installation atomically into the game library.

A failed installation removes its staging directory and leaves existing games unchanged. Installing another dump whose directory has the same name is rejected rather than overwritten.

## Run a game

Select an entry under **Installed games** and press **Run selected**. The game starts with its installation directory as its working directory. Use **Refresh** after changing the library path or its contents outside the launcher.

On Windows, the launcher observes the first 1.5 seconds of startup. If the executable exits during that interval, the launcher reports its NT status instead of reporting that the game started successfully.

Use the [compatibility evaluator](COMPATIBILITY_EVALUATION.md) to audit an installed game's imports, run project tests and perform a bounded startup probe with redacted diagnostics.

Linux currently uses drag-and-drop or path paste for selecting an executable. The launcher shows at most six installed games in this first version.
