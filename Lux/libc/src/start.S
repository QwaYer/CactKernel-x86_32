.section .text
.global _start
.type _start, @function

_start:
    movl (%esp), %eax       // argc 
    movl 4(%esp), %ecx      // argv 
    movl 8(%esp), %edx      // envp 
    pushl %edx
    pushl %ecx
    pushl %eax
    call main
    addl $12, %esp
    pushl %eax
    call exit
    hlt