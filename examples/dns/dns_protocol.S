    .section .rodata
    .balign 4
    .globl wi_dns_database_start
    .globl wi_dns_database_end
wi_dns_database_start:
    .incbin "target/examples/dns/dns_protocol.wfb"
wi_dns_database_end:

    .section .note.GNU-stack,"",@progbits
