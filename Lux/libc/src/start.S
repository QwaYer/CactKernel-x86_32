.section .text
.global _start
.type _start, @function

_start:
    movl (%esp), %eax
    movl 4(%esp), %ecx
    pushl %ecx
    pushl %eax
    call main
    addl $8, %esp
    pushl %eax
    call exit
    hlt
