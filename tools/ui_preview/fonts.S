// Embed the same font blobs the firmware links (paths passed via -DFONT17/-DFONT29)
#if defined(__APPLE__)
    .section __DATA,__const
#else
    .section .rodata
#endif
    .globl _binary_official_font_17_bin_start
    .globl _binary_official_font_17_bin_end
    .globl _binary_official_font_29_bin_start
    .globl _binary_official_font_29_bin_end
_binary_official_font_17_bin_start:
    .incbin FONT17
_binary_official_font_17_bin_end:
_binary_official_font_29_bin_start:
    .incbin FONT29
_binary_official_font_29_bin_end:
