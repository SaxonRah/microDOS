; callmix.com - M20.2 native CALL/RET boundary benchmark.
; 2 setup + 32768 * (CALL, ADD, RET, DEC, JNZ) + HLT = 163843 instructions.
        cpu     8086
        org     100h
        mov     cx, 8000h
        mov     bx, 0
loop:   call    subr
        dec     cx
        jnz     loop
        hlt
subr:   add     bx, 3
        ret
