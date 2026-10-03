# dosrecomp

`dosrecomp` statically recompiles discovered 8086 code to portable C using the shared decoder and runtime semantics.

The input binary is authoritative. Entry-point metadata may extend discovery but does not replace machine-code behavior.

Usage:

```text
dosrecomp --input file.com --output-c out.c --output-h out.h --symbol md_recomp_name
          [--code-start N] [--code-end N]
          [--entry N]... [--entries FILE]
          [--interp-at N]... [--name NAME] [--dump]
```

Generated code executes supported instructions natively and returns unsupported or invalidated regions to the canonical runtime.

`--entries FILE` adds targets that static direct-flow analysis cannot discover. `--interp-at N` forces a specific instruction through the interpreter.