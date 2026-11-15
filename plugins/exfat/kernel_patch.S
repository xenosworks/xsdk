# Milestone 77 step 4: the kernel loads the exFAT module from the flash at boot (docs/scope/77-exfat.md; the boot
# point and the .text padding in plugins/exfat/kernel_seam.md, section 7). tools/exfat_patch.py assembles this at the
# stub's address in the padding, links XexLoadImage and sub_800C8260 to the kernel's, and wraps it in xeBuild's patch
# format with the one-word patch that turns ExpSystemInitThread's `bl sub_800C8260` at 0x800613dc into `bl stub`.
#
# That bl is the first call after xam's load and after the point where xeBuild's DashLaunch patch loads launch.xex
# (its stub returns to 0x800613dc): a system image of the module's size loaded before launch.xex stops the boot in
# DashLaunch's hooks, loaded after it the console boots (results/2026-11-14-milestone-77-step-4, runs 24 to 28).
# sub_800C8260 takes no arguments: it writes one hardware register for two processor revisions.
#
# The stub loads the module as a system DLL with the arguments step 3's loader title proved on the console (flags 8,
# the DLL bit the loader's entry requires, and no minimum version), drops the status, so a flash without the file
# boots as before, and goes on to sub_800C8260 with the original return address. r14-r31 are callee-saved, so the
# kernel's r30 and r31 come through untouched. LR is kept below r1 before the frame is made, as the kernel's own
# prologues keep it.
	.text
	.globl	stub
stub:
	mflr	r12
	stw	r12, -8(r1)
	stwu	r1, -0x20(r1)
	lis	r3, name@ha
	addi	r3, r3, name@l
	li	r4, 8
	li	r5, 0
	li	r6, 0
	bl	XexLoadImage
	lwz	r12, 0x18(r1)
	addi	r1, r1, 0x20
	mtlr	r12
	b	sub_800C8260
name:
	.asciz	"\\Device\\Flash\\exfat.xex"
