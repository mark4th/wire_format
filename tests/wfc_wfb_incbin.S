    .section .rodata
    .balign 4
    .globl wfc_test_wfb_start
    .globl wfc_test_wfb_end
wfc_test_wfb_start:
    .incbin "example_telemetry.wfb"
wfc_test_wfb_end:

    .section .note.GNU-stack,"",@progbits
