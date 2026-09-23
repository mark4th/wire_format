    .section .rodata
    .balign 4
    .globl wfc_v2_wi_start
    .globl wfc_v2_wi_end
wfc_v2_wi_start:
    .incbin "v2.wi"
wfc_v2_wi_end:

    .section .note.GNU-stack,"",@progbits
