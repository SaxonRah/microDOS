# dosprobe

`dosprobe` performs structural recursive-descent analysis of 8086 binaries using the shared decoder.

Examples:

```text
build-host\Release\dosprobe.exe --input COMMAND.COM --base 0x100 --entry 0x100
build-host\Release\dosprobe.exe --input MSDOS.SYS --base 0 --entry 0
```

It reports reachable instructions, control flow, prefixes and current execution-engine coverage.

Use `--json path` for machine-readable output.

Indirect control flow may require additional entry-point metadata. The report describes code reachable from the supplied seeds; it is not proof that every byte in the image is executable code.