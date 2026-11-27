# Interactions

How a user drives npkextpp without typing options: what happens on a
double-click, which defaults stand in for missing options, and the keys and
mouse during a run. Notes for the full docs to come.

## Defaults when options are missing

| missing | assumed |
|---|---|
| `--install` | the current folder, if it is an installation (has `neox_bin/`, or `res/resdb` and `Documents/`) |
| `--install` and `--in` | the current folder is searched for archives and name manifests, whatever it is |
| `--out` | `./unpacked` |
| `--resdb` | every name manifest the search found |

On a double-click, the current folder is the folder the exe is in, so the exe
works when it is put into the game's folder.

## No arguments at all (a double-click)

1. **Search.** The current folder is walked: the installation's scan when the
   folder is one, the same walk otherwise. A live counter shows how far it got:

   ```
   Scanning      C:\Games\Magic Awakened
   Archives found (.npk): 321
   Resource manifests found (resdb): 2
   ```

   When it ends, `Scanning` becomes `Installation` (or `Folder` when the folder
   is not an installation), and the counts stay on screen, with the folders
   skipped for their `.noscan` file. Folders that cannot be opened are named in
   a warning.
2. **Question.**

   ```
   Output        C:\Games\Magic Awakened\unpacked
   Do you want to unpack all? (Y/n)
   ```

   Enter, `y` or `yes` unpacks everything that was found; `n` or `no` ends with
   "Nothing was unpacked." Anything else asks again. When there is no input
   to answer from (closed, or redirected from an empty file), the answer is no.
3. **Unpacking**, as with `--install`: names, plan, disk test, live progress, summary.
4. **The window stays open** at the end, also after an error, with
   "Press Enter to close." That happens on Windows only when the console window
   belongs to npkextpp alone (a double-click), never when it was started
   from cmd or PowerShell.

Nothing found: the run ends with an error that says where to put the exe.

With any argument given there is no question: the options say what to do. The
defaults above still apply.

## What the search leaves out

- **Folders marked `.noscan`:** a folder that holds a file named `.noscan` is
  left out with all of its subfolders, recursively. npkextpp writes one into
  every folder it unpacks into (a full unpack or `--entry`), so the output,
  wherever it is, is never searched through again: a second run doesn't walk the
  half a million files of the first. The file says the same in a few lines.
  Delete it to have the folder searched again; put one anywhere to keep the
  search out. The counter lists the skipped folders as it finds them:

  ```
  Skipped (.noscan): 1 folder: unpacked
  ```

  When the searched folder itself is marked, nothing is found, and the error
  says so.
- **Symbolic links.**
- **Manifests in name only:** a file counts as a manifest only when it is named
  `resdb` and starts like one (`CCZ!`).

## During a run

- **Mouse wheel**, **↑ ↓**, **PgUp PgDn**, **Home End:** scroll the archive list
  when it doesn't fit the window.
- **F:** the list follows the archive being unpacked again.
- **Shift + drag:** selects text (a plain drag doesn't while the wheel scrolls the list).
- **Ctrl+C:** stops cleanly. What was written stays, and the next run continues from there.

Keys and the wheel work in Linux and macOS terminals and in the Windows console
and Windows Terminal. The Windows side has not been tested yet.

## Platforms

- **Double-click:** on Windows, Explorer opens a console window and starts the
  exe in its own folder. Linux file managers mostly don't run a console program
  on a double-click; there it is started from a terminal (`./npkextpp`),
  and the same flow follows.
- **Dropping files or folders onto the exe (not built yet):** Windows starts the
  exe with the dropped paths as arguments. Linux file managers don't, unless a
  `.desktop` launcher is set up. macOS needs an app bundle.
- **Dropping a file onto an open console window** types its path, in every
  system's terminal, so a question that asks for a path works everywhere.
