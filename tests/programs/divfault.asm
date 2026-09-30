; divfault.com - M18 regression: DIV/MUL run through the shared interpreter
; core from compiled code. A divide by zero must stop the machine exactly as
; the interpreter does (MD_STOP_FAULT, same IP, same instruction count).
        cpu     8086
        org     100h
        mov     ax, 1234
        mov     bl, 10
        mul     bl              ; AX = AL*BL = 0D2h*10 = 2100
        mov     cx, 7
        xor     dx, dx
        div     cx              ; AX = 300, DX = 0
        xor     bl, bl
        div     bl              ; divide by zero -> fault
        hlt
