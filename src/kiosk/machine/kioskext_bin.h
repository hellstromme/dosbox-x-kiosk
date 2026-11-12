#ifndef DOSBOX_KIOSK_KIOSKEXT_BIN_H
#define DOSBOX_KIOSK_KIOSKEXT_BIN_H

#include <cstdint>
#include <cstddef>

#if defined(C_KIOSK)

// ============================================================================
// KIOSKEXT.COM - 57-Byte DOSBox-X Kiosk Clean Shutdown Utility (ORG 0x0100)
// Compatible with: 8088, 286, 386, 486, Pentium (Real Mode & V86 Mode)
// ============================================================================
// Offset  Bytes                  Instruction
// 0x0100  B4 0D                  mov ah, 0x0D        ; DOS Flush File Buffers
// 0x0102  CD 21                  int 0x21            ; Commit dirty FAT sectors
// 0x0104  B8 01 53               mov ax, 0x5301      ; APM Real-Mode Connect
// 0x0107  31 DB                  xor bx, bx          ; Device ID 0000h (BIOS)
// 0x0109  CD 15                  int 0x15
// 0x010B  72 15                  jc 0x0122           ; Jump +21 to bochs_fallback
// 0x010D  B8 0E 53               mov ax, 0x530E      ; APM Driver Version
// 0x0110  31 DB                  xor bx, bx          ; Device ID 0000h
// 0x0112  B9 01 01               mov cx, 0x0101      ; Request APM v1.1
// 0x0115  CD 15                  int 0x15
// 0x0117  B8 07 53               mov ax, 0x5307      ; APM Set Power State
// 0x011A  BB 01 00               mov bx, 0x0001      ; All devices (0001h)
// 0x011D  B9 03 00               mov cx, 0x0003      ; Power Off (0003h)
// 0x0120  CD 15                  int 0x15            ; Traps in bios.cpp
// bochs_fallback (0x0122 / index 34):
// 0x0122  BA 00 89               mov dx, 0x8900      ; Bochs shutdown port
// 0x0125  BE 31 01               mov si, 0x0131      ; Address of "Shutdown"
// 0x0128  B9 08 00               mov cx, 0x0008      ; 8 bytes
// 0x012B  FC                     cld
// 0x012C  AC                     lodsb               ; AL = [DS:SI++]
// 0x012D  EE                     out dx, al          ; Write to port 0x8900
// 0x012E  E2 FC                  loop 0x012C         ; Repeat 8 times
// 0x0130  C3                     ret                 ; Safety return
// shutdown_str (0x0131 / index 49):
// 0x0131  53 68 75 74 64 6F 77 6E "Shutdown"
// ============================================================================

static constexpr size_t KIOSKEXT_COM_SIZE = 57;
static constexpr uint8_t KIOSKEXT_COM_BYTES[KIOSKEXT_COM_SIZE] = {
    0xB4, 0x0D, 0xCD, 0x21,
    0xB8, 0x01, 0x53, 0x31, 0xDB, 0xCD, 0x15,
    0x72, 0x15,
    0xB8, 0x0E, 0x53, 0x31, 0xDB, 0xB9, 0x01, 0x01, 0xCD, 0x15,
    0xB8, 0x07, 0x53, 0xBB, 0x01, 0x00, 0xB9, 0x03, 0x00, 0xCD, 0x15,
    0xBA, 0x00, 0x89, 0xBE, 0x31, 0x01, 0xB9, 0x08, 0x00,
    0xFC, 0xAC, 0xEE, 0xE2, 0xFC, 0xC3,
    'S', 'h', 'u', 't', 'd', 'o', 'w', 'n'
};

#endif // defined(C_KIOSK)

#endif // DOSBOX_KIOSK_KIOSKEXT_BIN_H
