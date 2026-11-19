"""Build a native x86-64 Windows executable with GCC and GNU binutils."""

from pathlib import Path
import re
import struct
import subprocess

from icon_art import icon_resources


ROOT = Path(__file__).resolve().parent
OUT = ROOT / "build"
IMPORTS = {
    "msvcrt.dll": (
        "malloc", "realloc", "free", "memcpy", "memcmp", "memset", "strlen", "strcmp",
        "fread", "fwrite", "fclose", "ferror", "_wfopen",
    ),
    "kernel32.dll": (
        "GetModuleHandleW", "GetFileAttributesW", "FindFirstFileW", "FindNextFileW",
        "FindClose", "GetLastError", "MultiByteToWideChar", "WideCharToMultiByte",
        "ExitProcess", "GetTempFileNameW", "MoveFileExW", "DeleteFileW",
    ),
    "user32.dll": (
        "RegisterClassExW", "CreateWindowExW", "DefWindowProcW", "ShowWindow",
        "UpdateWindow", "GetMessageW", "TranslateMessage", "DispatchMessageW",
        "PostQuitMessage", "SendMessageW", "GetWindowTextW", "SetWindowTextW",
        "MessageBoxW", "LoadCursorW", "EnableWindow", "SetFocus", "IsDialogMessageW",
        "BeginPaint", "EndPaint", "FillRect", "DrawTextW", "DrawFocusRect",
        "LoadImageW", "DrawIconEx", "DestroyIcon",
    ),
    "gdi32.dll": ("CreateFontW", "DeleteObject", "CreateSolidBrush", "SetTextColor",
                  "SetBkColor", "SetBkMode", "SelectObject"),
    "comdlg32.dll": ("GetOpenFileNameW", "GetSaveFileNameW", "CommDlgExtendedError"),
    "shell32.dll": ("ShellExecuteW",),
    "ole32.dll": ("CoInitializeEx", "CoUninitialize", "CoCreateInstance", "CoTaskMemFree"),
}


def c_string(data, start):
    return bytes(data[start:data.index(0, start)]).decode("ascii")


def convert_object(elf_path, coff_path):
    """Carry the ELF relocation kind and explicit addend into COFF."""
    elf = elf_path.read_bytes()
    coff = bytearray(coff_path.read_bytes())
    shoff, shentsize, shnum, shstrndx = (
        struct.unpack_from("<Q", elf, 40)[0],
        *struct.unpack_from("<HHH", elf, 58),
    )
    section_headers = [
        struct.unpack_from("<IIQQQQIIQQ", elf, shoff + i * shentsize)
        for i in range(shnum)
    ]
    names_at = section_headers[shstrndx][4]
    fixups = {}
    for header in section_headers:
        if header[1] != 4:  # SHT_RELA
            continue
        name = c_string(elf, names_at + section_headers[header[7]][0])
        section_fixups = {}
        symtab = section_headers[header[6]]
        for pos in range(header[4], header[4] + header[5], header[9]):
            offset, info, addend = struct.unpack_from("<QQq", elf, pos)
            kind = info & 0xffffffff
            sym = symtab[4] + (info >> 32) * symtab[9]
            section_symbol = None
            if elf[sym + 4] & 15 == 3:  # STT_SECTION
                shndx = struct.unpack_from("<H", elf, sym + 6)[0]
                section_symbol = c_string(elf, names_at + section_headers[shndx][0])
            if kind in (2, 4):  # ELF PC32 / PLT32 to COFF REL32
                section_fixups[offset] = (4, "<i", addend + 4, section_symbol)
            elif kind == 1:
                section_fixups[offset] = (1, "<q", addend, section_symbol)
            elif kind in (10, 11):
                section_fixups[offset] = (2, "<i", addend, section_symbol)
            else:
                raise ValueError(f"Unsupported ELF relocation {kind} in {name}")
        fixups[name] = section_fixups
    count = struct.unpack_from("<H", coff, 2)[0]
    symbols, symbol_count = struct.unpack_from("<II", coff, 8)
    strings = symbols + 18 * symbol_count
    coff_sections = {}
    for i in range(count):
        header_at = 20 + 40 * i
        name = bytes(coff[header_at:header_at + 8]).rstrip(b"\0").decode("ascii")
        if re.fullmatch(r"/\d+", name):
            name = c_string(coff, strings + int(name[1:]))
        coff_sections[name] = (i + 1, header_at)
    section_symbols = {}
    extra_symbols = bytearray()
    for name, (_, header_at) in coff_sections.items():
        raw, reloc, _, reloc_count = struct.unpack_from("<IIIH", coff, header_at + 20)
        todo = fixups.pop(name, {}).copy()
        for n in range(reloc_count):
            at = reloc + 10 * n
            offset = struct.unpack_from("<I", coff, at)[0]
            kind, fmt, addend, target = todo.pop(offset)
            struct.pack_into(fmt, coff, raw + offset, addend)
            struct.pack_into("<H", coff, at + 8, kind)
            if target is not None:
                # objcopy can map a nameless ELF section symbol to the FILE symbol.
                # Add a real COFF symbol anchored at the target section start.
                if target not in section_symbols:
                    index = len(section_symbols)
                    section_symbols[target] = symbol_count + index
                    marker = f"sect{index}".encode("ascii")
                    extra_symbols += struct.pack("<8sIhHBB", marker, 0, coff_sections[target][0], 0, 3, 0)
                struct.pack_into("<I", coff, at + 4, section_symbols[target])
        if todo:
            raise ValueError(f"Missing COFF relocation for {name}: {todo}")
    if any(fixups.values()):
        raise ValueError(f"Unconverted relocation sections: {fixups}")
    coff[strings:strings] = extra_symbols
    struct.pack_into("<I", coff, 12, symbol_count + len(section_symbols))
    coff_path.write_bytes(coff)


def prepare_imports():
    slots = {}
    next_slot = 64
    for dll, functions in IMPORTS.items():
        slots[dll] = next_slot
        next_slot += len(functions) + 1
    header = "".join(
        f"#define SLOT_{name} {slots[dll] + i}\n"
        for dll, names in IMPORTS.items()
        for i, name in enumerate(names)
    )
    (OUT / "import_slots.h").write_text(header, encoding="ascii")
    return slots, next_slot


def version_resource():
    def aligned(data):
        return data + b"\0" * (-len(data) % 4)

    def block(name, value=b"", value_type=0, children=()):
        name_utf16 = (name + "\0").encode("utf-16-le")
        value_length = len(value) // (2 if value_type else 1)
        content = aligned(b"\0" * 6 + name_utf16) + value
        for child in children:
            content = aligned(content) + child
        return struct.pack("<HHH", len(content), value_length, value_type) + content[6:]

    metadata = {
        "CompanyName": "小tt",
        "FileDescription": "C 函数差异导出（脱敏版）",
        "FileVersion": "1.6.0.0",
        "InternalName": "FunctionDiff",
        "OriginalFilename": "FunctionDiff.exe",
        "ProductName": "FunctionDiff",
        "ProductVersion": "1.6.0.0",
    }
    strings = [block(key, (value + "\0").encode("utf-16-le"), 1) for key, value in metadata.items()]
    tables = [block(f"{lang:04X}04B0", value_type=1, children=strings) for lang in (0x0804, 0x0409)]
    translations = struct.pack("<4H", 0x0804, 1200, 0x0409, 1200)
    version = block(
        "VS_VERSION_INFO",
        struct.pack("<13I", 0xfeef04bd, 0x10000, 0x10006, 0, 0x10006, 0, 0x3f, 0,
                    0x40004, 1, 0, 0, 0),
        children=(
            block("StringFileInfo", value_type=1, children=tables),
            block("VarFileInfo", value_type=1, children=[block("Translation", translations)]),
        ),
    )

    return version


def resource_blob():
    entries = icon_resources() + [(16, 1, 0x0804, version_resource())]
    tree = {}
    for kind, number, language, data in entries:
        tree.setdefault(kind, {}).setdefault(number, {})[language] = data
    blob = bytearray()
    data_entries = []
    def directory(children):
        at = len(blob)
        identifiers = sorted(children)
        blob.extend(struct.pack("<IIHHHH", 0, 0, 0, 0, 0, len(identifiers)))
        blob.extend(bytes(8 * len(identifiers)))
        for index, key in enumerate(identifiers):
            child = children[key]
            if isinstance(child, dict):
                value = directory(child) | 0x80000000
            else:
                value = len(blob)
                blob.extend(bytes(16))
                data_entries.append((value, child))
            struct.pack_into("<II", blob, at + 16 + index * 8, key, value)
        return at
    directory(tree)
    for at, payload in data_entries:
        blob.extend(bytes(-len(blob) % 4))
        struct.pack_into("<IIII", blob, at, len(blob), len(payload), 0, 0)
        blob.extend(payload)
    return blob, [at for at, _ in data_entries]


def install_imports(executable, slots, end_slot, resource_entries):
    data = bytearray(executable.read_bytes())
    pe = struct.unpack_from("<I", data, 0x3c)[0]
    assert data[pe:pe + 4] == b"PE\0\0"
    count = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    assert struct.unpack_from("<H", data, optional)[0] == 0x20b
    sections = {}
    for index in range(count):
        at = optional + optional_size + 40 * index
        name = bytes(data[at:at + 8]).rstrip(b"\0")
        sections[name] = struct.unpack_from("<IIII", data, at + 8)
    size, rva, disk_size, disk = sections[b".winimp"]
    text_at = end_slot * 8

    def put_string(value, *, hint=False):
        nonlocal text_at
        if text_at % 2:
            text_at += 1
        payload = (b"\0\0" if hint else b"") + value.encode("ascii") + b"\0"
        if text_at + len(payload) > min(size, disk_size):
            raise ValueError("Import section is too small")
        address = text_at
        data[disk + address:disk + address + len(payload)] = payload
        text_at += len(payload)
        return rva + address

    data[disk:disk + disk_size] = b"\0" * disk_size
    for i, (dll, names) in enumerate(IMPORTS.items()):
        first = slots[dll] * 8
        struct.pack_into("<IIIII", data, disk + i * 20, 0, 0, 0, put_string(dll), rva + first)
        for j, name in enumerate(names):
            struct.pack_into("<Q", data, disk + first + j * 8, put_string(name, hint=True))
    directory = optional + 112
    struct.pack_into("<II", data, directory + 8, rva, (len(IMPORTS) + 1) * 20)
    struct.pack_into("<II", data, directory + 8 * 12, rva + 64 * 8, (end_slot - 64) * 8)
    resource_size, resource_rva, _, resource_disk = sections[b".rsrc"]
    for entry in resource_entries:
        at = resource_disk + entry
        offset = struct.unpack_from("<I", data, at)[0]
        struct.pack_into("<I", data, at, resource_rva + offset)
    struct.pack_into("<II", data, directory + 16, resource_rva, resource_size)
    executable.write_bytes(data)


def run(*args):
    subprocess.run(args, cwd=ROOT, check=True)


def main():
    OUT.mkdir(exist_ok=True)
    slots, end_slot = prepare_imports()
    objects = []
    for stem in ("compare", "windows"):
        elf = OUT / f"{stem}.elf.o"
        coff = OUT / f"{stem}.coff.o"
        run(
            "gcc", "-c", str(ROOT / f"{stem}.c"), "-o", str(elf),
            "-DTARGET_WINDOWS", "-I", str(OUT), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-O2", "-mabi=ms", "-mno-red-zone", "-fshort-wchar",
            "-fno-stack-protector", "-fno-asynchronous-unwind-tables", "-fno-unwind-tables",
            "-ffreestanding", "-fpie", "-fno-jump-tables", "-fno-ident",
            "-fno-tree-loop-distribute-patterns",
        )
        run("objcopy", "-O", "pe-x86-64", str(elf), str(coff))
        convert_object(elf, coff)
        objects.append(str(coff))
    raw_version = OUT / "version.bin"
    version_object = OUT / "version.coff.o"
    resources, resource_entries = resource_blob()
    raw_version.write_bytes(resources)
    run("objcopy", "-I", "binary", "-O", "pe-x86-64", "-B", "i386:x86-64",
        "--rename-section", ".data=.rsrc,alloc,load,readonly,data,contents",
        str(raw_version), str(version_object))
    exe = OUT / "FunctionDiff.exe"
    run("ld", "-mi386pep", "--subsystem", "windows:6.1", "--dynamicbase", "--nxcompat",
        "--no-insert-timestamp", "-e", "app_entry", "-o", str(exe), *objects, str(version_object))
    install_imports(exe, slots, end_slot, resource_entries)
    print(exe)


if __name__ == "__main__":
    main()
