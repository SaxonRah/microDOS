; xchgself.com - regression for M17: a compiled instruction whose memory store
; hits a compiled code byte must still complete all of its effects before
; compiled code hands over to the interpreter.
; XCHG stores into this program's own first instruction byte (B4h), then must
; also load that old byte into AH. Expected: AH = B4h, byte [0100h] = 42h.
        cpu     8086
        org     100h
        mov     ah, 42h            ; B4 42
        xchg    [0100h], ah        ; 86 26 00 01
        hlt                        ; F4
