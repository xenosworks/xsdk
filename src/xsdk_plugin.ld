/* Lays a plugin, a system DLL, out for elf2xex --dll: as src/xsdk.ld lays a title out, but on 4 KB pages from the base
   the link gives as --defsym=xsdk_plugin_base=<address>. */
OUTPUT_FORMAT("elf32-powerpc")
OUTPUT_ARCH(powerpc:common)
ENTRY(xsdk_plugin_entry)
/* It lives in libxsdk_plugin.a, which only the entry point refers to. */
EXTERN(xsdk_plugin_entry)

SECTIONS
{
	/* elf2xex puts the PE headers in the first 4 KB. */
	. = xsdk_plugin_base + 0x1000;
	.rdata : {
		*(.rodata .rodata.* .gnu.linkonce.r.*)
		*(.sdata2 .sdata2.* .sbss2 .sbss2.*)
	}

	. = ALIGN(0x1000);
	.text : {
		/* Imports first: Xenia runs a tail call's target as the caller (xenia:src/xenia/cpu/ppc/ppc_scanner.cc:204). */
		*(.text.xsdk_thunk_*)
		*(.text .text.*)
	}

	. = ALIGN(0x1000);
	.data : {
		*(.data .data.* .gnu.linkonce.d.*)
		*(.sdata .sdata.*)
	}
	.bss : {
		*(.sbss .sbss.* .bss .bss.* COMMON)
	}

	.xsdk.imports 0 : {
		KEEP(*(.xsdk.imports))
	}

	/* A plugin, C on libxsdk_plugin.a alone, has no unwinder or constructors. */
	/DISCARD/ : {
		*(.comment .note.* .gnu.attributes .eh_frame)
	}
}
