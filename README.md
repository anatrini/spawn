# spawn

An object for Max and Pd (vanilla and plugdata) that runs a command line and outputs what it prints. For example, it starts a Python script that sends data over OSC, and stops it when the patch closes.

```
[python -u tracker.py --port 9001(   [stop(
 |                                     |
[spawn                                  ]
 |                    |
[print]              [toggle]
```

- **Message**: the command line. It runs through `/bin/sh` in the folder of the patch, so quotes, `*`, `>` and `&&` work as in a terminal. Programs outside `/usr/bin` and `/bin` need their full path, for example the `python` of a conda or uv environment.
- **`stop`**: sends ctrl-C; if the command is still running after 3 s, it is ended. Deleting the object or closing the patch also stops it.
- **Left outlet**: each line the command prints (normal output and errors, in order).
- **Right outlet**: 1 when the command starts, 0 when it ends.

One command at a time per object. Python output arrives line by line (`PYTHONUNBUFFERED` is set).

In a Pd message box, a space inside a path is written with a backslash: `ls ~/Documents/Max\ 9`.

macOS only for now (Apple Silicon and Intel, macOS 11 or later). Windows will follow.

## Install

Download from [Releases](https://github.com/anatrini/spawn/releases/latest):

- **Max** (8.2 or later): `spawn-<version>-max-macos.zip`. Move the `spawn` folder into `~/Documents/Max 9/Packages/` and restart Max.
- **Pd** (vanilla, tested with 0.57): `spawn-<version>-pd-macos.zip`. Move the `spawn` folder into a folder of Pd's search path, for example `~/Documents/Pd/externals/`.
- **plugdata**: the same Pd zip; move the `spawn` folder into `~/Documents/plugdata/Externals/`.

The help patch opens from the object (right click → Help).

The externals are not signed with an Apple developer certificate, so macOS blocks them the first time. Remove the quarantine flag once, in a terminal, on the folder you moved, for example:

```
xattr -cr ~/Documents/Max\ 9/Packages/spawn
xattr -cr ~/Documents/plugdata/Externals/spawn
```

## Build

Needs the Xcode command line tools and CMake 3.19 or later. For Max, also the Max SDK (Package Manager, `~/Documents/Max 9/Packages/max-sdk`); for Pd, the header `m_pd.h` of Pd 0.57-0 is downloaded at configure time.

```
ln -s ~/Documents/Max\ 9/Packages/max-sdk max-sdk
cmake -S . -B build
cmake --build build
```

The Max external is written to `externals/spawn.mxo` (the repository is a Max package), the Pd external to `pd/spawn.pd_darwin`, next to `pd/spawn-help.pd`. To build only one of them: `-DSPAWN_MAX=OFF` or `-DSPAWN_PD=OFF`.

Tests of the process code, without Max or Pd (`SPAWN_TEST_PYTHON` is the full path of a Python interpreter, for example the one of a conda environment; without it the Python checks are skipped):

```
cmake -S . -B build -DSPAWN_TESTS=ON
cmake --build build
SPAWN_TEST_PYTHON=~/miniforge3/envs/myenv/bin/python build/test_proc
```

## License

MIT, see [LICENSE](LICENSE).
