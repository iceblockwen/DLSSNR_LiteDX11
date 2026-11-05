EXTERN ResolveDxgi:PROC
EXTERN DxgiTargets:QWORD
.code
FORWARD MACRO number
LOCAL resolved
Proxy&number PROC
 mov rax, QWORD PTR [DxgiTargets + number * 8]
 test rax, rax
 jnz resolved
 jmp Slow&number
resolved:
 jmp rax
Proxy&number ENDP
Slow&number PROC FRAME
 push rcx
 .allocstack 8
 push rdx
 .allocstack 8
 push r8
 .allocstack 8
 push r9
 .allocstack 8
 sub rsp, 104
 .allocstack 104
 .endprolog
 movdqu [rsp+32], xmm0
 movdqu [rsp+48], xmm1
 movdqu [rsp+64], xmm2
 movdqu [rsp+80], xmm3
 mov ecx, number
 call ResolveDxgi
 mov r11, rax
 movdqu xmm0, [rsp+32]
 movdqu xmm1, [rsp+48]
 movdqu xmm2, [rsp+64]
 movdqu xmm3, [rsp+80]
 add rsp, 104
 pop r9
 pop r8
 pop rdx
 pop rcx
 jmp r11
Slow&number ENDP
ENDM
FORWARD 0
FORWARD 1
FORWARD 2
FORWARD 3
FORWARD 4
FORWARD 5
FORWARD 6
FORWARD 7
FORWARD 8
FORWARD 9
FORWARD 10
FORWARD 11
FORWARD 12
FORWARD 13
FORWARD 14
END



