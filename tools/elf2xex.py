#!/usr/bin/env python3
"""Convert a title ELF linked with src/xsdk.ld, or a DLL's, into an unencrypted XEX2, its pages of zeros left out."""

import argparse
import hashlib
import struct
import sys
from pathlib import Path

ELF_HEADER = struct.Struct(">16sHHIIIIIHHHHHH")
ELF_SECTION = struct.Struct(">IIIIIIIIII")
ET_EXEC = 2
EM_PPC = 20
SHF_WRITE = 0x1
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4
SHT_NOBITS = 8

# Titles load here; a DLL gives its own with --base, clear of the title's and the other modules'. The functions below
# take the image's base as an argument.
TITLE_BASE = 0x82000000
PE_HEADER_BYTES = 0x1000
# Image pages are 64 KB unless the image asks for 4 KB (xenia:src/xenia/kernel/util/xex2_info.h:76).
PAGE_BYTES = 0x10000
# Images with 64 KB pages load from 0x80000000 to 0x8FFFFFFF; from 0x90000000 they take 4 KB pages, which a DLL's page
# descriptors here don't describe (xenia:src/xenia/memory.cc:45-47, 165-168).
LARGE_PAGE_IMAGES_START = 0x80000000
LARGE_PAGE_IMAGES_END = 0x90000000

# xenia:src/xenia/kernel/util/xex2_info.h:55-59
SECTION_CODE = 1
SECTION_DATA = 2
SECTION_READONLY = 3

# xenia:src/xenia/kernel/util/xex2_info.h:278-308
HEADER_RESOURCE_INFO = 0x000002FF
HEADER_FILE_FORMAT_INFO = 0x000003FF
HEADER_ENTRY_POINT = 0x00010100
HEADER_IMAGE_BASE_ADDRESS = 0x00010201
HEADER_IMPORT_LIBRARIES = 0x000103FF
HEADER_TLS_INFO = 0x00020104
HEADER_DEFAULT_STACK_SIZE = 0x00020200
HEADER_SYSTEM_FLAGS = 0x00030000
HEADER_EXECUTION_INFO = 0x00040006
HEADER_EXPORTS_BY_NAME = 0x00E10402

MODULE_TITLE = 0x00000001
# xenia:src/xenia/kernel/util/xex2_info.h:117. The platform's DLLs carry both flags, and no TLS or stack size headers
# (xcoreclr.dll, agcore.dll and the .ni.dll files of the Plex 0.0.0.7 runtime).
MODULE_DLL = 0x00000008
SYSTEM_NO_FORCED_REBOOT = 0x00000001
SYSTEM_HANDLE_GAMEPAD_DISCONNECT = 0x00000020
REGION_ALL = 0xFFFFFFFF
MEDIA_ALL = 0xFFFFFFFF
ENCRYPTION_NONE = 0
# Basic compression: blocks of data, each followed by zeros the loader fills in rather than reads
# (xenia:src/xenia/kernel/util/xex2_info.h:345-348, xenia:src/xenia/cpu/xex_module.cc:564-648).
COMPRESSION_BASIC = 1
ZERO_RUN_BYTES = 0x1000
TLS_SLOTS = 64

SECURITY_INFO_BYTES = 0x184
PAGE_DESCRIPTOR_BYTES = 0x18
IMAGE_INFO_BYTES = 0x174
HEADER_ALIGN_BYTES = 0x1000

# xenia:third_party/pe/pe_image.h and the section flags at xenia:src/xenia/kernel/util/xex2_info.h:27-34
PE_MACHINE_POWERPCBE = 0x01F2
PE_EXECUTABLE_IMAGE = 0x0002
PE_RELOCS_STRIPPED = 0x0001
PE_32BIT_MACHINE = 0x0100
PE_OPTIONAL_HEADER_BYTES = 224
PE_OPTIONAL_MAGIC = 0x10B
PE_SUBSYSTEM_XBOX = 14
PE_SECTION_FLAGS = {
    SECTION_CODE: 0x60000020,
    SECTION_READONLY: 0x40000040,
    SECTION_DATA: 0xC0000040,
}
PE_SECTION_NAMES = {SECTION_CODE: b".text", SECTION_READONLY: b".rdata", SECTION_DATA: b".data"}

# A title's name is string 0x8000 of its default language in an XDBF database, in the resource named after its title
# ID (xenia:src/xenia/kernel/kernel_state.cc:111-125, user_module.cc:267-288, util/xex2_info.h:427-437, and
# util/xdbf_utils.h:26-77, xdbf_utils.cc:16-24, 63-87, 123-135). Xenia reads neither the database's version and free
# table nor the sizes in XSTC and XSTR.
XDBF_SECTION_METADATA = 1
XDBF_SECTION_STRING_TABLE = 3
XDBF_ID_XSTC = 0x58535443
XDBF_ID_TITLE = 0x8000
XDBF_VERSION = 0x10000
# xenia:src/xenia/xbox.h:330-332
LANGUAGE_ENGLISH = 1
# The database goes in the first page after the PE headers, below the byte tests/xex_checks.py flips.
TITLE_NAME_END = 0x800

IMPORT_RECORD = struct.Struct(">IIII16s")
RECORD_THUNK = 0x01000000
RECORD_THUNK_SECOND = 0x02000000

EXPORT_RECORD = struct.Struct(">II")
# A DLL's export table, which the security info points to at 0x160: three magic words, a module number and a version
# left 0, the image base's top half, the count, the first ordinal (1), then each ordinal's address less the image base,
# 0 for an ordinal not exported (xenia:src/xenia/kernel/util/xex2_info.h:582, 607-616). The magic and the zeros are as
# the Plex 0.0.0.7 runtime's xcoreclr.dll and agcore.dll have them; XAM's own, in xam.xex 2.0.17559, has the same
# magic and fills the module number and version. Xenia reads the base, the count
# and the offsets, not the rest (xenia:src/xenia/cpu/xex_module.cc:123-138).
EXPORT_MAGIC = (0x48000000, 0x00485645, 0x48000000)
FIRST_ORDINAL = 1
SHT_SYMTAB = 2
ELF_SYMBOL = struct.Struct(">IIIBBH")


def version(major, minor, build, qfe):
    return major << 28 | minor << 24 | build << 8 | qfe


# Per-library import IDs; how they're derived is unknown (SynthXEX aee2b8c6 src/setdata/optheaders.c:159-167).
LIBRARY_IDS = {"xboxkrnl.exe": 0x45DC17E0, "xam.xex": 0xFCA15C76}

# The versions a working XDK-built homebrew title carries (XexTool listing on the user's console files).
IMPORT_VERSION = version(2, 0, 21256, 0)
IMPORT_VERSION_MIN = version(2, 0, 1861, 0)


def parse_version(text):
    parts = [int(part) for part in text.split(".")]
    if len(parts) != 4 or not all(0 <= part < 1 << bits for part, bits in zip(parts, (4, 4, 16, 8))):
        raise argparse.ArgumentTypeError(f"{text}: want major.minor.build.fix, in 4, 4, 16 and 8 bits")
    return version(*parts)


def fail(message):
    sys.exit(f"elf2xex: {message}")


def align(value, alignment):
    return (value + alignment - 1) & ~(alignment - 1)


def sha1(*parts):
    digest = hashlib.sha1()
    for part in parts:
        digest.update(part)
    return digest.digest()


def read_elf(data):
    header = ELF_HEADER.unpack_from(data)
    ident, e_type, machine = header[0], header[1], header[2]
    if ident[:4] != b"\x7fELF" or ident[4] != 1 or ident[5] != 2:
        fail("not a 32-bit big-endian ELF")
    if e_type != ET_EXEC or machine != EM_PPC:
        fail("not a PowerPC executable")
    entry, section_offset = header[4], header[6]
    section_count, names_index = header[12], header[13]

    raw = [ELF_SECTION.unpack_from(data, section_offset + i * ELF_SECTION.size) for i in range(section_count)]
    names_offset = raw[names_index][4]
    sections = []
    for name, kind, flags, address, offset, size, *_ in raw:
        end = data.index(b"\0", names_offset + name)
        sections.append({
            "name": data[names_offset + name:end].decode(),
            "kind": kind,
            "flags": flags,
            "address": address,
            "data": b"" if kind == SHT_NOBITS else data[offset:offset + size],
            "size": size,
        })
    return entry, sections


def section_type(section):
    if section["flags"] & SHF_EXECINSTR:
        return SECTION_CODE
    if section["flags"] & SHF_WRITE:
        return SECTION_DATA
    return SECTION_READONLY


def build_image(sections, base):
    loaded = [s for s in sections if s["flags"] & SHF_ALLOC and s["size"]]
    for section in loaded:
        if section["address"] < base + PE_HEADER_BYTES:
            fail(f"section {section['name']} overlaps the PE headers")
    end = max(s["address"] + s["size"] for s in loaded)
    image = bytearray(align(end - base, PAGE_BYTES))
    page_types = [SECTION_READONLY] + [None] * (len(image) // PAGE_BYTES - 1)

    for section in loaded:
        start = section["address"] - base
        image[start:start + len(section["data"])] = section["data"]
        first_page = start // PAGE_BYTES
        last_page = (start + section["size"] - 1) // PAGE_BYTES
        for page in range(first_page, last_page + 1):
            if page_types[page] not in (None, section_type(section)):
                fail(f"section {section['name']} shares a 64 KB page with a section of another type")
            page_types[page] = section_type(section)

    runs = []
    for page, kind in enumerate(page_types):
        kind = kind or SECTION_READONLY
        if runs and runs[-1]["type"] == kind:
            runs[-1]["pages"] += 1
        else:
            runs.append({"type": kind, "first_page": page, "pages": 1})
    for run in runs:
        start = base + run["first_page"] * PAGE_BYTES
        end = start + run["pages"] * PAGE_BYTES
        inside = [s for s in loaded if start <= s["address"] < end]
        run["start"] = min(s["address"] for s in inside) - base if inside else None
        run["end"] = max(s["address"] + s["size"] for s in inside) - base if inside else None
        run["initialized"] = sum(len(s["data"]) for s in inside)
        run["uninitialized"] = sum(s["size"] - len(s["data"]) for s in inside)
    return image, runs


def read_imports(sections, image, base):
    found = [s for s in sections if s["name"] == ".xsdk.imports"]
    if not found:
        return {}
    data = found[0]["data"]
    libraries = {}
    for offset in range(0, len(data), IMPORT_RECORD.size):
        slot, thunk, ordinal, _, library = IMPORT_RECORD.unpack_from(data, offset)
        name = library.rstrip(b"\0").decode()
        libraries.setdefault(name, []).append({"slot": slot, "thunk": thunk, "ordinal": ordinal})
    for name, imports in libraries.items():
        imports.sort(key=lambda entry: entry["ordinal"])
        for entry in imports:
            addresses = [entry["slot"]]
            if entry["thunk"]:
                addresses.append(entry["thunk"])
            for address in addresses:
                word = struct.unpack_from(">I", image, address - base)[0]
                if word & 0xFFFF != entry["ordinal"]:
                    fail(f"{name} ordinal {entry['ordinal']:#x}: record at {address:#010x} reads {word:#010x}")
    return dict(sorted(libraries.items()))


def read_exports(sections, image, base):
    found = [s for s in sections if s["name"] == ".xsdk.exports"]
    if not found:
        return {}
    data = found[0]["data"]
    exports = {}
    for offset in range(0, len(data), EXPORT_RECORD.size):
        ordinal, address = EXPORT_RECORD.unpack_from(data, offset)
        if ordinal < FIRST_ORDINAL or ordinal in exports:
            fail(f"export ordinal {ordinal} is below {FIRST_ORDINAL} or exported twice")
        if not base + PE_HEADER_BYTES <= address < base + len(image):
            fail(f"export ordinal {ordinal} at {address:#010x} is outside the image")
        exports[ordinal] = address
    return exports


def add_export_table(image, offset, exports, base):
    """Returns the table's address and where the image's headers end after it."""
    ordinals = range(FIRST_ORDINAL, max(exports) + 1)
    words = [*EXPORT_MAGIC, 0, 0, 0, 0, 0, base >> 16, len(ordinals), FIRST_ORDINAL]
    words += [exports[ordinal] - base if ordinal in exports else 0 for ordinal in ordinals]
    table = struct.pack(f">{len(words)}I", *words)
    offset = align(offset, 16)
    if offset + len(table) > PE_HEADER_BYTES:
        fail("the export table doesn't fit beside the PE headers")
    image[offset:offset + len(table)] = table
    return base + offset, offset + len(table)


def read_symbols(data):
    """Returns {name: address} of the ELF's symbol table."""
    header = ELF_HEADER.unpack_from(data)
    section_offset, section_count = header[6], header[12]
    raw = [ELF_SECTION.unpack_from(data, section_offset + i * ELF_SECTION.size) for i in range(section_count)]
    symbols = {}
    for _, kind, _, _, offset, size, link, _, _, _ in raw:
        if kind != SHT_SYMTAB:
            continue
        strings = raw[link][4]
        for at in range(offset, offset + size, ELF_SYMBOL.size):
            name, value = ELF_SYMBOL.unpack_from(data, at)[:2]
            end = data.index(b"\0", strings + name)
            symbols[data[strings + name:end].decode()] = value
    return symbols


def read_named_exports(items, elf, image, base):
    """Returns {name: address} for --export-name's NAME or NAME=SYMBOL items."""
    symbols = read_symbols(elf)
    named = {}
    for item in items:
        name, _, symbol = item.partition("=")
        symbol = symbol or name
        if symbol not in symbols:
            fail(f"--export-name {item}: the ELF has no symbol {symbol}")
        if name in named:
            fail(f"--export-name {name} is given twice")
        if not base + PE_HEADER_BYTES <= symbols[symbol] < base + len(image):
            fail(f"--export-name {item}: {symbol} at {symbols[symbol]:#010x} is outside the image")
        named[name] = symbols[symbol]
    return named


def add_named_exports(image, offset, named, module_name, base):
    """Writes the exports by name as a little-endian PE export directory, in the shape the Plex 0.0.0.7 runtime's
    agcore.dll and npctrl.dll have, and returns its offset and size for header 0x00E10402. AddressOfFunctions,
    AddressOfNames, AddressOfNameOrdinals and the names' entries are relative to the directory, the functions'
    entries and the module's name are image offsets; Xenia reads it the same way (xenia:src/xenia/cpu/
    xex_module.cc:158-195), and XexGetProcedureAddress takes a name's address in place of an ordinal."""
    names = sorted(named)
    count = len(names)
    functions_at = 40
    names_at = functions_at + 4 * count
    ordinals_at = names_at + 4 * count
    strings_at = ordinals_at + 2 * count
    directory = align(offset, 16)
    strings = b""
    name_offsets = []
    for name in names:
        name_offsets.append(strings_at + len(strings))
        strings += name.encode() + b"\0"
    module_name_offset = directory + strings_at + len(strings)
    strings += module_name.encode() + b"\0"
    table = struct.pack("<IIHHIIIIIII", 0, 0, 0, 0, module_name_offset, FIRST_ORDINAL, count, count, functions_at,
        names_at, ordinals_at)
    table += struct.pack(f"<{count}I", *[named[name] - base for name in names])
    table += struct.pack(f"<{count}I", *name_offsets)
    table += struct.pack(f"<{count}H", *range(count))
    table += strings
    if directory + len(table) > PE_HEADER_BYTES:
        fail("the exports by name don't fit beside the PE headers")
    image[directory:directory + len(table)] = table
    return directory, len(table)


def import_libraries_header(libraries):
    names = list(libraries)
    strings = b"".join(align_bytes(name.encode() + b"\0", 4) for name in names)

    records = []
    for index, name in enumerate(names):
        if name not in LIBRARY_IDS:
            fail(f"no import ID known for {name}")
        table = []
        for entry in libraries[name]:
            table.append(entry["slot"])
            if entry["thunk"]:
                table.append(entry["thunk"])
        body = struct.pack(">IIIHH", LIBRARY_IDS[name], IMPORT_VERSION, IMPORT_VERSION_MIN, index, len(table))
        body += struct.pack(f">{len(table)}I", *table)
        records.append(body)

    # Each record's digest covers the next record after its size; the security info's digest covers the first.
    next_digest = bytes(20)
    chained = []
    for body in reversed(records):
        chained.append(struct.pack(">I", 4 + 20 + len(body)) + next_digest + body)
        next_digest = sha1(next_digest, body)
    chained.reverse()

    block = strings + b"".join(chained)
    header = struct.pack(">III", 12 + len(block), len(strings), len(names)) + block
    return header, next_digest


def align_bytes(data, alignment):
    return data + bytes(align(len(data), alignment) - len(data))


def pe_headers(image, runs, entry, stack_bytes, base):
    sections = [run for run in runs if run["start"] is not None]
    code = [run for run in sections if run["type"] == SECTION_CODE]
    data = [run for run in sections if run["type"] != SECTION_CODE]
    file_header = struct.pack("<HHIIIHH", PE_MACHINE_POWERPCBE, len(sections), 0, 0, 0, PE_OPTIONAL_HEADER_BYTES,
        PE_EXECUTABLE_IMAGE | PE_RELOCS_STRIPPED | PE_32BIT_MACHINE)
    optional = struct.pack("<HBBIIIIIII", PE_OPTIONAL_MAGIC, 0, 0,
        sum(run["initialized"] for run in code),
        sum(run["initialized"] for run in data),
        sum(run["uninitialized"] for run in sections),
        entry - base,
        code[0]["start"] if code else 0,
        data[0]["start"] if data else 0,
        base)
    optional += struct.pack("<IIHHHHHHIIIIHHIIIIII", PE_HEADER_BYTES, PE_HEADER_BYTES, 0, 0, 0, 0, 0, 0, 0, len(image),
        PE_HEADER_BYTES, 0, PE_SUBSYSTEM_XBOX, 0, stack_bytes, stack_bytes, 0, 0, 0, 16)
    optional += bytes(16 * 8)

    section_headers = b""
    for run in sections:
        size = run["end"] - run["start"]
        section_headers += struct.pack("<8sIIIIIIHHI", PE_SECTION_NAMES[run["type"]], size, run["start"], size,
            run["start"], 0, 0, 0, 0, PE_SECTION_FLAGS[run["type"]])

    dos_header = bytearray(0x40)
    # The loader checks the first four bytes, not just "MZ" (xenia:src/xenia/cpu/xex_module.h:164).
    dos_header[0:4] = b"MZ\x90\x00"
    struct.pack_into("<I", dos_header, 0x3C, len(dos_header))
    headers = bytes(dos_header) + b"PE\0\0" + file_header + optional + section_headers
    if len(headers) > PE_HEADER_BYTES:
        fail("PE headers don't fit in their page")
    image[0:len(headers)] = headers
    return len(headers)


def title_database(name):
    text = name.encode()
    strings = struct.pack(">HH", XDBF_ID_TITLE, len(text)) + text
    xstr = struct.pack(">4sIIH", b"XSTR", 1, 2 + len(strings), 1) + strings
    xstc = struct.pack(">4sIII", b"XSTC", 1, 4, LANGUAGE_ENGLISH)
    entries = struct.pack(">HQII", XDBF_SECTION_METADATA, XDBF_ID_XSTC, 0, len(xstc))
    entries += struct.pack(">HQII", XDBF_SECTION_STRING_TABLE, LANGUAGE_ENGLISH, len(xstc), len(xstr))
    content_bytes = len(xstc) + len(xstr)
    free = struct.pack(">II", content_bytes, 0xFFFFFFFF - content_bytes)
    return struct.pack(">4sIIIII", b"XDBF", XDBF_VERSION, 2, 2, 1, 1) + entries + free + xstc + xstr


def add_title_name(image, headers_end, title_id, name, base):
    database = title_database(name)
    offset = align(headers_end, 16)
    if offset + len(database) > TITLE_NAME_END:
        fail("the title name doesn't fit beside the PE headers")
    image[offset:offset + len(database)] = database
    return struct.pack(">I8sII", 4 + 16, f"{title_id:08X}".encode(), base + offset, len(database))


def page_descriptors(image, runs):
    # One descriptor per page, as a working XDK-built title has (user's console files).
    page_types = [run["type"] for run in runs for _ in range(run["pages"])]
    # Each descriptor's digest covers the next page and its descriptor; the security info's digest covers the first.
    next_digest = bytes(20)
    descriptors = []
    for page in reversed(range(len(page_types))):
        descriptor = struct.pack(">I", 1 << 4 | page_types[page]) + next_digest
        descriptors.append(descriptor)
        next_digest = sha1(image[page * PAGE_BYTES:(page + 1) * PAGE_BYTES], descriptor)
    descriptors.reverse()
    return b"".join(descriptors), next_digest


def compression_blocks(image):
    """Splits the image into [data, zeros] blocks at 4 KB pages, the zeros at its end left out of the blocks."""
    blocks = []
    zero_page = bytes(ZERO_RUN_BYTES)
    for offset in range(0, len(image), ZERO_RUN_BYTES):
        if image[offset:offset + ZERO_RUN_BYTES] == zero_page and blocks:
            blocks[-1][1] += ZERO_RUN_BYTES
        elif blocks and not blocks[-1][1]:
            blocks[-1][0] += ZERO_RUN_BYTES
        else:
            blocks.append([ZERO_RUN_BYTES, 0])
    # The console starts no title whose last block ends in zeros, and fills what the blocks leave of the image with
    # zeros (docs/HARDWARE.md, "Zero runs in a .xex, milestone 63").
    blocks[-1][1] = 0
    return blocks


def file_data(image, blocks):
    data = bytearray()
    offset = 0
    for data_bytes, zero_bytes in blocks:
        data += image[offset:offset + data_bytes]
        offset += data_bytes + zero_bytes
    return data


def expand(blocks, data, image_bytes):
    image = bytearray()
    offset = 0
    for data_bytes, zero_bytes in blocks:
        image += data[offset:offset + data_bytes] + bytes(zero_bytes)
        offset += data_bytes
    return image + bytes(image_bytes - len(image))


def optional_header_table(inline, blocks, data_offset, placed):
    keys = sorted(list(inline) + list(blocks) + list(placed))
    table = b""
    data = b""
    for key in keys:
        if key in inline:
            table += struct.pack(">II", key, inline[key])
        elif key in placed:
            table += struct.pack(">II", key, placed[key])
        else:
            table += struct.pack(">II", key, data_offset + len(data))
            data += align_bytes(blocks[key], 8)
    return table, data


def build_xex(image, runs, entry, libraries, resources, export_table, named_directory, args):
    # A module with no imports has no import libraries header, as the platform's .ni.dll files don't.
    imports, import_digest = import_libraries_header(libraries) if libraries else (b"", bytes(20))
    descriptors, image_digest = page_descriptors(image, runs)
    compression = compression_blocks(image)
    image_data = file_data(image, compression)
    assert expand(compression, image_data, len(image)) == image

    inline = {
        HEADER_ENTRY_POINT: entry,
        HEADER_IMAGE_BASE_ADDRESS: args.base,
        HEADER_SYSTEM_FLAGS: SYSTEM_NO_FORCED_REBOOT | SYSTEM_HANDLE_GAMEPAD_DISCONNECT,
    }
    blocks = {
        HEADER_FILE_FORMAT_INFO: struct.pack(">IHH", 8 + 8 * len(compression), ENCRYPTION_NONE, COMPRESSION_BASIC)
            + b"".join(struct.pack(">II", *block) for block in compression),
        HEADER_EXECUTION_INFO: struct.pack(">IIIIBBBBI", 0, args.version, 0, args.title_id, 0, 0, 1, 1, 0),
    }
    if not args.dll:
        inline[HEADER_DEFAULT_STACK_SIZE] = args.stack_size
        blocks[HEADER_TLS_INFO] = struct.pack(">IIII", TLS_SLOTS, 0, 0, 0)
    if resources:
        blocks[HEADER_RESOURCE_INFO] = resources
    if named_directory:
        blocks[HEADER_EXPORTS_BY_NAME] = struct.pack(">II", *named_directory)
    placed = {HEADER_IMPORT_LIBRARIES: 0} if libraries else {}

    header_count = len(inline) + len(blocks) + len(placed)
    security_offset = align(0x18 + 8 * header_count, 8)
    security_bytes = SECURITY_INFO_BYTES + len(descriptors)
    data_offset = align(security_offset + security_bytes, 8)
    data_bytes = sum(align(len(block), 8) for block in blocks.values())
    header_size = align(data_offset + data_bytes + len(imports), HEADER_ALIGN_BYTES)
    # The import libraries end the headers exactly, as XexTool and SynthXEX (placer.c:72-106) place them.
    import_offset = header_size - len(imports)
    placed = {key: import_offset for key in placed}
    table, data = optional_header_table(inline, blocks, data_offset, placed)

    security = bytearray(SECURITY_INFO_BYTES)
    struct.pack_into(">II", security, 0, security_bytes, len(image))
    struct.pack_into(">III20sI20s", security, 0x108, IMAGE_INFO_BYTES, 0, args.base, image_digest, len(libraries),
        import_digest)
    struct.pack_into(">I", security, 0x160, export_table)
    struct.pack_into(">III", security, 0x178, REGION_ALL, MEDIA_ALL, len(descriptors) // PAGE_DESCRIPTOR_BYTES)

    module_flags = MODULE_TITLE | MODULE_DLL if args.dll else MODULE_TITLE
    header = bytearray(header_size)
    struct.pack_into(">4sIIIII", header, 0, b"XEX2", module_flags, header_size, 0, security_offset, header_count)
    header[0x18:0x18 + len(table)] = table
    header[security_offset:security_offset + SECURITY_INFO_BYTES] = security
    header[security_offset + SECURITY_INFO_BYTES:security_offset + security_bytes] = descriptors
    header[data_offset:data_offset + len(data)] = data
    header[import_offset:] = imports

    # The header digest covers everything the signature doesn't.
    header_digest = sha1(header[security_offset + 0x17C:], header[:security_offset + 8])
    header[security_offset + 0x164:security_offset + 0x178] = header_digest
    return bytes(header) + image_data


def patch_import_records(image, libraries, base):
    for index, imports in enumerate(libraries.values()):
        for entry in imports:
            records = [(entry["slot"], 0)]
            if entry["thunk"]:
                records += [(entry["thunk"], RECORD_THUNK), (entry["thunk"] + 4, RECORD_THUNK_SECOND)]
            for address, kind in records:
                struct.pack_into(">I", image, address - base, kind | index << 16 | entry["ordinal"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", type=Path)
    parser.add_argument("xex", type=Path)
    parser.add_argument("--title-id", type=lambda text: int(text, 16), default=0x5853444B)
    parser.add_argument("--stack-size", type=lambda text: int(text, 0), default=1024 * 1024)
    parser.add_argument("--title-name", help="the name the dashboard shows")
    parser.add_argument("--version", type=parse_version, default=0, help="the title's version, as 1.2.300.4")
    parser.add_argument("--dll", action="store_true", help="a DLL, exporting what its .xsdk.exports section lists")
    parser.add_argument("--base", type=lambda text: int(text, 16), default=TITLE_BASE, help="where the image loads")
    parser.add_argument("--export-name", action="append", default=[], metavar="NAME[=SYMBOL]",
        help="a DLL's: export the ELF symbol SYMBOL (default: NAME) by name too, as NAME")
    args = parser.parse_args()
    if args.dll and args.title_name:
        parser.error("a DLL has no title name")
    if args.base != TITLE_BASE and not args.dll:
        parser.error("a title loads at 0x82000000, as src/xsdk.ld links it; --base is for a DLL")
    if args.base % PAGE_BYTES:
        parser.error("--base must be a multiple of 64 KB")
    if args.export_name and not args.dll:
        parser.error("only a DLL exports: --export-name is for a DLL")

    base = args.base
    elf = args.elf.read_bytes()
    entry, sections = read_elf(elf)
    first = min(s["address"] for s in sections if s["flags"] & SHF_ALLOC and s["size"])
    if not base + PE_HEADER_BYTES <= first < base + PAGE_BYTES:
        fail(f"the ELF is linked at {first - PE_HEADER_BYTES:#010x}, not at {base:#010x}: "
            "give elf2xex the base it was linked at")
    image, runs = build_image(sections, base)
    if not (LARGE_PAGE_IMAGES_START <= base and base + len(image) <= LARGE_PAGE_IMAGES_END):
        fail(f"the image, {base:#010x} to {base + len(image):#010x}, must lie within "
            f"{LARGE_PAGE_IMAGES_START:#010x} to {LARGE_PAGE_IMAGES_END:#010x}, where images take 64 KB pages")
    libraries = read_imports(sections, image, base)
    exports = read_exports(sections, image, base)
    if exports and not args.dll:
        fail("only a DLL exports")
    patch_import_records(image, libraries, base)
    headers_end = pe_headers(image, runs, entry, args.stack_size, base)
    resources = add_title_name(image, headers_end, args.title_id, args.title_name, base) if args.title_name else b""
    export_table = 0
    if exports:
        export_table, headers_end = add_export_table(image, headers_end, exports, base)
    named_directory = None
    if args.export_name:
        named = read_named_exports(args.export_name, elf, image, base)
        named_directory = add_named_exports(image, headers_end, named, args.xex.name, base)
    args.xex.write_bytes(build_xex(image, runs, entry, libraries, resources, export_table, named_directory, args))


if __name__ == "__main__":
    main()
