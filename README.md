# spawn

An object for **Max** and **Pd** (vanilla and plugdata) that runs a command line and outputs what it prints. A typical use: start a Python script that sends data over OSC, see its messages in the console, and stop it when the patch closes.

```
[python -u tracker.py --port 9001(   [stop(
 |                                     |
[spawn                                  ]
 |                    |
[print]              [toggle]
```

- **Message**: the command line. It runs through `/bin/sh` in the folder of the patch, so relative paths, quotes, `*`, `>` and `&&` work as in a terminal.
- **`stop`**: sends ctrl-C; if the command is still running after 3 s, it is ended. Deleting the object or closing the patch also stops it.
- **Left outlet**: each line the command prints, normal output and errors, in order.
- **Right outlet**: 1 when the command starts, 0 when it ends.

One command at a time per object. Python output arrives line by line, because `PYTHONUNBUFFERED` is set.

## Requirements

| | |
|---|---|
| System | macOS 11 or later, Apple Silicon and Intel (one universal binary). Windows will follow. |
| Max | 8.2 or later (tested with Max 9) |
| Pd | tested with Pd vanilla 0.57 and plugdata 0.9.2 (Pd 0.55) |

## Install

Download the zip you need from [Releases](https://github.com/anatrini/spawn/releases/latest):

| Host | Zip | Move the `spawn` folder into |
|---|---|---|
| Max | `spawn-<version>-max-macos.zip` | `~/Documents/Max 9/Packages/` (Max 8: `~/Documents/Max 8/Packages/`) |
| Pd vanilla | `spawn-<version>-pd-macos.zip` | `~/Documents/Pd/externals/` |
| plugdata | `spawn-<version>-pd-macos.zip` | `~/Documents/plugdata/Externals/` |

The externals are not signed with an Apple developer certificate, so macOS blocks them the first time they load. Remove the quarantine flag once, in a terminal, on the folder you moved:

```
xattr -cr ~/Documents/Max\ 9/Packages/spawn
xattr -cr ~/Documents/Pd/externals/spawn
xattr -cr ~/Documents/plugdata/Externals/spawn
```

Then restart Max, Pd or plugdata and create a `[spawn]` object. The help patch opens from the object (right click → Help).

In Pd vanilla, if `[spawn]` is not found, check that `~/Documents/Pd/externals` is in Preferences → Path.

## Running a Python script

Programs outside `/usr/bin` and `/bin` need their path. For Python, give the interpreter of the script's environment, not a system `python3`. With a [uv](https://docs.astral.sh/uv/) environment `.venv` next to the patch:

```
.venv/bin/python script.py --port 9001
```

The path is relative to the folder of the patch, so the same message works on every computer that has the same folder layout. Save the patch first: an unsaved patch runs commands in the home folder.

In a Pd message box, a space inside a path is written with a backslash: `ls ~/Documents/Max\ 9`.

### Receiving OSC from the script

`[spawn]` only starts the script and shows what it prints; the data travel over OSC.

- **Max**: `[udpreceive 9001]`, then `[route /hand/raw]`.
- **Pd**: OSC is binary, so `[netreceive]` needs `-b`, followed by `[oscparse]`, which splits the address into words:

```
[netreceive -u -b 9001]
|
[oscparse]
|
[list trim]
|
[route hand]
|
[route raw]
```

Without `-b`, Pd reads the packet as text and stops at the first zero byte: only the address arrives, without the numbers.

## Troubleshooting

- **Nothing happens, or `sh: …: command not found`**: the program is not on the path of `/bin/sh`. Use its full path, or a path relative to the patch.
- **`command ended with error code …`**: the command failed; its own error messages are in the lines just above.
- **Camera or microphone**: macOS asks permission for the app that started the script (Max, Pd or plugdata), not for Python. The first time, the script may fail with a message such as `not authorized to capture video`; click Allow in the system dialog, or enable the app in System Settings → Privacy & Security → Camera (or Microphone), then send the command again. If the app is not listed: `tccutil reset Camera com.plugdata.plugdata` (or `com.cycling74.Max`, `info.puredata.pd.pd-gui`) and try again.
- **`… cannot be opened because the developer cannot be verified`**: run the `xattr -cr` command above on the installed folder.

## Build

Needs the Xcode command line tools and CMake 3.19 or later. For Max, also the Max SDK (Max Package Manager, `~/Documents/Max 9/Packages/max-sdk`); for Pd, the header `m_pd.h` of Pd 0.57-0 is downloaded at configure time and checked against a fixed SHA-256.

```
ln -s ~/Documents/Max\ 9/Packages/max-sdk max-sdk
cmake -S . -B build
cmake --build build
```

The Max external is written to `externals/spawn.mxo` (the repository is a Max package), the Pd external to `pd/spawn.pd_darwin`, next to `pd/spawn-help.pd`. Both are universal (arm64 and x86_64) and signed ad hoc. To build only one of them: `-DSPAWN_MAX=OFF` or `-DSPAWN_PD=OFF`.

Layout:

```
source/core/   process handling shared by both (posix_spawn, reader thread, stop)
source/max/    Max binding
source/pd/     Pd binding
help/          Max help patch
pd/            Pd help patch (and the built external)
tests/         tests of source/core
```

Tests of the process code, without Max or Pd. `SPAWN_TEST_PYTHON` is the full path of a Python interpreter, for example the one of a uv or conda environment; without it the Python checks are skipped.

```
cmake -S . -B build -DSPAWN_TESTS=ON
cmake --build build
SPAWN_TEST_PYTHON=/path/to/env/bin/python build/test_proc
```

## License

MIT, see [LICENSE](LICENSE). Changes between versions: [CHANGELOG.md](CHANGELOG.md).
