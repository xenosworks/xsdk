# Milestone 77 step 4, a diagnostic variant of plugins/exfat/kernel_patch.S (2026-11-14). The console hangs on the boot
# that loads the module, and its ring of light carries RGB LEDs, so the stub reports through the SMC's power instead:
# once XexLoadImage has returned, whatever it answered, it sends the shutdown message, 0x82, 0x01
# (libxenon:libxenon/drivers/xenon_smc/xenon_smc.c:268-272, xenon_smc_power_shutdown), and goes on as the production
# stub does.
# A console that switches itself off a few seconds into the boot had the load return; one that stays on, hung, never
# got it back. HalSendSMCMessage is ordinal 0x29 in the 17559 kernel's export table, 0x80067f48, given to
# tools/exfat_patch.py as --defsym=HalSendSMCMessage=0x80067f48; it takes the NULL response. The frame is 0x40 bytes:
# the saved LR at 0x38, the 16-byte message at 0x20.
	.text
	.globl	stub
stub:
	mflr	r12
	stw	r12, -8(r1)
	stwu	r1, -0x40(r1)
	lis	r3, name@ha
	addi	r3, r3, name@l
	li	r4, 8
	li	r5, 0
	li	r6, 0
	bl	XexLoadImage
	lis	r11, 0x8201
	stw	r11, 0x20(r1)
	li	r11, 0
	stw	r11, 0x24(r1)
	stw	r11, 0x28(r1)
	stw	r11, 0x2c(r1)
	addi	r3, r1, 0x20
	li	r4, 0
	bl	HalSendSMCMessage
	lwz	r12, 0x38(r1)
	addi	r1, r1, 0x40
	mtlr	r12
	b	ExpCheckSystemUpdate
name:
	.asciz	"\\Device\\Flash\\exfat.xex"
