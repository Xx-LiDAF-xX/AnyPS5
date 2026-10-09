# Compatibility evaluation

`tools/compatibility_eval.py` runs repeatable checks against an installed game without adding third-party dependencies. It measures conditions that the project can verify automatically; it does not claim that a game is playable merely because it starts.

New launcher installations include `game.registry.json`. The evaluator combines that registry with the exact PRX files installed beside the game and the current runtime source tree. Reports use an anonymous game identifier and redact absolute paths.

Run static import and layout checks:

```text
python tools/compatibility_eval.py <INSTALLED_GAME_DIR> --json compatibility-report.json
```

Run the configured project tests first and observe startup for ten seconds:

```text
python tools/compatibility_eval.py <INSTALLED_GAME_DIR> --ctest <BUILD_DIR> --probe-seconds 10 --json compatibility-report.json
```

Build an anonymized static matrix for every dump below a directory:

```text
python tools/compatibility_matrix.py <DUMP_ROOT> --relinker <RELINKER> --libs <RUNTIME_LIBS> --json compatibility-matrix.json
```

Each dump is converted in its own temporary directory. Only the JSON matrix is retained. Use `--to-intel` to evaluate the Intel-lowered conversion path.

Verify and rank missing-import call sites in an installed game:

```text
python tools/callsite_abi.py <INSTALLED_GAME_DIR> --libs <RUNTIME_LIBS> --json callsite-report.json
```

Only byte-verified direct and tail calls are treated as ABI evidence. A recorded site that no longer matches the converted executable receives confidence `none`; reinstall the game with the current relinker before inferring a signature from it.

Add `--infer-abi` when GNU `objdump` is available to record recent writes to the six SysV integer argument registers before the first verified direct call. These writes are heuristic evidence, not a proven signature or argument count.

For Windows games, create a temporary Linux ELF conversion of the same clean dump and analyze that ELF and its registry. Windows packaging rewrites imported references after registry generation, so a Windows executable can legitimately report no byte-verified sites. The temporary ELF is for static analysis only and need not be launched.

The tool can perform and clean up that conversion automatically:

```text
python tools/callsite_abi.py <DUMP_DIR> --relinker <RELINKER> --libs <RUNTIME_LIBS> --infer-abi --json callsite-report.json
```

The launch probe terminates the game after the observation window. Run it only when losing unsaved game state is acceptable. Its result is `startup_survived`, not `playable`. Rendering, controls, audio, save data, networking, scene transitions and completion still require functional testing.

## Verdicts

| Verdict | Meaning |
|---------|---------|
| `project_tests_failed` | The selected CTest suite failed or timed out. Fix project regressions before interpreting the game result. |
| `load_blocked` | Required registry data, libraries or exports are missing, or Windows library ownership is incompatible. |
| `startup_failed` | The process exited during the observation window. The report classifies common native failure statuses and runtime messages. |
| `startup_survived` | The process remained alive for the requested observation period. Functional playability remains unverified. |
| `static_risk` | Static loading can proceed, but the title imports one or more throwing runtime stubs. |
| `static_ready` | No static blocker or known imported throwing stub was found. Runtime behavior remains unverified. |

## Report contents

The JSON report records the source commit and dirty state, test summary, unique and referenced import counts, missing libraries, absent imports, throwing stubs, provider mismatches, launch duration, normalized exit classification, recognized runtime signals and a short redacted output excerpt.

Problem imports retain NIDs and library names so compatibility work can target specific exports. The tool never downloads symbol databases, firmware, keys or proprietary files.

## Compatibility workflow

1. Run the project tests and fix regressions.
2. Eliminate missing libraries, absent imports and Windows provider mismatches.
3. Implement imported throwing stubs from documented clean-room behavior and add focused tests.
4. Run a short launch probe and classify the first deterministic failure.
5. For unresolved memory or behavior faults, isolate an experimental compatibility patch and state its assumption.
6. Record the before/after crash, test and gameplay results; keep the patch only when it improves the measured result without regressing the suite.
7. Repeat the same report after each change and compare blocker counts and startup behavior.
8. Perform manual functional testing before adding a result to the compatibility list.

Host CPU support, Vulkan device features, graphics-driver correctness and incomplete system-service behavior remain real constraints. Experimental patches are permitted, but the report keeps their effect observable and reproducible.
