; branchmix.com - M20.1 bounded-CFG resident-region benchmark
; 8086, ORG 100h.  The JZ alternates taken/not-taken and reconverges before
; the final DEC CX/JNZ backedge.  Final state: AX=0000h BX=C000h CX=0000h.
; 212,995 instructions before HLT, 212,996 including HLT.
        cpu 8086
        org 100h
        mov cx,8000h
        mov ax,0
        mov bx,0
.top:
        xor ax,1
        cmp ax,0
        jz .skip_add
        add bx,3
.skip_add:
        or  bx,0              ; deterministic CF=0 before DEC
        dec cx
        jnz .top
        hlt
