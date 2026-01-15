[bits 32]

global keyboard_isr
extern keyboard_handler

keyboard_isr:
    pusha          
    
    call keyboard_handler ; 
    
    popa         
    iretd         