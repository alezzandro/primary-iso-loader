#!/usr/bin/env bash
# Build a sparse 120 GB qcow2 whose only content is a UEFI GRUB.
# The instance boots this volume first. GRUB then loads the attached ISO's
# own configuration: Assisted Installer discovery (EFI/redhat/grub.cfg) or a
# Red Hat Enterprise Linux DVD (EFI/BOOT/grub.cfg). The installer then
# overwrites this 120 GB volume with the operating system.
#
# The image is assembled without root: grub2-mkimage builds the EFI
# binary, mtools fills a FAT EFI system partition, and parted writes the
# GPT into a sparse raw file before qemu-img converts it to qcow2.
#
# The discovery volume is a separate image:
#   qemu-img convert -f raw -O qcow2 discovery.iso discovery-disk.qcow2
#
# Local boot check, primary disk first:
#   cp /usr/share/edk2/ovmf/OVMF_VARS.fd /tmp/OVMF_VARS.fd
#   qemu-system-x86_64 -m 4096 -machine q35 -accel kvm \
#     -drive if=pflash,format=raw,readonly=on,file=/usr/share/edk2/ovmf/OVMF_CODE.fd \
#     -drive if=pflash,format=raw,file=/tmp/OVMF_VARS.fd \
#     -drive file=primary-boot.qcow2,format=qcow2,if=virtio \
#     -drive file=discovery-disk.qcow2,format=qcow2,if=virtio
set -euo pipefail

VIRTUAL_SIZE=120G
ESP_SIZE_MIB=512
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OUTPUT=${SCRIPT_DIR}/primary-boot.qcow2
GRUB_CFG=${SCRIPT_DIR}/grub.cfg
FORCE=0

usage() {
    echo "Usage: $0 [--output PATH] [--force]"
    echo "Builds a sparse ${VIRTUAL_SIZE} UEFI chainloader qcow2."
}

while [[ $# -gt 0 ]]; do
    case $1 in
        --output)
            OUTPUT=$2
            shift 2
            ;;
        --force)
            FORCE=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

for cmd in qemu-img parted mkfs.vfat grub2-mkimage grub2-script-check mmd mcopy truncate gcc ld python3; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "Missing command: $cmd" >&2
        exit 1
    fi
done

if [[ ! -f $GRUB_CFG ]]; then
    echo "Missing GRUB configuration: $GRUB_CFG" >&2
    exit 1
fi

grub2-script-check "$GRUB_CFG"

if [[ -e $OUTPUT && $FORCE -ne 1 ]]; then
    echo "Refusing to overwrite $OUTPUT (pass --force)." >&2
    exit 1
fi

WORK=$(mktemp -d /tmp/primary-boot-build.XXXXXX)
cleanup() {
    local rc=$?
    rm -rf "$WORK"
    if [[ $rc -ne 0 ]]; then
        rm -f "$OUTPUT"
    fi
    exit "$rc"
}
trap cleanup EXIT

install -d "$WORK/efi/EFI/BOOT"
gcc -ffreestanding -fno-stack-protector -fshort-wchar -mno-red-zone \
    -mgeneral-regs-only -fno-asynchronous-unwind-tables -fPIE -O2 -Wall -Wextra \
    -c "$SCRIPT_DIR/show-mac.c" -o "$WORK/show-mac.o"
ld -nostdlib -static -pie --no-dynamic-linker -T "$SCRIPT_DIR/efi.lds" \
    -o "$WORK/show-mac.elf" "$WORK/show-mac.o"
python3 - "$WORK/show-mac.elf" "$WORK/efi/EFI/BOOT/show-mac.efi" <<'PY'
import struct
import sys
from pathlib import Path

elf = Path(sys.argv[1]).read_bytes()
e_shoff = struct.unpack_from("<Q", elf, 40)[0]
e_shentsize = struct.unpack_from("<H", elf, 58)[0]
e_shnum = struct.unpack_from("<H", elf, 60)[0]
e_shstrndx = struct.unpack_from("<H", elf, 62)[0]

def section_header(index):
    off = e_shoff + index * e_shentsize
    name, _typ, _flags, addr, offset, size, _link, _info, _align, _entsize = struct.unpack_from("<IIQQQQIIQQ", elf, off)
    return name, addr, offset, size

str_name, _str_addr, str_off, str_size = section_header(e_shstrndx)
strtab = elf[str_off:str_off + str_size]
sections = {}
for index in range(e_shnum):
    name_off, addr, offset, size = section_header(index)
    name = strtab[name_off:strtab.index(0, name_off)].decode()
    sections[name] = (addr, elf[offset:offset + size])

text_addr, text = sections[".text"]
data_addr, data = sections[".data"]
if data_addr < text_addr + len(text):
    raise SystemExit("ELF .text and .data overlap; refusing to build the EFI helper")
file_align = 0x200
section_align = 0x1000

def align(value, boundary):
    return (value + boundary - 1) & ~(boundary - 1)

headers = 0x200
text_raw = align(len(text), file_align)
data_raw = align(len(data), file_align)
text_ptr = headers
data_ptr = text_ptr + text_raw
text_rva = section_align
data_rva = text_rva + (data_addr - text_addr)
image_size = align(data_rva + len(data), section_align)

out = bytearray(data_ptr + data_raw)
out[0:2] = b"MZ"
struct.pack_into("<I", out, 0x3C, 0x80)
pe = 0x80
out[pe:pe + 4] = b"PE\0\0"
struct.pack_into("<HHIIIHH", out, pe + 4, 0x8664, 2, 0, 0, 0, 240, 0x0022)
opt = pe + 24
# PE32+ optional header through NumberOfRvaAndSizes. Data directories stay zero.
prefix = struct.pack(
    "<HBBIIIIIQIIHHHHHHIIIIHHQQQQII",
    0x20B, 0, 0,
    len(text), len(data), 0,
    text_rva, text_rva,
    0x400000,
    0x1000, file_align,
    0, 0, 0, 0, 0, 0,
    0,
    image_size, headers, 0,
    10, 0,
    0x100000, 0x1000, 0x100000, 0x1000,
    0, 16,
)
if len(prefix) != 112:
    raise SystemExit(f"optional header prefix is {len(prefix)} bytes, expected 112")
out[opt:opt + len(prefix)] = prefix

def section(name, vsize, va, rawsize, rawptr, flags):
    raw = name.encode().ljust(8, b"\0")
    return raw + struct.pack("<IIIIIIHHI", vsize, va, rawsize, rawptr, 0, 0, 0, 0, flags)

sec = opt + 240
out[sec:sec + 40] = section(".text", len(text), text_rva, text_raw, text_ptr, 0x60000020)
out[sec + 40:sec + 80] = section(".data", len(data), data_rva, data_raw, data_ptr, 0xC0000040)
out[text_ptr:text_ptr + len(text)] = text
out[data_ptr:data_ptr + len(data)] = data
Path(sys.argv[2]).write_bytes(out)
PY
grub2-mkimage \
    -O x86_64-efi \
    -p /EFI/BOOT \
    -o "$WORK/efi/EFI/BOOT/BOOTX64.EFI" \
    part_gpt part_msdos fat iso9660 chain normal boot configfile echo sleep test connectefi \
    linux search search_label gzio ext2 efi_gop efi_uga video_bochs video_cirrus all_video

install -m 0644 "$GRUB_CFG" "$WORK/efi/EFI/BOOT/grub.cfg"
echo "primary-bootloader" > "$WORK/efi/EFI/BOOT/primary-bootloader.marker"

truncate -s "${ESP_SIZE_MIB}M" "$WORK/esp.img"
mkfs.vfat -F 32 -n ESP "$WORK/esp.img" >/dev/null
export MTOOLS_SKIP_CHECK=1
mcopy -s -i "$WORK/esp.img" "$WORK/efi/EFI" ::/

truncate -s "$VIRTUAL_SIZE" "$WORK/disk.raw"
parted -s "$WORK/disk.raw" mklabel gpt
parted -s "$WORK/disk.raw" mkpart ESP fat32 1MiB $((ESP_SIZE_MIB + 1))MiB
parted -s "$WORK/disk.raw" set 1 esp on
parted -s "$WORK/disk.raw" name 1 ESP

dd if="$WORK/esp.img" of="$WORK/disk.raw" bs=1M seek=1 conv=notrunc,sparse status=none

rm -f "$OUTPUT"
qemu-img convert -f raw -O qcow2 "$WORK/disk.raw" "$OUTPUT"

echo "Wrote $OUTPUT"
qemu-img info "$OUTPUT"
