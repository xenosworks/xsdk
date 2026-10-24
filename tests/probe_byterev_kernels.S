// The kernels of tests/probe_byterev.c, in assembly so that the compiler can't reorder or drop what's timed. Each takes
// the base address in r3, the passes in r4 and the values in r5, and does 8 operations a pass. A load kernel leaves
// what it loaded in values, and a store kernel stores what values holds. r14-r21 hold data and r22-r29 offsets; a
// kernel saves r14-r31, and uses f0-f7 and v0-v12 as it likes.

	.set FRAME, 224
	// r14-r31 go above the back chain and the LR word.
	.set SAVE, 16
	// Eight 8-byte slots on the stack, for the data that goes between a GPR and an FPR.
	.set SLOT, SAVE + 18 * 8
	// The slots lie 144 bytes apart: 16-aligned for lvx and stvx, each in a cache line of its own.
	.set STRIDE, 144
	.set VALUE, 14
	.set OFFSET, 22

	.macro enter name
	.section .text.\name, "ax"
	.balign 32
	.globl \name
	.type \name, @function
\name:
	stwu r1, -FRAME(r1)
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	std r\n, SAVE + 8 * (\n - 14)(r1)
	.endr
	.endm

	.macro leave name
	.irp n, 14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	ld r\n, SAVE + 8 * (\n - 14)(r1)
	.endr
	addi r1, r1, FRAME
	blr
	.size \name, . - \name
	.endm

	.macro offsets
	.irp k, 0,1,2,3,4,5,6,7
	li OFFSET + \k, STRIDE * \k
	.endr
	.endm

	// The loop runs r4 passes, from a 32-byte boundary.
	.macro loop
	mtctr r4
	.balign 32
1:
	.endm

	.macro again
	bdnz 1b
	.endm

	// Eight independent loads of one form.
	.macro loads name, insn
	enter \name
	offsets
	loop
	.irp k, 0,1,2,3,4,5,6,7
	\insn VALUE + \k, 3, OFFSET + \k
	.endr
	again
	.irp k, 0,1,2,3,4,5,6,7
	std VALUE + \k, 8 * \k(r5)
	.endr
	leave \name
	.endm

	// A dependent chain: each load's result is the offset of the next.
	.macro chase name, insn
	enter \name
	li VALUE, 0
	loop
	.rept 8
	\insn VALUE, 3, VALUE
	.endr
	again
	std VALUE, 0(r5)
	leave \name
	.endm

	// Eight independent stores of one form.
	.macro stores name, insn
	enter \name
	offsets
	.irp k, 0,1,2,3,4,5,6,7
	ld VALUE + \k, 8 * \k(r5)
	.endr
	loop
	.irp k, 0,1,2,3,4,5,6,7
	\insn VALUE + \k, 3, OFFSET + \k
	.endr
	again
	leave \name
	.endm

	// A dependent chain: each store's word is loaded back and stored again.
	.macro store_load name, store, load
	enter \name
	ld VALUE, 0(r5)
	li OFFSET, 0
	loop
	.rept 8
	\store VALUE, 3, OFFSET
	\load VALUE, 3, OFFSET
	.endr
	again
	// One more pair makes the count odd, so that a pair that reverses shows in the result.
	\store VALUE, 3, OFFSET
	\load VALUE, 3, OFFSET
	std VALUE, 0(r5)
	leave \name
	.endm

	loads probe_byterev_lhzx, lhzx
	loads probe_byterev_lhbrx, lhbrx
	loads probe_byterev_lwzx, lwzx
	loads probe_byterev_lwbrx, lwbrx
	loads probe_byterev_ldx, ldx
	chase probe_byterev_chase_lwzx, lwzx
	chase probe_byterev_chase_lwbrx, lwbrx
	chase probe_byterev_chase_ldx, ldx
	stores probe_byterev_sthx, sthx
	stores probe_byterev_sthbrx, sthbrx
	stores probe_byterev_stwx, stwx
	stores probe_byterev_stwbrx, stwbrx
	stores probe_byterev_stdx, stdx
	store_load probe_byterev_stwx_lwzx, stwx, lwzx
	store_load probe_byterev_stwbrx_lwzx, stwbrx, lwzx
	store_load probe_byterev_stwbrx_lwbrx, stwbrx, lwbrx

	// The doubleword byte-reversed forms, which the console may refuse (Xenia has both, for 360 code:
	// xenia:src/xenia/cpu/ppc/ppc_emit_memory.cc:640-690), so the probe runs them last.
	loads probe_byterev_ldbrx, ldbrx
	chase probe_byterev_chase_ldbrx, ldbrx
	stores probe_byterev_stdbrx, stdbrx
