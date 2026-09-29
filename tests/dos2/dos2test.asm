; ============================================================================
; DOS2TEST.COM - MS-DOS 2.0 service conformance test for microDOS
;
; Run at the A> prompt:   A>DOS2TEST
;
; Every test is a subroutine that returns CF=0 (pass) or CF=1 with AX = the
; DOS error code / a small test-specific code (fail). The runner prints one
; line per test, a summary, and exits through INT 21h AH=4Ch with
; AL = number of failed tests.
;
; Scratch state on the disk is created under A:\D2T and removed again, so
; the persistent image is left as it was found (unless a test fails midway;
; re-running cleans up leftovers first).
;
; Only MS-DOS 2.0 services are used. Assemble with:
;     nasm -f bin -o DOS2TEST.COM dos2test.asm
; ============================================================================

        cpu     8086
        org     100h

CHILD_EXIT_CODE equ     42
STACK_BYTES     equ     1024
BIGFILE_SIZE    equ     3000            ; spans 3 clusters and 6 sectors

start:
        ; ---- child mode: "DOS2TEST /CHILD" exits with a fixed code -------
        mov     si, 81h
.skipsp:
        lodsb
        cmp     al, ' '
        je      .skipsp
        cmp     al, '/'
        jne     .parent
        lodsw
        cmp     ax, 'CH'
        jne     .parent
        mov     ax, 4C00h + CHILD_EXIT_CODE
        int     21h

.parent:
        ; ---- move the stack into our image and give back the rest of the
        ;      64K block so 48h/4Bh have memory to work with ---------------
        mov     sp, stack_top
        mov     bx, program_end + 15    ; offset in PSP segment -> paragraphs
        mov     cl, 4
        shr     bx, cl
        mov     ah, 4Ah
        int     21h                     ; ES = PSP on entry for a COM
        jnc     .shrunk
        mov     dx, msg_shrink_fail
        call    print
        mov     ax, 4CFFh
        int     21h
.shrunk:
        mov     [cs_seg], cs
        mov     dx, msg_banner
        call    print

        ; leftovers from an interrupted earlier run
        call    cleanup_scratch

        mov     si, test_table
.next_test:
        lodsw
        or      ax, ax
        jz      .done
        mov     dx, ax                  ; name
        lodsw
        mov     bx, ax                  ; proc
        push    si
        push    bx
        call    print                   ; "  NAME ............"
        pop     bx
        call    bx
        jc      .failed
        inc     word [pass_count]
        mov     dx, msg_pass
        call    print
        jmp     short .after
.failed:
        inc     word [fail_count]
        push    ax
        mov     dx, msg_fail
        call    print
        pop     ax
        call    print_hex16
        mov     dx, msg_crlf
        call    print
.after:
        ; every test must leave DS=ES=CS
        push    cs
        pop     ds
        push    cs
        pop     es
        pop     si
        jmp     short .next_test

.done:
        call    cleanup_scratch
        mov     dx, msg_summary1
        call    print
        mov     ax, [pass_count]
        call    print_dec
        mov     dx, msg_summary2
        call    print
        mov     ax, [fail_count]
        call    print_dec
        mov     dx, msg_summary3
        call    print
        cmp     word [fail_count], 0
        jne     .someFailed
        mov     dx, msg_allpass
        call    print
        jmp     short .exit
.someFailed:
        mov     dx, msg_somefail
        call    print
.exit:
        mov     al, [fail_count]
        mov     ah, 4Ch
        int     21h

; ============================================================================
; helpers
; ============================================================================

print:                                  ; DS:DX -> '$' string (AH=09h)
        push    ax
        mov     ah, 09h
        int     21h
        pop     ax
        ret

putc:                                   ; AL = char (AH=02h)
        push    ax
        push    dx
        mov     dl, al
        mov     ah, 02h
        int     21h
        pop     dx
        pop     ax
        ret

print_hex16:                            ; AX
        push    ax
        mov     al, ah
        call    print_hex8
        pop     ax
print_hex8:
        push    ax
        mov     cl, 4
        shr     al, cl
        call    .nib
        pop     ax
        and     al, 0Fh
.nib:
        add     al, '0'
        cmp     al, '9'
        jbe     .out
        add     al, 7
.out:
        jmp     putc

print_dec:                              ; AX unsigned
        xor     cx, cx
        mov     bx, 10
.div:
        xor     dx, dx
        div     bx
        push    dx
        inc     cx
        or      ax, ax
        jnz     .div
.emit:
        pop     ax
        add     al, '0'
        call    putc
        loop    .emit
        ret

; fail helper: jump here with AX = code
fail:
        stc
        ret
pass:
        clc
        ret

cleanup_scratch:                        ; best effort; errors ignored
        mov     dx, path_root
        mov     ah, 3Bh
        int     21h
        mov     dx, path_file1
        mov     ah, 41h
        int     21h
        mov     dx, path_file2
        mov     ah, 41h
        int     21h
        mov     dx, path_file3
        mov     ah, 41h
        int     21h
        mov     dx, path_big
        mov     ah, 41h
        int     21h
        mov     dx, fcb_file_asciz
        mov     ah, 41h
        int     21h
        mov     dx, path_dir
        mov     ah, 3Ah
        int     21h
        ret

; fill pattern: byte i = (i * 7 + 3) & 0FFh, i = offset in file
fill_pattern:                           ; DI = buffer, CX = count, BX = start offset
.l:
        mov     al, bl
        mov     ah, 7
        mul     ah
        add     al, 3
        stosb
        inc     bx
        loop    .l
        ret

; compare buffer to pattern: SI = buffer, CX = count, BX = start offset
; returns CF=1 on mismatch
check_pattern:
.l:
        mov     al, bl
        mov     ah, 7
        mul     ah
        add     al, 3
        cmp     al, [si]
        jne     .bad
        inc     si
        inc     bx
        loop    .l
        clc
        ret
.bad:
        stc
        ret

; ============================================================================
; tests
; ============================================================================

; --- 30h: version must be 2.x ----------------------------------------------
t_version:
        mov     ah, 30h
        int     21h
        cmp     al, 2
        jne     .bad
        clc
        ret
.bad:
        jmp     fail

; --- 40h to STDOUT returns the byte count (the M12 bug class) -------------
t_handle_write:
        mov     ah, 40h
        mov     bx, 1
        mov     cx, msg_hw_len
        mov     dx, msg_hw
        int     21h
        jc      .err
        cmp     ax, msg_hw_len
        jne     .bad
        clc
        ret
.bad:
        mov     ax, 0E001h
.err:
        jmp     fail

; --- 02h/06h character output + 0Bh input status --------------------------
t_char_io:
        mov     dl, '<'
        mov     ah, 02h
        int     21h
        mov     dl, '>'
        mov     ah, 06h                 ; direct console output
        int     21h
        mov     dl, ' '
        mov     ah, 02h
        int     21h
        mov     ah, 0Bh
        int     21h
        cmp     al, 0
        je      .ok
        cmp     al, 0FFh
        je      .ok
        mov     ah, 0E0h
        jmp     fail
.ok:
        clc
        ret

; --- 19h/0Eh: current drive is A:, select A: reports >= 1 drive -----------
t_drive:
        mov     ah, 19h
        int     21h
        cmp     al, 0
        jne     .bad
        mov     dl, 0
        mov     ah, 0Eh
        int     21h
        cmp     al, 1
        jb      .bad
        clc
        ret
.bad:
        xor     ah, ah
        or      ax, 0E000h
        jmp     fail

; --- 2Ah/2Bh date round trip (CLOCK device read + write) ------------------
t_date:
        mov     ah, 2Ah
        int     21h
        cmp     cx, 1980
        jb      .bad1
        cmp     dh, 1
        jb      .bad2
        cmp     dh, 12
        ja      .bad2
        cmp     dl, 1
        jb      .bad3
        cmp     dl, 31
        ja      .bad3
        cmp     al, 6
        ja      .bad4
        mov     ah, 2Bh                 ; set the same date back
        int     21h
        cmp     al, 0
        jne     .bad5
        clc
        ret
.bad1:  mov     ax, 0E001h
        jmp     fail
.bad2:  mov     ax, 0E002h
        jmp     fail
.bad3:  mov     ax, 0E003h
        jmp     fail
.bad4:  mov     ax, 0E004h
        jmp     fail
.bad5:  mov     ax, 0E005h
        jmp     fail

; --- 2Ch/2Dh time round trip ----------------------------------------------
t_time:
        mov     ah, 2Ch
        int     21h
        cmp     ch, 23
        ja      .bad
        cmp     cl, 59
        ja      .bad
        cmp     dh, 59
        ja      .bad
        cmp     dl, 99
        ja      .bad
        mov     ah, 2Dh
        int     21h
        cmp     al, 0
        jne     .bad2
        clc
        ret
.bad:   mov     ax, 0E001h
        jmp     fail
.bad2:  mov     ax, 0E002h
        jmp     fail

; --- 1Ah/2Fh set/get DTA --------------------------------------------------
t_dta:
        mov     dx, dta_buf
        mov     ah, 1Ah
        int     21h
        mov     ah, 2Fh
        int     21h                     ; ES:BX
        mov     ax, es
        push    cs
        pop     es
        cmp     ax, [cs_seg]
        jne     .bad
        cmp     bx, dta_buf
        jne     .bad
        clc
        ret
.bad:
        mov     ax, 0E001h
        jmp     fail

; --- 35h/25h interrupt vectors --------------------------------------------
t_vectors:
        mov     ax, 3560h
        int     21h                     ; ES:BX = old INT 60h
        mov     [old_vec], bx
        mov     [old_vec + 2], es
        push    cs
        pop     es
        mov     dx, t_vectors           ; any recognizable CS offset
        mov     ax, 2560h
        int     21h
        mov     ax, 3560h
        int     21h
        mov     ax, es
        push    cs
        pop     es
        cmp     ax, [cs_seg]
        jne     .bad
        cmp     bx, t_vectors
        jne     .bad
        push    ds
        lds     dx, [old_vec]
        mov     ax, 2560h
        int     21h
        pop     ds
        clc
        ret
.bad:
        push    ds
        lds     dx, [old_vec]
        mov     ax, 2560h
        int     21h
        pop     ds
        mov     ax, 0E001h
        jmp     fail

; --- 33h ctrl-break flag, 54h/2Eh verify flag -----------------------------
t_flags:
        mov     ax, 3300h
        int     21h
        cmp     dl, 1
        ja      .bad1
        mov     [saved_flag], dl
        mov     dl, 1
        mov     ax, 3301h
        int     21h
        mov     ax, 3300h
        int     21h
        cmp     dl, 1
        jne     .bad2
        mov     dl, [saved_flag]
        mov     ax, 3301h
        int     21h

        mov     ah, 54h
        int     21h
        mov     [saved_flag], al
        mov     ax, 2E01h
        xor     dl, dl
        int     21h
        mov     ah, 54h
        int     21h
        cmp     al, 1
        jne     .bad3
        mov     al, [saved_flag]
        mov     ah, 2Eh
        xor     dl, dl
        int     21h
        clc
        ret
.bad1:  mov     ax, 0E001h
        jmp     fail
.bad2:  mov     ax, 0E002h
        jmp     fail
.bad3:  mov     ax, 0E003h
        jmp     fail

; --- 36h disk free space --------------------------------------------------
t_diskfree:
        mov     dl, 0
        mov     ah, 36h
        int     21h
        cmp     ax, 0FFFFh
        je      .bad1
        cmp     cx, 512
        jne     .bad2
        cmp     bx, dx
        ja      .bad3
        cmp     ax, 2                   ; 360K: 2 sectors/cluster
        jne     .bad4
        clc
        ret
.bad1:  mov     ax, 0E001h
        jmp     fail
.bad2:  mov     ax, 0E002h
        jmp     fail
.bad3:  mov     ax, 0E003h
        jmp     fail
.bad4:  mov     ax, 0E004h
        jmp     fail

; --- 44h IOCTL get device info: STDOUT is a device, CON is console out ----
t_ioctl:
        mov     ax, 4400h
        mov     bx, 1
        int     21h
        jc      .err
        test    dl, 80h                 ; ISDEV
        jz      .bad
        test    dl, 02h                 ; ISCOT (console output)
        jz      .bad
        clc
        ret
.bad:
        mov     ax, dx
.err:
        jmp     fail

; --- 45h DUP / 46h FORCEDUP / 3Eh CLOSE on handles ------------------------
t_dup:
        mov     bx, 1
        mov     ah, 45h
        int     21h
        jc      .err
        mov     [tmp_handle], ax
        mov     bx, ax
        mov     ah, 40h                 ; write through the duplicate
        mov     cx, 1
        mov     dx, msg_dot
        int     21h
        jc      .err_close
        cmp     ax, 1
        jne     .bad_close
        mov     bx, 1                   ; FORCEDUP stdout onto that handle
        mov     cx, [tmp_handle]
        mov     ah, 46h
        int     21h
        jc      .err_close
        mov     bx, [tmp_handle]
        mov     ah, 3Eh
        int     21h
        jc      .err
        clc
        ret
.bad_close:
        mov     ax, 0E001h
.err_close:
        push    ax
        mov     bx, [tmp_handle]
        mov     ah, 3Eh
        int     21h
        pop     ax
.err:
        jmp     fail

; --- 3Dh on a missing file must fail with error 2 -------------------------
t_open_missing:
        mov     dx, path_missing
        mov     ax, 3D00h
        int     21h
        jnc     .bad
        cmp     ax, 2
        jne     .err
        clc
        ret
.bad:
        mov     bx, ax
        mov     ah, 3Eh
        int     21h
        mov     ax, 0E001h
.err:
        jmp     fail

; --- 39h/3Bh/47h: mkdir, chdir, getcwd, back to root ----------------------
t_dirs:
        mov     dx, path_dir
        mov     ah, 39h
        int     21h
        jc      .err
        mov     dx, path_dir
        mov     ah, 3Bh
        int     21h
        jc      .err
        mov     byte [cwd_buf], 0FFh
        mov     si, cwd_buf
        mov     dl, 0
        mov     ah, 47h
        int     21h
        jc      .err_root
        mov     si, cwd_buf
        mov     di, name_dir
        mov     cx, 4                   ; "D2T",0
        repe    cmpsb
        jne     .bad
        mov     dx, path_root
        mov     ah, 3Bh
        int     21h
        jc      .err
        mov     si, cwd_buf             ; root reports empty path
        mov     dl, 0
        mov     ah, 47h
        int     21h
        jc      .err
        cmp     byte [cwd_buf], 0
        jne     .bad2
        clc
        ret
.bad:
        mov     ax, 0E001h
.err_root:
        push    ax
        mov     dx, path_root
        mov     ah, 3Bh
        int     21h
        pop     ax
        jmp     fail
.bad2:
        mov     ax, 0E002h
.err:
        jmp     fail

; --- 3Ch/40h/3Eh: create a multi-cluster file in the subdirectory ---------
t_create_write:
        mov     di, big_buf
        mov     cx, BIGFILE_SIZE
        xor     bx, bx
        call    fill_pattern
        mov     dx, path_big
        xor     cx, cx
        mov     ah, 3Ch
        int     21h
        jc      .err
        mov     [tmp_handle], ax
        mov     bx, ax
        mov     cx, BIGFILE_SIZE
        mov     dx, big_buf
        mov     ah, 40h
        int     21h
        jc      .err_close
        cmp     ax, BIGFILE_SIZE
        jne     .short
        mov     bx, [tmp_handle]
        mov     ah, 3Eh
        int     21h
        jc      .err
        clc
        ret
.short:
        mov     ax, 0E001h
.err_close:
        push    ax
        mov     bx, [tmp_handle]
        mov     ah, 3Eh
        int     21h
        pop     ax
.err:
        jmp     fail

; --- 3Dh/3Fh/42h: read back, seek, partial read, size via seek-end --------
t_read_seek:
        mov     dx, path_big
        mov     ax, 3D00h
        int     21h
        jc      .err
        mov     [tmp_handle], ax
        ; clear buffer then read everything
        mov     di, big_buf
        mov     cx, BIGFILE_SIZE
        xor     al, al
        rep     stosb
        mov     bx, [tmp_handle]
        mov     cx, BIGFILE_SIZE + 100  ; ask for more than exists
        mov     dx, big_buf
        mov     ah, 3Fh
        int     21h
        jc      .err_close
        cmp     ax, BIGFILE_SIZE
        jne     .bad1
        mov     si, big_buf
        mov     cx, BIGFILE_SIZE
        xor     bx, bx
        call    check_pattern
        jc      .bad2
        ; read at EOF returns 0
        mov     bx, [tmp_handle]
        mov     cx, 10
        mov     dx, big_buf
        mov     ah, 3Fh
        int     21h
        jc      .err_close
        or      ax, ax
        jnz     .bad3
        ; seek to 1500 (crosses a cluster boundary at 1024) and read 40
        mov     bx, [tmp_handle]
        xor     cx, cx
        mov     dx, 1500
        mov     ax, 4200h
        int     21h
        jc      .err_close
        cmp     ax, 1500
        jne     .bad4
        mov     bx, [tmp_handle]
        mov     cx, 40
        mov     dx, big_buf
        mov     ah, 3Fh
        int     21h
        jc      .err_close
        cmp     ax, 40
        jne     .bad5
        mov     si, big_buf
        mov     cx, 40
        mov     bx, 1500
        call    check_pattern
        jc      .bad6
        ; relative seek back 540 -> 1000
        mov     bx, [tmp_handle]
        mov     cx, 0FFFFh
        mov     dx, -540
        mov     ax, 4201h
        int     21h
        jc      .err_close
        cmp     ax, 1000
        jne     .bad7
        ; seek to end -> size
        mov     bx, [tmp_handle]
        xor     cx, cx
        xor     dx, dx
        mov     ax, 4202h
        int     21h
        jc      .err_close
        or      dx, dx
        jnz     .bad8
        cmp     ax, BIGFILE_SIZE
        jne     .bad8
        mov     bx, [tmp_handle]
        mov     ah, 3Eh
        int     21h
        jc      .err
        clc
        ret
.bad1:  mov     ax, 0E001h
        jmp     short .err_close
.bad2:  mov     ax, 0E002h
        jmp     short .err_close
.bad3:  mov     ax, 0E003h
        jmp     short .err_close
.bad4:  mov     ax, 0E004h
        jmp     short .err_close
.bad5:  mov     ax, 0E005h
        jmp     short .err_close
.bad6:  mov     ax, 0E006h
        jmp     short .err_close
.bad7:  mov     ax, 0E007h
        jmp     short .err_close
.bad8:  mov     ax, 0E008h
.err_close:
        push    ax
        mov     bx, [tmp_handle]
        mov     ah, 3Eh
        int     21h
        pop     ax
.err:
        jmp     fail

; --- open read/write, append 100 bytes, verify new size -------------------
t_append:
        mov     dx, path_big
        mov     ax, 3D02h
        int     21h
        jc      .err
        mov     [tmp_handle], ax
        mov     bx, ax
        xor     cx, cx
        xor     dx, dx
        mov     ax, 4202h
        int     21h
        jc      .err_close
        mov     di, big_buf
        mov     cx, 100
        mov     bx, BIGFILE_SIZE
        call    fill_pattern
        mov     bx, [tmp_handle]
        mov     cx, 100
        mov     dx, big_buf
        mov     ah, 40h
        int     21h
        jc      .err_close
        cmp     ax, 100
        jne     .bad1
        mov     bx, [tmp_handle]
        mov     ah, 3Eh
        int     21h
        jc      .err
        ; reopen and check size + the appended tail
        mov     dx, path_big
        mov     ax, 3D00h
        int     21h
        jc      .err
        mov     [tmp_handle], ax
        mov     bx, ax
        xor     cx, cx
        mov     dx, BIGFILE_SIZE - 10
        mov     ax, 4200h
        int     21h
        jc      .err_close
        mov     bx, [tmp_handle]
        mov     cx, 200
        mov     dx, big_buf
        mov     ah, 3Fh
        int     21h
        jc      .err_close
        cmp     ax, 110
        jne     .bad2
        mov     si, big_buf
        mov     cx, 110
        mov     bx, BIGFILE_SIZE - 10
        call    check_pattern
        jc      .bad3
        mov     bx, [tmp_handle]
        mov     ah, 3Eh
        int     21h
        jc      .err
        clc
        ret
.bad1:  mov     ax, 0E001h
        jmp     short .err_close
.bad2:  mov     ax, 0E002h
        jmp     short .err_close
.bad3:  mov     ax, 0E003h
.err_close:
        push    ax
        mov     bx, [tmp_handle]
        mov     ah, 3Eh
        int     21h
        pop     ax
.err:
        jmp     fail

; --- 43h attributes: archive set, read-only blocks write-open -------------
t_attrib:
        mov     dx, path_big
        mov     ax, 4300h
        int     21h
        jc      .err
        test    cl, 20h
        jz      .bad1
        mov     dx, path_big
        mov     cx, 01h                 ; read-only
        mov     ax, 4301h
        int     21h
        jc      .err
        mov     dx, path_big
        mov     ax, 3D01h               ; open for write must be refused
        int     21h
        jnc     .bad2_close
        cmp     ax, 5                   ; access denied
        jne     .bad3
        mov     dx, path_big
        mov     cx, 20h
        mov     ax, 4301h
        int     21h
        jc      .err
        mov     dx, path_big
        mov     ax, 4300h
        int     21h
        jc      .err
        test    cl, 01h
        jnz     .bad4
        clc
        ret
.bad1:  mov     ax, 0E001h
        jmp     short .err
.bad2_close:
        mov     bx, ax
        mov     ah, 3Eh
        int     21h
        mov     ax, 0E002h
        jmp     short .restore
.bad3:  or      ax, 0E300h
.restore:
        push    ax
        mov     dx, path_big
        mov     cx, 20h
        mov     ax, 4301h
        int     21h
        pop     ax
        jmp     short .err
.bad4:  mov     ax, 0E004h
.err:
        jmp     fail

; --- 57h get/set file date and time ---------------------------------------
t_filetime:
        mov     dx, path_big
        mov     ax, 3D00h
        int     21h
        jc      .err
        mov     [tmp_handle], ax
        mov     bx, ax
        mov     ax, 5700h
        int     21h
        jc      .err_close
        or      dx, dx                  ; date stamped from the CLOCK device
        jz      .bad1
        mov     bx, [tmp_handle]
        mov     cx, 6000h               ; 12:00:00
        mov     dx, 0068h               ; 1980-03-08
        mov     ax, 5701h
        int     21h
        jc      .err_close
        mov     bx, [tmp_handle]
        mov     ah, 3Eh
        int     21h
        jc      .err
        ; reopen: stamp must have been written to the directory entry
        mov     dx, path_big
        mov     ax, 3D00h
        int     21h
        jc      .err
        mov     [tmp_handle], ax
        mov     bx, ax
        mov     ax, 5700h
        int     21h
        jc      .err_close
        cmp     cx, 6000h
        jne     .bad2
        cmp     dx, 0068h
        jne     .bad2
        mov     bx, [tmp_handle]
        mov     ah, 3Eh
        int     21h
        clc
        ret
.bad1:  mov     ax, 0E001h
        jmp     short .err_close
.bad2:  mov     ax, 0E002h
.err_close:
        push    ax
        mov     bx, [tmp_handle]
        mov     ah, 3Eh
        int     21h
        pop     ax
.err:
        jmp     fail

; --- 56h rename, then 4Eh/4Fh find first/next -----------------------------
t_rename_find:
        ; two small files: FILE1.TXT (renamed to FILE2.TXT) and FILE3.TXT
        mov     dx, path_file1
        call    create_small
        jc      .err
        mov     dx, path_file3
        call    create_small
        jc      .err
        mov     dx, path_file1
        mov     di, path_file2
        mov     ah, 56h
        int     21h
        jc      .err
        ; old name gone
        mov     dx, path_file1
        mov     ax, 3D00h
        int     21h
        jnc     .bad1_close
        ; search D2T\*.TXT : expect FILE2.TXT and FILE3.TXT only
        mov     dx, dta_buf
        mov     ah, 1Ah
        int     21h
        mov     word [find_count], 0
        mov     byte [find_mask], 0
        mov     dx, path_glob
        xor     cx, cx
        mov     ah, 4Eh
        int     21h
        jc      .err
.found:
        inc     word [find_count]
        mov     al, [dta_buf + 1Eh + 4]         ; "FILEn.TXT": the digit
        cmp     al, '2'
        jne     .not2
        or      byte [find_mask], 1
.not2:
        cmp     al, '3'
        jne     .not3
        or      byte [find_mask], 2
        cmp     word [dta_buf + 1Ah], small_len ; size field
        jne     .bad2
.not3:
        mov     ah, 4Fh
        int     21h
        jnc     .found
        cmp     ax, 18                          ; no more files
        jne     .err
        cmp     word [find_count], 2
        jne     .bad3
        cmp     byte [find_mask], 3
        jne     .bad3
        ; the directory itself is found from the root with attribute 10h
        mov     dx, path_dir_nobs
        mov     cx, 10h
        mov     ah, 4Eh
        int     21h
        jc      .err
        test    byte [dta_buf + 15h], 10h
        jz      .bad4
        clc
        ret
.bad1_close:
        mov     bx, ax
        mov     ah, 3Eh
        int     21h
        mov     ax, 0E001h
        jmp     short .err
.bad2:  mov     ax, 0E002h
        jmp     short .err
.bad3:  mov     ax, [find_count]
        or      ah, 0E3h
        jmp     short .err
.bad4:  mov     ax, 0E004h
.err:
        jmp     fail

create_small:                           ; DX = path; writes small_msg
        xor     cx, cx
        mov     ah, 3Ch
        int     21h
        jc      .r
        mov     bx, ax
        mov     cx, small_len
        mov     dx, small_msg
        mov     ah, 40h
        int     21h
        pushf
        push    ax
        mov     ah, 3Eh
        int     21h
        pop     ax
        popf
.r:
        ret

; --- 41h delete, 3Ah rmdir, and the expected errors afterwards ------------
t_delete_rmdir:
        ; rmdir of a non-empty directory must fail
        mov     dx, path_dir
        mov     ah, 3Ah
        int     21h
        jnc     .bad1
        mov     dx, path_file2
        mov     ah, 41h
        int     21h
        jc      .err
        mov     dx, path_file3
        mov     ah, 41h
        int     21h
        jc      .err
        mov     dx, path_big
        mov     ah, 41h
        int     21h
        jc      .err
        mov     dx, path_big            ; deleting again: file not found
        mov     ah, 41h
        int     21h
        jnc     .bad2
        cmp     ax, 2
        jne     .err
        mov     dx, path_dir
        mov     ah, 3Ah
        int     21h
        jc      .err
        mov     dx, path_dir            ; chdir into it: path not found
        mov     ah, 3Bh
        int     21h
        jnc     .bad3
        cmp     ax, 3
        jne     .err
        clc
        ret
.bad1:  mov     ax, 0E001h
        jmp     short .err
.bad2:  mov     ax, 0E002h
        jmp     short .err
.bad3:  mov     ax, 0E003h
        push    ax
        mov     dx, path_root
        mov     ah, 3Bh
        int     21h
        pop     ax
.err:
        jmp     fail

; --- 48h/4Ah/49h memory allocation ----------------------------------------
t_memory:
        mov     bx, 0FFFFh              ; impossible request reports largest
        mov     ah, 48h
        int     21h
        jnc     .bad1
        cmp     ax, 8
        jne     .err
        cmp     bx, 100h
        jb      .bad2
        mov     bx, 100h                ; 4 KiB
        mov     ah, 48h
        int     21h
        jc      .err
        mov     [mem_seg], ax
        mov     es, ax                  ; write and read back a pattern
        xor     di, di
        mov     cx, 100h * 16
        mov     al, 5Ah
        rep     stosb
        xor     di, di
        mov     cx, 100h * 16
        repe    scasb
        jne     .bad3
        mov     es, [mem_seg]           ; shrink to 40h paragraphs
        mov     bx, 40h
        mov     ah, 4Ah
        int     21h
        jc      .err_free
        mov     es, [mem_seg]
        mov     ah, 49h
        int     21h
        jc      .err_es
        push    cs
        pop     es
        clc
        ret
.bad1:  mov     ax, 0E001h
        jmp     short .err_es
.bad2:  mov     ax, 0E002h
        jmp     short .err_es
.bad3:  mov     ax, 0E003h
.err_free:
        push    ax
        mov     es, [mem_seg]
        mov     ah, 49h
        int     21h
        pop     ax
.err_es:
        push    cs
        pop     es
.err:
        jmp     fail

; --- FCB services: 29h parse, 16h create, 15h/14h seq write/read,
;     0Fh open, 10h close, 11h search, 13h delete --------------------------
t_fcb:
        mov     dx, fcb_dta
        mov     ah, 1Ah
        int     21h
        ; parse "FCBTEST.DAT" into the FCB
        mov     di, fcb
        mov     cx, 37
        xor     al, al
        rep     stosb
        mov     si, fcb_name_text
        mov     di, fcb
        mov     ax, 2900h
        int     21h
        cmp     al, 0
        jne     .bad1
        cmp     byte [fcb + 1], 'F'
        jne     .bad1
        cmp     byte [fcb + 9], 'D'
        jne     .bad1
        ; create, write two 128-byte records
        mov     dx, fcb
        mov     ah, 16h
        int     21h
        cmp     al, 0
        jne     .bad2
        mov     word [fcb + 0Ch], 0     ; current block
        mov     byte [fcb + 20h], 0     ; current record
        mov     word [fcb + 0Eh], 128   ; record size
        mov     di, fcb_dta
        mov     cx, 128
        xor     bx, bx
        call    fill_pattern
        mov     dx, fcb
        mov     ah, 15h
        int     21h
        cmp     al, 0
        jne     .bad3
        mov     di, fcb_dta
        mov     cx, 128
        mov     bx, 128
        call    fill_pattern
        mov     dx, fcb
        mov     ah, 15h
        int     21h
        cmp     al, 0
        jne     .bad3
        mov     dx, fcb
        mov     ah, 10h
        int     21h
        cmp     al, 0
        jne     .bad4
        ; reopen: size must be 256
        mov     dx, fcb
        mov     ah, 0Fh
        int     21h
        cmp     al, 0
        jne     .bad5
        cmp     word [fcb + 10h], 256
        jne     .bad6
        mov     word [fcb + 0Ch], 0
        mov     byte [fcb + 20h], 0
        mov     word [fcb + 0Eh], 128
        mov     dx, fcb
        mov     ah, 14h
        int     21h
        cmp     al, 0
        jne     .bad7
        mov     si, fcb_dta
        mov     cx, 128
        xor     bx, bx
        call    check_pattern
        jc      .bad8
        mov     dx, fcb
        mov     ah, 14h
        int     21h
        cmp     al, 0
        jne     .bad7
        mov     si, fcb_dta
        mov     cx, 128
        mov     bx, 128
        call    check_pattern
        jc      .bad8
        mov     dx, fcb                 ; third read is EOF (AL=1)
        mov     ah, 14h
        int     21h
        cmp     al, 1
        jne     .bad9
        mov     dx, fcb
        mov     ah, 10h
        int     21h
        ; search for it with an unopened FCB, then delete
        mov     si, fcb_name_text
        mov     di, fcb2
        mov     ax, 2900h
        int     21h
        mov     dx, fcb2
        mov     ah, 11h
        int     21h
        cmp     al, 0
        jne     .bad10
        mov     dx, fcb2
        mov     ah, 13h
        int     21h
        cmp     al, 0
        jne     .bad11
        mov     dx, fcb2                ; gone
        mov     ah, 11h
        int     21h
        cmp     al, 0FFh
        jne     .bad12
        clc
        ret
.bad1:  mov     ax, 0E001h
        jmp     short .err
.bad2:  mov     ax, 0E002h
        jmp     short .err
.bad3:  mov     ax, 0E003h
        jmp     short .err
.bad4:  mov     ax, 0E004h
        jmp     short .err
.bad5:  mov     ax, 0E005h
        jmp     short .err
.bad6:  mov     ax, [fcb + 10h]
        jmp     short .err
.bad7:  mov     ah, 0E7h
        jmp     short .err
.bad8:  mov     ax, 0E008h
        jmp     short .err
.bad9:  mov     ah, 0E9h
        jmp     short .err
.bad10: mov     ax, 0E00Ah
        jmp     short .err
.bad11: mov     ax, 0E00Bh
        jmp     short .err
.bad12: mov     ax, 0E00Ch
.err:
        jmp     fail

; --- 4Bh EXEC + 4Dh return code: run ourselves as a child -----------------
t_exec_child:
        mov     dx, path_self
        mov     bx, tail_child
        call    do_exec
        jc      .err
        mov     ah, 4Dh
        int     21h
        cmp     ax, CHILD_EXIT_CODE     ; AH=0 normal termination
        jne     .bad
        clc
        ret
.bad:
        jmp     fail
.err:
        jmp     fail

; --- 4Bh EXEC COMMAND.COM /C VER (nested shell) ---------------------------
t_exec_command:
        mov     dx, msg_crlf
        call    print
        mov     dx, path_command
        mov     bx, tail_ver
        call    do_exec
        jc      .err
        mov     ah, 4Dh
        int     21h
        cmp     ah, 0
        jne     .bad
        mov     dx, msg_exec_cmd_pad
        call    print
        clc
        ret
.bad:
        jmp     fail
.err:
        jmp     fail

; DX = program path, BX = command tail (length byte, text, CR)
; DOS 2 EXEC destroys every register except CS:IP, so SS:SP is kept in CS.
do_exec:
        mov     [exec_block + 2], bx
        mov     [exec_block + 4], cs
        mov     [exec_block + 8], cs
        mov     [exec_block + 12], cs
        mov     word [exec_block], 0    ; inherit environment
        mov     word [exec_block + 6], 5Ch
        mov     word [exec_block + 10], 6Ch
        push    bp
        push    si
        push    di
        mov     [save_sp], sp
        mov     [save_ss], ss
        mov     bx, exec_block
        mov     ax, 4B00h
        int     21h
        cli
        mov     ss, [cs:save_ss]
        mov     sp, [cs:save_sp]
        sti
        push    cs
        pop     ds
        push    cs
        pop     es
        pop     di
        pop     si
        pop     bp
        ret                             ; CF/AX from EXEC preserved

; ============================================================================
; data
; ============================================================================

cs_seg:         dw      0               ; filled at startup below
old_vec:        dd      0
saved_flag:     db      0
tmp_handle:     dw      0
mem_seg:        dw      0
find_count:     dw      0
find_mask:      db      0
pass_count:     dw      0
fail_count:     dw      0
save_sp:        dw      0
save_ss:        dw      0
exec_block:     times 14 db 0

path_root:      db      '\', 0
path_dir:       db      '\D2T', 0
path_dir_nobs:  db      'D2T', 0
name_dir:       db      'D2T', 0
path_big:       db      '\D2T\BIG.DAT', 0
path_file1:     db      '\D2T\FILE1.TXT', 0
path_file2:     db      '\D2T\FILE2.TXT', 0
path_file3:     db      '\D2T\FILE3.TXT', 0
path_glob:      db      '\D2T\*.TXT', 0
path_missing:   db      'NOSUCH.XYZ', 0
path_self:      db      'A:\DOS2TEST.COM', 0
path_command:   db      'A:\COMMAND.COM', 0
fcb_file_asciz: db      '\FCBTEST.DAT', 0
fcb_name_text:  db      'FCBTEST.DAT', 0

tail_child:     db      7, ' /CHILD', 13
tail_ver:       db      7, ' /C VER', 13

small_msg:      db      'microDOS DOS2TEST scratch file', 13, 10
small_len       equ     $ - small_msg

msg_hw:         db      '(40h->STDOUT) '
msg_hw_len      equ     $ - msg_hw
msg_dot:        db      '.'

msg_banner:     db      13, 10, 'DOS2TEST - MS-DOS 2.0 service test for microDOS', 13, 10
                db      '-----------------------------------------------', 13, 10, '$'
msg_pass:       db      'PASS', 13, 10, '$'
msg_fail:       db      'FAIL  code=$'
msg_crlf:       db      13, 10, '$'
msg_exec_cmd_pad: db    '  ...................................... $'
msg_summary1:   db      '-----------------------------------------------', 13, 10
                db      'passed: $'
msg_summary2:   db      '   failed: $'
msg_summary3:   db      13, 10, '$'
msg_allpass:    db      'ALL TESTS PASSED', 13, 10, '$'
msg_somefail:   db      'SOME TESTS FAILED (exit code = failures)', 13, 10, '$'
msg_shrink_fail: db     'DOS2TEST: AH=4Ah shrink failed', 13, 10, '$'

; name strings: fixed width so PASS/FAIL line up
%macro TESTNAME 2
%1:     db      '  ', %2
        times   40 - ($ - %1) db '.'
        db      ' $'
%endmacro

        TESTNAME n_version,      '30h  version is 2.x '
        TESTNAME n_hwrite,       '40h  STDOUT byte count '
        TESTNAME n_chario,       '02h/06h/0Bh char I/O '
        TESTNAME n_drive,        '19h/0Eh current drive '
        TESTNAME n_date,         '2Ah/2Bh date round trip '
        TESTNAME n_time,         '2Ch/2Dh time round trip '
        TESTNAME n_dta,          '1Ah/2Fh DTA '
        TESTNAME n_vectors,      '35h/25h vectors '
        TESTNAME n_flags,        '33h/54h/2Eh flags '
        TESTNAME n_diskfree,     '36h disk free '
        TESTNAME n_ioctl,        '44h IOCTL device info '
        TESTNAME n_dup,          '45h/46h DUP/FORCEDUP '
        TESTNAME n_openmiss,     '3Dh missing -> err 2 '
        TESTNAME n_dirs,         '39h/3Bh/47h dirs '
        TESTNAME n_create,       '3Ch/40h create 3000B '
        TESTNAME n_readseek,     '3Fh/42h read + seek '
        TESTNAME n_append,       '3Dh rw append '
        TESTNAME n_attrib,       '43h attributes '
        TESTNAME n_filetime,     '57h file date/time '
        TESTNAME n_renfind,      '56h/4Eh/4Fh rename+find '
        TESTNAME n_delete,       '41h/3Ah delete+rmdir '
        TESTNAME n_memory,       '48h/4Ah/49h memory '
        TESTNAME n_fcb,          'FCB 29h/0F-16h '
        TESTNAME n_execchild,    '4Bh/4Dh EXEC child '
        TESTNAME n_execcmd,      '4Bh COMMAND /C VER '

test_table:
        dw      n_version,   t_version
        dw      n_hwrite,    t_handle_write
        dw      n_chario,    t_char_io
        dw      n_drive,     t_drive
        dw      n_date,      t_date
        dw      n_time,      t_time
        dw      n_dta,       t_dta
        dw      n_vectors,   t_vectors
        dw      n_flags,     t_flags
        dw      n_diskfree,  t_diskfree
        dw      n_ioctl,     t_ioctl
        dw      n_dup,       t_dup
        dw      n_openmiss,  t_open_missing
        dw      n_dirs,      t_dirs
        dw      n_create,    t_create_write
        dw      n_readseek,  t_read_seek
        dw      n_append,    t_append
        dw      n_attrib,    t_attrib
        dw      n_filetime,  t_filetime
        dw      n_renfind,   t_rename_find
        dw      n_delete,    t_delete_rmdir
        dw      n_memory,    t_memory
        dw      n_fcb,       t_fcb
        dw      n_execchild, t_exec_child
        dw      n_execcmd,   t_exec_command
        dw      0

; ---- uninitialized buffers: not stored in the .COM file --------------------
        section .bss
        alignb  16
dta_buf:        resb    128
fcb_dta:        resb    128
fcb:            resb    40
fcb2:           resb    40
cwd_buf:        resb    68
big_buf:        resb    BIGFILE_SIZE + 200
stack_area:     resb    STACK_BYTES
stack_top:
program_end:
