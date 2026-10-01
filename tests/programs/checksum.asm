; M21 convergence workload: 32 KiB sequential checksum.
; All three optimized tiers can execute the same LODSW / ADD / LOOP region.

bits 16
org  100h

    mov si, 2000h
    mov cx, 4000h
    xor dx, dx
    cld
.loop:
    lodsw
    add dx, ax
    loop .loop
    hlt
