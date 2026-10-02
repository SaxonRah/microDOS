; MDSTRESS.COM v2 - microDOS engine stress/demo workload
; NASM syntax: nasm -f bin MDSTRESS.ASM -o MDSTRESS.COM
;
; Modes:
;   MDSTRESS       all nine phases, automatic
;   MDSTRESS /S    all nine phases, pause at each READY boundary
;   MDSTRESS 1..9  run exactly one phase and return to COMMAND.COM
;
; The prebuilt COM is checked in so the normal microDOS build does not require
; NASM. This source documents the workload and is intended to remain the
; editable canonical assembly.
bits 16
org 100h

jmp start

puts:
    mov ah,09h
    int 21h
    ret
newline:
    mov dx,msg_crlf
    call puts
    ret
wait_step:
    cmp byte [step_mode],0
    je .ret
    mov dx,msg_ready
    call puts
    mov ah,08h
    int 21h
    call newline
.ret:
    ret

print_hex16:
    push ax
    push bx
    push cx
    push dx
    mov bx,ax
    mov cx,4
.loop:
    rol bx,1
    rol bx,1
    rol bx,1
    rol bx,1
    mov dl,bl
    and dl,0Fh
    cmp dl,9
    jbe .digit
    add dl,7
.digit:
    add dl,'0'
    mov ah,02h
    int 21h
    loop .loop
    pop dx
    pop cx
    pop bx
    pop ax
    ret

print_cs:
    mov dx,msg_cs
    call puts
    push cs
    pop ax
    call print_hex16
    call newline
    ret

proc_a:
    push ax
    push bx
    push dx
    call proc_b
    pop dx
    pop bx
    pop ax
    inc ax
    xor bx,ax
    ret
proc_b:
    push si
    push di
    call proc_c
    xchg si,di
    pop di
    pop si
    ret
proc_c:
    add ax,1
    adc bx,ax
    xor si,bx
    rol di,1
    ret

start:
    push cs
    pop ds
    push cs
    pop es
    cld
    mov byte [step_mode],0
    mov byte [single_phase],0
    mov byte [error_flag],0
    mov word [checksum],1357h

    ; COMMAND.COM supplies a leading blank in the PSP command tail.
    cmp byte [80h],2
    jb .mode_done
    mov al,[82h]
    cmp al,'/'
    je .slash
    cmp al,'1'
    jb .mode_done
    cmp al,'9'
    ja .mode_done
    sub al,'0'
    mov [single_phase],al
    jmp .mode_done
.slash:
    cmp byte [80h],3
    jb .mode_done
    mov al,[83h]
    and al,0DFh
    cmp al,'S'
    jne .mode_done
    mov byte [step_mode],1
.mode_done:
    mov dx,msg_banner
    call puts
    call print_cs
    cmp byte [single_phase],0
    je .not_single
    mov dx,msg_single
    call puts
    mov al,[single_phase]
    add al,'0'
    mov dl,al
    mov ah,02h
    int 21h
    call newline
    jmp phase1_gate
.not_single:
    cmp byte [step_mode],0
    je .auto
    mov dx,msg_step
    call puts
    jmp phase1_gate
.auto:
    mov dx,msg_auto
    call puts

; Each gate implements:
;   if single_phase==0 || single_phase==N: run phase
;   else skip to next gate
; After a phase, single-phase mode jumps directly to finish.

phase1_gate:
    cmp byte [single_phase],0
    je phase1
    cmp byte [single_phase],1
    je phase1
    jmp phase2_gate
phase1:
    mov dx,msg_p1
    call puts
    call wait_step
    mov bp,8
    mov ax,1357h
    mov bx,2468h
    mov si,0ACE1h
    mov di,1D0Fh
.outer:
    mov cx,8000h
.inner:
    add ax,bx
    adc si,ax
    xor bx,si
    sub di,bx
    rol ax,1
    ror bx,1
    test si,1
    jz .noinc
    inc di
.noinc:
    dec cx
    jnz .inner
    dec bp
    jnz .outer
    mov ax,[checksum]
    xor ax,bx
    xor ax,si
    xor ax,di
    mov [checksum],ax
    cmp byte [single_phase],0
    je phase2_gate
    jmp finish

phase2_gate:
    cmp byte [single_phase],0
    je phase2
    cmp byte [single_phase],2
    je phase2
    jmp phase3_gate
phase2:
    mov dx,msg_p2
    call puts
    call wait_step
    mov bp,64
    mov bx,buffer1
    mov ax,3141h
.outer:
    xor si,si
    mov cx,1024
.fill:
    add ax,9E37h
    xor ax,si
    mov [bx+si],ax
    add si,2
    loop .fill
    xor si,si
    xor dx,dx
    mov cx,1024
.read:
    mov ax,[bx+si]
    add dx,ax
    xor dx,si
    add si,2
    loop .read
    dec bp
    jnz .outer
    mov ax,[checksum]
    xor ax,dx
    mov [checksum],ax
    cmp byte [single_phase],0
    je phase3_gate
    jmp finish

phase3_gate:
    cmp byte [single_phase],0
    je phase3
    cmp byte [single_phase],3
    je phase3
    jmp phase4_gate
phase3:
    mov dx,msg_p3
    call puts
    call wait_step
    push ds
    pop es
    cld
    mov bp,32
.outer:
    mov ax,bp
    mov di,buffer1
    mov cx,2048
    rep stosw
    mov si,buffer1
    mov di,buffer2
    mov cx,2048
    rep movsw
    mov si,buffer1
    mov di,buffer2
    mov cx,2048
    repe cmpsw
    mov ax,0FFFFh
    mov di,buffer2
    mov cx,2048
    repne scasw
    dec bp
    jnz .outer
    mov ax,[checksum]
    xor ax,di
    mov [checksum],ax
    cmp byte [single_phase],0
    je phase4_gate
    jmp finish

phase4_gate:
    cmp byte [single_phase],0
    je phase4
    cmp byte [single_phase],4
    je phase4
    jmp phase5_gate
phase4:
    mov dx,msg_p4
    call puts
    call wait_step
    mov bp,4
    mov ax,1234h
    mov bx,5678h
    mov si,9ABCh
    mov di,0DEF0h
.outer:
    xor cx,cx
.inner:
    call proc_a
    loop .inner
    dec bp
    jnz .outer
    mov ax,[checksum]
    xor ax,bx
    xor ax,si
    xor ax,di
    mov [checksum],ax
    cmp byte [single_phase],0
    je phase5_gate
    jmp finish

phase5_gate:
    cmp byte [single_phase],0
    je phase5
    cmp byte [single_phase],5
    je phase5
    jmp phase6_gate
phase5:
    mov dx,msg_p5
    call puts
    call wait_step
    mov bp,8
    mov ax,1234h
    mov bx,251
    mov di,257
.outer:
    mov cx,4000h
.inner:
    xor dx,dx
    mul bx
    div di
    xor ax,dx
    add ax,1357h
    rol ax,1
    ror dx,1
    loop .inner
    dec bp
    jnz .outer
    mov [tmp_word],ax
    mov ax,[checksum]
    mov dx,[tmp_word]
    xor ax,dx
    mov [checksum],ax
    cmp byte [single_phase],0
    je phase6_gate
    jmp finish

phase6_gate:
    cmp byte [single_phase],0
    je phase6
    cmp byte [single_phase],6
    je phase6
    jmp phase7_gate
phase6:
    mov dx,msg_p6
    call puts
    call wait_step
    mov bp,8
    mov ax,0ACE1h
    mov dx,1D0Fh
.outer:
    mov cx,8000h
.inner:
    test ax,1
    jz .even
    shr ax,1
    xor ax,0B400h
    jmp short .merge
.even:
    ror ax,1
    adc dx,ax
.merge:
    cmp ax,8000h
    jb .low
    not dx
.low:
    test dx,0100h
    jz .noneg
    neg dx
.noneg:
    add ax,dx
    loop .inner
    dec bp
    jnz .outer
    mov [tmp_word],ax
    mov ax,[checksum]
    mov dx,[tmp_word]
    xor ax,dx
    mov [checksum],ax
    cmp byte [single_phase],0
    je phase7_gate
    jmp finish

phase7_gate:
    cmp byte [single_phase],0
    je phase7
    cmp byte [single_phase],7
    je phase7
    jmp phase8_gate
phase7:
    mov dx,msg_p7
    call puts
    call wait_step
    mov bp,8
    mov ax,1234h
    mov bx,0ABCDh
    mov si,55AAh
.outer:
    mov cx,8000h
.inner:
    xchg ax,bx
    pushf
    lahf
    xor ah,5Ah
    sahf
    popf
    not ax
    neg bx
    cbw
    cwd
    xchg dx,si
    inc ax
    dec bx
    loop .inner
    dec bp
    jnz .outer
    mov ax,[checksum]
    xor ax,bx
    xor ax,si
    xor ax,dx
    mov [checksum],ax
    cmp byte [single_phase],0
    je phase8_gate
    jmp finish

phase8_gate:
    cmp byte [single_phase],0
    je phase8
    cmp byte [single_phase],8
    je phase8
    jmp phase9_gate
phase8:
    mov dx,msg_p8
    call puts
    call wait_step
    mov dx,tmp_name
    xor cx,cx
    mov ah,3Ch
    int 21h
    jnc .created
    mov byte [error_flag],1
    jmp .mem
.created:
    mov [file_handle],ax
    mov bp,8
.write:
    mov bx,[file_handle]
    mov dx,buffer1
    mov cx,4096
    mov ah,40h
    int 21h
    jc .file_fail
    dec bp
    jnz .write
    mov bx,[file_handle]
    xor cx,cx
    xor dx,dx
    mov ax,4200h
    int 21h
    jc .file_fail
    mov bp,8
.read:
    mov bx,[file_handle]
    mov dx,buffer2
    mov cx,4096
    mov ah,3Fh
    int 21h
    jc .file_fail
    dec bp
    jnz .read
    jmp .close
.file_fail:
    mov byte [error_flag],1
.close:
    mov bx,[file_handle]
    mov ah,3Eh
    int 21h
    mov dx,tmp_name
    mov ah,41h
    int 21h
.mem:
    mov bp,64
.mem_loop:
    mov bx,0040h
    mov ah,48h
    int 21h
    jc .mem_skip
    mov es,ax
    mov ah,49h
    int 21h
    push ds
    pop es
.mem_skip:
    dec bp
    jnz .mem_loop
    mov si,buffer2
    mov cx,1024
    xor dx,dx
.sum:
    lodsw
    add dx,ax
    loop .sum
    mov ax,[checksum]
    xor ax,dx
    mov [checksum],ax
    cmp byte [single_phase],0
    je phase9_gate
    jmp finish

phase9_gate:
    cmp byte [single_phase],0
    je phase9
    cmp byte [single_phase],9
    je phase9
    jmp finish
phase9:
    mov dx,msg_p9
    call puts
    call wait_step
    mov cx,4096
    xor bx,bx
.loop:
    mov ax,cx
    mov [selfmod_target+1],ax
    call selfmod_target
    add bx,ax
    loop .loop
    mov ax,[checksum]
    xor ax,bx
    mov [checksum],ax

finish:
    mov dx,msg_done
    call puts
    mov ax,[checksum]
    call print_hex16
    call newline
    cmp byte [error_flag],0
    je .ok
    mov dx,msg_warn
    call puts
.ok:
    mov dx,msg_exit
    call puts
    mov ax,4C00h
    int 21h

msg_crlf   db 13,10,'$'
msg_banner db 13,10,'microDOS ENGINE STRESS LAB v2',13,10
           db 'Deterministic workload for interpreter/cache/AOT/JIT profiling.',13,10
           db 'Modes: MDSTRESS | MDSTRESS /S | MDSTRESS 1..9',13,10,'$'
msg_cs     db '[MDSTRESS] CS=0x$'
msg_single db '[MDSTRESS] single phase = $'
msg_step   db 'STEP mode: Ctrl+] at READY, then ENTER.',13,10,'$'
msg_auto   db 'AUTO mode: running all phases.',13,10,'$'
msg_ready  db '  [MDSTRESS] READY - Ctrl+] for stats, then press ENTER',13,10,'$'
msg_p1     db 13,10,'[01/09] ALU + FLAGS + SHIFT MIXER',13,10,'$'
msg_p2     db 13,10,'[02/09] MEMORY ADDRESS + STORE FABRIC',13,10,'$'
msg_p3     db 13,10,'[03/09] REP STRING DMA / COMPARE / SCAN',13,10,'$'
msg_p4     db 13,10,'[04/09] CALL/RET + STACK CONTROL GRAPH',13,10,'$'
msg_p5     db 13,10,'[05/09] INTEGER MUL/DIV + ROTATE PIPELINE',13,10,'$'
msg_p6     db 13,10,'[06/09] CONDITIONAL BRANCH / LFSR FABRIC',13,10,'$'
msg_p7     db 13,10,'[07/09] RARE 8086 OPCODE COMPATIBILITY SWEEP',13,10,'$'
msg_p8     db 13,10,'[08/09] DOS INT 21h FILE + MEMORY SERVICE LOAD',13,10,'$'
msg_p9     db 13,10,'[09/09] SELF-MODIFYING CODE / CACHE INVALIDATION',13,10,'$'
msg_done   db 13,10,'MDSTRESS complete. deterministic checksum = 0x$'
msg_warn   db 'WARNING: DOS file phase reported an error.',13,10,'$'
msg_exit   db 'Return to COMMAND.COM.',13,10,'$'
tmp_name   db 'MDSTRS.TMP',0
step_mode  db 0
single_phase db 0
error_flag db 0
file_handle dw 0
checksum   dw 0
tmp_word   dw 0

align 16
buffer1 times 4096 db 0
buffer2 times 4096 db 0

align 16
selfmod_target:
    mov ax,0000h
    ret
