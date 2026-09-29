# mkfat12

`mkfat12` builds the deterministic 360 KiB FAT12 image used by the MS-DOS 2.0
system bring-up path.

It does not replace DOS filesystem code. It only constructs a block image that the
native DOS 2 block-device shim exposes as sectors. `MSDOS.SYS` still performs FAT,
root-directory, handle, and file reads itself.

Current image geometry:

```text
512 bytes/sector
2 sectors/cluster
1 reserved sector
2 FATs
2 sectors/FAT
112 root entries
720 total sectors
FDh media byte
```

The image contains the pinned released `COMMAND.COM` as `COMMAND.COM` beginning at
cluster 2. Additional files can be placed after it:

```
mkfat12 --command COMMAND.COM --add tests\dos2\DOS2TEST.COM DOS2TEST.COM --output msdos2.img
```

`md.bat image dos2` adds `tests\dos2\DOS2TEST.COM` when it exists.

Every directory entry is stamped 1983-03-08 12:00 so the image stays
deterministic. (DOS 2 `DIR` omits the date and time columns entirely when the
date word is zero.)

Normally invoke it through:

```powershell
.\md.bat image dos2
```

or let:

```powershell
.\md.bat run dos2
```

rebuild the image automatically.
