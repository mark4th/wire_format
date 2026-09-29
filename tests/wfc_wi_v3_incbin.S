    .section .rodata
    .balign 4
    .globl wfc_v3_wi_start
    .globl wfc_v3_wi_end
wfc_v3_wi_start:
    .incbin "v3.wi"
wfc_v3_wi_end:

    .section .note.GNU-stack,"",@progbits
