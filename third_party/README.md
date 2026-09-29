# Third-party reference trees

Third-party source is fetched locally and is not copied into the microDOS repository.

## Microsoft MS-DOS 2.0

Install the pinned reference checkout with:

```powershell
.\md.bat deps msdos
```

microDOS pins:

```text
repository: https://github.com/microsoft/MS-DOS.git
commit:     2d04cacc5322951f187bb17e017c12920ac8ebe2
```

The bring-up currently consumes:

```text
third_party/msdos/v2.0/bin/MSDOS.SYS
third_party/msdos/v2.0/bin/COMMAND.COM
third_party/msdos/v2.0/source/
```

At the pinned commit the official Git blobs are:

```text
MSDOS.SYS    803eb7004b3e3d0093d955565beff7ec39f5b8c4    16690 bytes
COMMAND.COM  820f39322f00e0cecd52568ac5505b8d1611b552    15480 bytes
```

Microsoft's repository carries its own license. Keep that upstream license with any
redistribution of upstream material. microDOS's own source remains under its root
`LICENSE`.
