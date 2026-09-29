# dosrecomp

`dosrecomp` will be the source-assisted 8086 static recompiler.

The intended pipeline is:

```text
binary + relocation/source metadata
            |
         decoder
            |
      control-flow graph
            |
   known/unknown target model
            |
      portable C emitter
            |
      normal C compiler
```

The binary is authoritative. Original source and maps/symbols are metadata that improve names, function boundaries, jump-table discovery, data/code classification, and reviewability.

The first compiler milestone will consume a tiny `.COM`, discover its straight-line basic blocks, and mechanically emit the same shape currently demonstrated by `src/generated/hello_recomp.c`. That hand-written file exists only to validate the runtime ABI before the analyzer is written.
