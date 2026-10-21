# spawn

A Max object that runs a command line and outputs what it prints. For example, it starts a Python script that sends data to Max over OSC, and stops it when the patch closes.

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

macOS only for now (Apple Silicon and Intel, macOS 11 or later). Windows and Pd will follow.

## Install

Download `spawn-0.1.0-macos.zip` from [Releases](https://github.com/anatrini/spawn/releases/latest), unzip it, move the `spawn` folder into `~/Documents/Max 9/Packages/` and restart Max. The help patch opens from the object (right click → Open spawn Help).

The external is not signed with an Apple developer certificate, so macOS blocks it the first time. Remove the quarantine flag once, in a terminal:

```
xattr -cr ~/Documents/Max\ 9/Packages/spawn
```

## Build

Needs the Xcode command line tools, CMake 3.19 or later and the Max SDK (Package Manager, `~/Documents/Max 9/Packages/max-sdk`).

```
ln -s ~/Documents/Max\ 9/Packages/max-sdk max-sdk
cmake -S . -B build
cmake --build build
```

The external is written to `externals/spawn.mxo`. Tests of the process code, without Max (`SPAWN_TEST_PYTHON` is the full path of a Python interpreter, for example the one of a conda environment; without it the Python checks are skipped):

```
cmake -S . -B build -DSPAWN_TESTS=ON
cmake --build build
SPAWN_TEST_PYTHON=~/miniforge3/envs/myenv/bin/python build/test_proc
```

## License

MIT, see [LICENSE](LICENSE).
