/* Embeds web/index.html into the binary as the NUL terminated string web_index_html. */
    .section .rodata
    .global web_index_html
    .type   web_index_html, %object
web_index_html:
    .incbin "web/index.html"
    .byte   0
    .size   web_index_html, . - web_index_html
    .section .note.GNU-stack,"",%progbits
