# Browsing Mac disks from AmiFM: design options

**Status (2026-11-07): route A is built.** `L:MacHFS-Handler`, `MacMount` and
`MacBrowse ... MOUNT`; usage in the README, the DOSDriver in `docs/HFV0`. Not done from
the list below: the optional ExAll packet (dos.library emulates it), growing the
catalog file, and the final test of mounting a written image in ShapeShifter.

The goal: browse a Mac disk image in AmiFM or in MacBrowse's own window, and add and remove files.

## What AmiFM is

AmiFM is a dual-pane GadTools file manager. It reaches files
only through dos.library path calls (`Lock`, `Examine`/`ExNext`, `Open`, `CreateDir`,
`DeleteFile`, `Rename`). There is no backend or VFS layer. Whatever it can browse has to
look like an AmigaDOS volume.

## Route A: a DOS handler that mounts an image as a volume (recommended)

`L:MacHFS-Handler` is mounted with a command such as `MacMount HFV0: MacDisks:MacHD.hfv`
(or `MacBrowse <image> MOUNT HFV0:`). The image then appears as `HFV0:` with the Mac volume
name.

* **Why recommended.** AmiFM works unchanged, and so do Directory Opus, the Shell,
  Workbench, `Copy` and scripts. The file system exerciser `AmiFSX` can test it.
* **Code reuse.** It reuses `src/hfs.c` (catalog/extents B-trees, insert/delete, bitmap,
  MDB, CHECK) and `charset.c`.
* **Packets.** LOCATE_OBJECT, FREE_LOCK, COPY_DIR, PARENT, EXAMINE_OBJECT, EXAMINE_NEXT,
  FINDINPUT, FINDOUTPUT, FINDUPDATE, READ, WRITE, SEEK, SET_FILE_SIZE, END, DELETE_OBJECT,
  CREATE_DIR, RENAME_OBJECT, SET_DATE, SET_COMMENT, SET_PROTECT (accepted, no effect),
  INFO, DISK_INFO, CURRENT_VOLUME, SAME_LOCK, IS_FILESYSTEM, FLUSH, INHIBIT and DIE.
  The OS 3.x ExAll packet is optional, because dos.library emulates it.
* **Locks and Examine.**
  * A lock key is the CNID.
  * `Examine` returns the Latin-1 name, with `/` mapped to a stand-in. The size is the
    data-fork size and the date is the Mac modification date.
  * The comment is `TYPE/CRTR`. Writing a comment of that form with `SetComment` sets
    type/creator, which is how a file manager can retype a file.
* **Resource forks.** They appear as virtual AppleDouble `._name` files: read-write,
  and hideable with a mount flag. This makes AmiFM copies lossless both ways. Copying a
  `name` + `._name` pair onto the volume rebuilds both forks and the Finder info.
* **Missing pieces in `hfs.c`.**
  * Streaming writes: allocate a clump at a time and add extents-overflow records when
    a fork needs more than three extents.
  * Rename and move: delete and re-insert the record, and fix the folder thread records.
  * Growing the catalog file when it is full (optional; until then, report "disk full").
* **Safety (ShapeShifter).**
  * Mounting takes an exclusive lock on the image file for as long as it is mounted.
    ShapeShifter therefore cannot open it at the same time, and the mount fails if
    ShapeShifter already has it open.
  * Mounting is refused if the MDB is not marked as cleanly unmounted (a Mac has it).
    A read-only mount (`RO`) is allowed.
  * While the volume is dirty, the handler clears the MDB flag. After writes it flushes
    the MDB, bitmap and both MDB copies about 2 s after the volume goes idle, sets the
    flag again, and does the same on INHIBIT and DIE.
* **Testing.** Handlers cannot run under vamos. Test in Copperline, scripted from the
  Shell: Copy, List, Delete, AmiFSX on HFV0:, then MacBrowse CHECK and hfsls.py on the
  image from the host. The final test is mounting the result in ShapeShifter.
* **Size.** About 1,500 lines on top of hfs.c. It is the bigger job, but it is the only
  route that makes every Amiga tool work.

## Route B: an HFS backend inside AmiFM

Add a pane-backend interface to AmiFM (list, stat, open, read, write, mkdir, delete,
rename), with one implementation over dos.library and one over `hfs.c`.

* Only AmiFM benefits.
* All of AmiFM's roughly 24 dos.library call sites and its copy, move and find code
  have to go through the new layer. It is a sizeable change to a released tool.
* The streaming-write and rename work in `hfs.c` is needed anyway.

## Its own file manager

MacBrowse's GadTools window already browses, copies in and out (choosing the format),
creates folders and deletes. It is the zero-install option and also covers images that
cannot be mounted (read-only, or not cleanly unmounted). It could later become
dual-pane, with an Amiga drawer on one side, but with Route A in place AmiFM already
gives that.

## Recommendation

Do Route A and keep the MacBrowse window as the lightweight tool.

1. In `hfs.c`: streaming allocation with extents-overflow writes, then rename/move.
   Both are testable on the host and under vamos with the existing test suite.
2. The handler itself, read-only first, then writes, tested in Copperline with AmiFSX.
3. A `MacMount` command, or a `MOUNT` command in MacBrowse, and a mountlist example.
