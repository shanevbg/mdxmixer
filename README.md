# mdxmixer

A native Windows replacement for the SteelSeries Sonar mixer concept: apps
grouped into channels over virtual audio cables, each channel with its own EQ
and independent personal (what you hear) and streaming (what goes out)
volumes, a streaming submix any recorder can capture, and a processed virtual
microphone. One tray app, one named pipe, no service mesh — the design and
its rationale live in
[docs/specs/2026-09-22-mdxmixer-design.md](docs/specs/2026-09-22-mdxmixer-design.md).

## Build

Visual Studio 2022 (v143) with the C++ workload.

```text
powershell -ExecutionPolicy Bypass -File build.ps1            # Debug
powershell -ExecutionPolicy Bypass -File build.ps1 Release
powershell -ExecutionPolicy Bypass -File build.ps1 Test
powershell -ExecutionPolicy Bypass -File build.ps1 Debug Clean
```

x64 is the only platform, so there is nothing to select and no platform
suffix anywhere. Binaries land at the repo root:

```text
bin\Debug\mdxmixer.exe
bin\Release\mdxmixer.exe
bin\Test\mdxmixer_test.exe
```

Intermediates go to `obj\<Config>\`. Both trees are git-ignored. Compilation
is parallel (`/MP` per file, MSBuild `/m`); the build prints the core count.

## Tests

```text
bin\Test\mdxmixer_test.exe            # headless unit suite
bin\Test\mdxmixer_test.exe --audio    # cable-dependent integration tests
```

The `--audio` tests skip cleanly unless environment variables name real
endpoints (ids from `mdxmixer.exe --devices`):

| variable | meaning |
|---|---|
| `MDXM_TEST_RENDER` / `MDXM_TEST_CAPTURE` | channel cable pair (tone in / engine taps) |
| `MDXM_TEST_RENDER2` / `MDXM_TEST_CAPTURE2` | streaming cable pair (engine out / verification) |
| `MDXM_TEST_LOOPBACK=1` | capture ids equal the render ids, tapped via WASAPI loopback — runs with no cable installed |
| `MDXM_TEST_PERSONAL` | personal-output endpoint for the graph tests (defaults to the system default) |
| `MDXM_TEST_SOAK=1` | run the drift soak for the full 3 minutes |

Measurements are frequency-selective (a 440 Hz Goertzel bin), so live program
audio on the endpoints does not break the assertions.

## CLI modes

```text
mdxmixer.exe                 the tray app (single instance; a second launch raises the first's window)
mdxmixer.exe --devices       print every endpoint id/name
mdxmixer.exe --sessions      print live render sessions + whether the routing policy API answers
mdxmixer.exe --monitor <captureIdOrName> <renderIdOrName>
                             passthrough monitor: capture -> cushion -> unity render
```

## Runtime

- Config: `mdxmixer.json` beside the exe — portable, written atomically, a
  `"complete": true` marker written last guards against truncation.
- IPC: `\\.\pipe\mdxmixer`, UTF-16LE messages, `VERB|field=value|…` grammar;
  the verb set is in the spec's IPC section.
- Logs: `log/mdxmixer.log` beside the exe, level from config `logLevel`.

## License

CC-BY-NC 4.0 — see [LICENSE](LICENSE). Same terms and holder as MDropDX12,
which is what keeps the parts adapted from it clean.

## Prerequisite for real use

Virtual cables (VB-CABLE family or any signed cable driver) carry the
channels; see [docs/rollout.md](docs/rollout.md) for the ordered, reversible
rollout on a machine currently running Sonar.
