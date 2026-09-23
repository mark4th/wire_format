    .section .rodata
    .balign 4
    .globl wfc_test_wi_start
    .globl wfc_test_wi_end
wfc_test_wi_start:
    .incbin "example_telemetry.wi"
wfc_test_wi_end:

    .section .note.GNU-stack,"",@progbits
