/* Lays a DLL out for elf2xex --dll, as xsdk.ld lays out a title: read-only, code and writable data each start on their
   own 64 KB page. Link with -Wl,--defsym=xsdk_dll_base=<address> and give elf2xex the same --base. The loader calls
   DllMain and nothing else, so a DLL with constructors or destructors fails to link. */
OUTPUT_FORMAT("elf32-powerpc")
OUTPUT_ARCH(powerpc:common)
ENTRY(DllMain)

SECTIONS
{
	/* elf2xex puts the PE headers and the export table in the first 4 KB. */
	. = xsdk_dll_base + 0x1000;
	.rdata : {
		*(.rodata .rodata.* .gnu.linkonce.r.*)
		*(.sdata2 .sdata2.* .sbss2 .sbss2.*)
		*(.gcc_except_table .gcc_except_table.*)
	}
	.eh_frame : {
		KEEP(*(.eh_frame))
	}

	. = ALIGN(0x10000);
	.text : {
		/* Imports first: Xenia runs a tail call's target as the caller (xenia:src/xenia/cpu/ppc/ppc_scanner.cc:204). */
		*(.text.xsdk_thunk_*)
		*(.text .text.*)
	}

	. = ALIGN(0x10000);
	.init_array : {
		KEEP(*(.preinit_array .init_array .init_array.* .fini_array .fini_array.*))
	}
	ASSERT(SIZEOF(.init_array) == 0, "a DLL's constructors and destructors would never run: nothing calls them")
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
	/* What XSDK_EXPORT lists; elf2xex makes it the export table. */
	.xsdk.exports 0 : {
		KEEP(*(.xsdk.exports))
	}

	/DISCARD/ : {
		*(.comment .note.* .gnu.attributes)
	}
}
