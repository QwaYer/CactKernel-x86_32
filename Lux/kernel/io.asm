[bits 32]

global port_long_out
global port_long_in
global port_byte_out
global port_byte_in

port_long_out:
    mov edx, [esp + 4]  
    mov eax, [esp + 8]  
    out dx, eax         
    ret

port_long_in:
    mov edx, [esp + 4]   
    in eax, dx          
    ret

port_byte_out:
    mov edx, [esp + 4]
    mov al, [esp + 8]
    out dx, al
    ret

port_byte_in:
    mov edx, [esp + 4]
    in al, dx
    ret