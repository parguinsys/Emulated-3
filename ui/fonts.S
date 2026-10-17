/* OFL fonts compiled into the binary (see third_party/README.md and ui/fonts.h); assembled with -Wa,-I<third_party/fonts> */
	.section .rodata
	.balign 16
	.global eiii_font_regular
	.hidden eiii_font_regular
	.global eiii_font_regular_end
	.hidden eiii_font_regular_end
eiii_font_regular:
	.incbin "NotoSans-CondensedMedium.ttf"
eiii_font_regular_end:
	.balign 16
	.global eiii_font_bold
	.hidden eiii_font_bold
	.global eiii_font_bold_end
	.hidden eiii_font_bold_end
eiii_font_bold:
	.incbin "NotoSans-CondensedSemiBold.ttf"
eiii_font_bold_end:
	.balign 16
	.global eiii_font_narrow
	.hidden eiii_font_narrow
	.global eiii_font_narrow_end
	.hidden eiii_font_narrow_end
eiii_font_narrow:
	.incbin "ArchivoNarrow-wght.ttf"
eiii_font_narrow_end:
	.balign 8
	.global eiii_font_regular_size
	.hidden eiii_font_regular_size
eiii_font_regular_size:
	.quad eiii_font_regular_end - eiii_font_regular
	.global eiii_font_bold_size
	.hidden eiii_font_bold_size
eiii_font_bold_size:
	.quad eiii_font_bold_end - eiii_font_bold
	.global eiii_font_narrow_size
	.hidden eiii_font_narrow_size
eiii_font_narrow_size:
	.quad eiii_font_narrow_end - eiii_font_narrow
	.section .note.GNU-stack,"",%progbits
