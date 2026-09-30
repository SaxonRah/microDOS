; memloop.com - memory-bound benchmark / differential fixture (M15).
; Read-modify-write over a 32 KiB window (8000h-FFFFh) with a 97-byte stride,
; larger than the RP2350's 16 KiB XIP cache. Never touches its own code.
; 2 setup + 32768 iterations x 7 + HLT = 229379 guest instructions.
        cpu     8086
        org     100h
        mov     cx, 8000h
        mov     si, 8000h
top:    mov     al, [si]
        add     al, 3
        mov     [si], al
        add     si, 97
        or      si, 8000h
        dec     cx
        jnz     top
        hlt
