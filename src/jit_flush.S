// Xenon's self-modifying-code sequence (libxenon:drivers/ppc/cache.S:55-70); aligned inclusive endpoints.
	.text
	.balign 16
	.globl xsdk_jit_flush_lines
	.type xsdk_jit_flush_lines, @function
xsdk_jit_flush_lines:
	mr r5, r3
1:
	dcbst 0, r5
	cmpw r5, r4
	beq 2f
	addi r5, r5, 128
	b 1b
2:
	sync
3:
	icbi 0, r3
	cmpw r3, r4
	beq 4f
	addi r3, r3, 128
	b 3b
4:
	sync
	isync
	blr
	.size xsdk_jit_flush_lines, . - xsdk_jit_flush_lines
