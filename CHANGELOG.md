# Changelog

## 0.3.0

- Windows 10 and 11, x64: Max external (`spawn.mxe64`) and Pd external (`spawn.dll`). The command runs through `cmd.exe`; `stop` sends ctrl-C and ends the whole command after 3 s.

## 0.2.0 (2026-10-22)

- Pd external for Pd vanilla and plugdata (`spawn.pd_darwin`, universal), with help patch. Same messages and outlets as in Max.
- Process handling moved to `source/core`, shared by Max and Pd.
- Max external: the whole bundle is signed ad hoc.
- Releases now ship two zips: one for Max, one for Pd and plugdata.

## 0.1.0 (2026-10-21)

- First release: Max external for macOS 11 or later, Apple Silicon and Intel.
