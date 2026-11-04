#ifndef XSDK_EXPORT_H
#define XSDK_EXPORT_H

// Exports a DLL's function under an ordinal, which a title looks up with XexGetProcedureAddress. elf2xex --dll makes
// the records into the DLL's export table (docs/GUIDE.md, "DLLs"). The function must have C linkage; a record is the
// ordinal, then the address, in a section that isn't loaded, as the import records are (tools/gen_imports.py).
#define XSDK_EXPORT(ordinal, function) \
	__asm__("\t.section .xsdk.exports, \"\"\n\t.long " #ordinal "\n\t.long " #function "\n\t.previous")

#endif
