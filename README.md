# MacBrowse

Browse Mac HFS disk images from AmigaOS and copy files in both directions, without
running ShapeShifter. MacBrowse opens the image files directly as ordinary AmigaDOS files.

It is a dual-mode tool. It works as a CLI command, and when started from its icon (or as
`MacBrowse <image>`) it opens a GadTools browser window.

## What it reads

* Bare HFS volume images, such as ShapeShifter "file disks" (`.hfv`), with the Master
  Directory Block at byte 1024. ShapeShifter makes these with "Create...".
* Images that start with an **Apple partition map**: hardfiles and CD images with
  512- or 2048-byte blocks. The first `Apple_HFS` partition is used, or pick another
  with `PART n` (see `PARTS`).
* DiskCopy 4.2 images (84-byte header).

It walks the catalog B-tree and the extents-overflow B-tree, so fragmented files and
fragmented catalogs come out complete. HFS+ volumes, including HFS+ inside an HFS
wrapper, are not supported.

## CLI

```
MacBrowse <image> <command> [args] [options]

INFO                     volume information (and whether it is cleanly unmounted)
PARTS                    list the Apple partition map
LIST [path] [ALL]        list a folder (ALL = recursive)
LS [DEPTH n]             flat list of every file (same format as hfsls.py)
GET <path..> [TO dir|file] [MODE DATA|RSRC|MACBIN|DOUBLE|SINGLE]
PUT <file|dir..> [TO folder] [AS name] [TYPE t] [CREATOR c] [PLAIN] [REPLACE]
MKDIR <path>             create a folder
DELETE <path..>          delete files or empty folders
RENAME <path> <newpath>  rename or move (into newpath if it is a folder; ":" = the volume)
CHECK                    verify B-trees, node maps, bitmap and MDB counts
CREATE <MB> [NAME name]  write a new, empty image (FORCE: over an old one)
SYNCIN <drawer> [FREE mb] [NAME name]   make the image match an AmigaDOS drawer
SYNCOUT <drawer>         bring what changed on the Mac back to the drawer
MOUNT <device:> [RO] [NODOUBLE]   mount as an AmigaDOS volume (see below)
Options: PART n  RAW  FORCE  QUIET  VERBOSE   (-- ends the options)
```

* Mac paths use `:`, as in `"Data:Mission 200"`. The volume name may be given first.
  Wildcards `*`, `?` and `#?` work in any path part.
* Names are shown in ISO-8859-1. Mac Roman characters that have no Latin-1 equivalent
  are shown as an ASCII stand-in (`ƒ` is shown as `f`), and you can type them that way
  in paths. `RAW` prints the Mac Roman bytes instead.
* `GET` copies a file or a whole folder. The formats are:
  * `DATA` (the default): the data fork only.
  * `RSRC`: the resource fork only.
  * `MACBIN`: MacBinary II, written as `name.bin`.
  * `DOUBLE`: AppleDouble, with the data fork in `name` and the resource fork plus
    Finder info in `._name`.
  * `SINGLE`: AppleSingle.

  Each copied file gets its Mac modification date and a `TYPE/CRTR` file comment.
* `PUT` detects MacBinary I/II/III, AppleSingle/AppleDouble and BinHex 4.0 and decodes
  them to the real forks and type/creator. For a plain file it also picks up a
  `._name` AppleDouble sidecar next to it. `PLAIN` turns this detection off. Without
  any metadata, the type/creator comes from the file extension (`.txt` gives
  `TEXT/ttxt`, `.sit` gives `SIT!/SIT!`, and so on); otherwise it is `????`.
  StuffIt archives are copied in unchanged, for StuffIt Expander on the Mac.

Examples:

```
MacBrowse MacDisks:MacHD.hfv LIST ALL
MacBrowse MacDisks:MacHD.hfv GET "Applications:SimpleText" TO RAM: MODE MACBIN
MacBrowse MacDisks:Transfer.hfv PUT RAM:Download/#? TO "Incoming"
```

### A synced drawer

`SYNCIN` and `SYNCOUT` keep an image and an AmigaDOS drawer the same, so that a drawer
of the Amiga shows up as a disk on the Mac desktop. ShapeShifter 3.20 runs them by itself
for its "synced drawer"; by hand it is:

```
MacBrowse Work:Shared.hfv SYNCIN Work:Shared NAME Amiga   ; before the Mac starts
... use the disk "Amiga" on the Mac, shut the Mac down ...
MacBrowse Work:Shared.hfv SYNCOUT Work:Shared              ; afterwards
```

* `SYNCIN` creates the image when there is none, sized for the drawer plus `FREE`
  megabytes (32 by default), and makes a new, larger one when the drawer has outgrown
  it or when the Mac did not shut the old one down cleanly. Before it changes the image
  it first does what `SYNCOUT` does, so a Mac session that ended without a sync loses
  nothing.
* What both sides looked like after the last sync is kept in `<image>.sync`. A file that
  changed on one side is copied to the other. One that changed on both sides is kept
  twice: the Mac's version appears in the drawer as `name (Mac)`. A file deleted on one
  side is deleted on the other, unless it was changed there in the meantime.
* Files go to the Mac as they are (no MacBinary or BinHex decoding); the type and creator
  come from the extension. A Mac file with a resource fork, or with a type the extension
  does not give, comes back with an AppleDouble sidecar `._name`, which `SYNCIN` uses to
  put the file back complete.
* Left out: `.info` icons, the sidecars as items of their own, AmigaDOS names longer
  than 31 characters (with a warning), and the Mac's invisible items (Desktop files,
  Trash). The Mac's icon positions and window settings therefore do not survive a
  rebuilt image.
* Two safety rules for deleting in the drawer: nothing is deleted when the image is not
  the one of the last sync (erased on the Mac, or replaced), and nothing is deleted when
  more than half of the files (and more than ten) are missing on the Mac. The next
  `SYNCIN` then puts the files on the Mac again. `FORCE` switches both rules off.
* An image remembers its drawer. `SYNCIN` with another drawer makes the image anew
  without bringing anything over; `SYNCOUT` into another drawer is refused.
* The sync is not live: it runs while the Mac is not using the image. An image and its
  drawer hold up to 2 GB.

The return codes are 0 on success, 5 for a break (Ctrl-C), 10 if some items failed and
20 if the image could not be opened.

## Window

Double-click the icon, or shift-select an image icon and then double-click MacBrowse.
You can also run `MacBrowse <image>` from the Shell, or set the `IMAGE=` tooltype.

The window shows the current Mac folder, with folders listed first. Double-click a
folder to open it. Backspace or **Parent** goes up. **Copy out...** copies the
selected file or folder to a drawer in the format chosen in *Copy out as*. **Copy
in...** takes several files or drawers. There are also **New folder...** and
**Delete** buttons.

## Mounting an image as a volume (MacHFS-Handler)

`L:MacHFS-Handler` makes a Mac disk image an ordinary AmigaDOS volume. AmiFM, Directory
Opus, Workbench, the Shell and every other program can then browse it and copy files
in and out.

```
MacMount HFV0: MacDisks:Transfer.hfv        read-write
MacMount HFV1: MacDisks:MacOS80.hfv RO      read-only
MacMount HFV0: UNMOUNT
MacBrowse MacDisks:Transfer.hfv MOUNT HFV0: [RO] [NODOUBLE]   (the same, from MacBrowse)
```

The volume gets the Mac volume name, and a disk icon appears on Workbench. MacMount
template: `DEVICE/A,IMAGE,RO=READONLY/S,NODOUBLE/S,PART/K/N,HANDLER/K,LOG/K,STACK/K/N,UNMOUNT=DISMOUNT/S,QUIET/S`.
You can also mount it with a DOSDriver: see `docs/HFV0`, which also documents the
handler's `Startup` options.

On the volume:

* Folders are drawers. A file shows its data fork, with the Mac modification date. The
  Finder type and creator appear as the file comment, as in `APPL/ttxt`. Setting a
  comment of that form (`Filenote file "TEXT/ttxt"`) changes them. A new file gets its
  type from its extension, as with `PUT`.
* A file that has a resource fork is listed together with a virtual `._name`: an
  AppleDouble file holding the resource fork and the Finder info. Copying `name` and
  `._name` off the volume and back again is therefore lossless, with any program, in
  either order. Writing `._name` sets the resource fork and Finder info of `name`
  (and creates `name` if it is not there yet). Deleting `._name` empties the resource
  fork. Deleting `name` deletes both forks. Mount with `NODOUBLE` to hide `._name`.
* Rename and move work for files and folders. `Relabel` renames the Mac volume (27
  characters at most). `SetProtect` is accepted but has no effect. A Mac file that is
  locked in the Finder shows as not writable or deletable.
* Names are converted as in the CLI. Mac names are at most 31 characters long.

Safety:

* The handler holds an exclusive lock on the image file for as long as it is mounted,
  so ShapeShifter cannot open it at the same time, and the mount fails while
  ShapeShifter has it open. MacMount also refuses an image that another MacHFS device
  has mounted already, because some emulator host-folder drives do not enforce
  exclusive locks.
* A read-write mount is refused unless the volume is marked as cleanly unmounted.
  `RO` mounts it anyway, read-only.
* While files change, the volume is marked dirty. About 2 seconds after the last
  change, the handler writes the bitmap and both MDB copies and marks it clean again.
  It does the same for `ACTION_FLUSH`, Inhibit and unmount. If the Amiga crashes in
  the middle of a copy, the Mac therefore checks the disk the next time.
* `MacMount <device:> UNMOUNT` refuses while files or drawers on the volume are in use.

## Writing safely

Write commands (`PUT`, `MKDIR`, `DELETE`, and the window's copy-in, new-folder and
delete) are refused in two cases:

* if anything else has the image file open. MacBrowse takes an exclusive lock on it,
  so the write also fails while ShapeShifter is using the image.
* if the volume is not marked as cleanly unmounted. A Mac clears that flag while it
  has the disk mounted. Quit ShapeShifter first. `FORCE` overrides this check (CLI only).

While MacBrowse writes, it clears the flag, and it sets it again when it finishes. If
a write is interrupted, the Mac therefore checks the disk the next time it mounts it.

Write support covers files and folders: B-tree insertion with node splits and new
roots, deletion, and rename/move (including the folder thread records). A fork grows
a clump at a time from the volume bitmap. After three extents it continues in the
extents-overflow B-tree, so fragmented free space is no problem. Both MDB copies are
updated. MacBrowse does **not** grow the catalog or extents file. If one of them is
full, you get "catalog is full" (or "B-tree ... is full") and must make room on the
Mac. Run `CHECK` after copying if in doubt.

## Building and testing

The Amiga binary is built with amiga-gcc in WSL. A native host build is used for fast tests.

```
make            # out/MacBrowse   (m68k AmigaOS, libnix, runs with a 4 KB stack)
make host       # out/macbrowse-host
make icon       # out/MacBrowse.info  (tools/mkicon.py)
make handler    # out/MacHFS-Handler (linked with -nostartfiles: src/hstart.s, src/hlibc.c)
make out/MacMount
make forktest   # out/forktest-host and out/forktest (m68k, for vamos)
```

The tests need Python on Windows and the reference scripts in
`C:\projects\tools\shapeshifter`. They only read the real images, and they write only
to copies.

```
python test/compare.py [--vamos] IMAGE...         # LS vs test/ref/hfsls.py, GET vs hfsget.py byte-for-byte
python test/writetest.py [--vamos] IMAGE WORKDIR  # write tests on a copy, CHECK after each step
python test/fragtest.py [--vamos] IMAGE WORKDIR   # overflow extents, disk full, RENAME (a small, nearly full image)
out/forktest-host COPY.hfv [SEED [OPS [FILES [MAXLEN]]]]   # random fork I/O against a model
```

The handler cannot run under vamos. It was tested in Copperline (A1200, OS 3.2.3) with
scripted Shell commands (Copy, List, Rename, Delete, Filenote, Relabel, Mount), AmiFSX
(3000 operations with a second handle, plus 2000 more), and AmiFM driven by scripted
clicks. The images were checked from the host afterwards with `CHECK` and `hfsls.py`.

`--vamos` runs the real m68k binary under vamos with a 4 KB stack instead of the
host build.

## Licence

GPL-3.0, see `LICENSE`.
