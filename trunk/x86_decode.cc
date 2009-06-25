#include "x86_decode.h"

namespace playground {

unsigned short next_inst(const char **ip, bool is64bit, bool *has_prefix,
                         char **rex_ptr, char **mod_rm_ptr, char **sib_ptr,
                         bool *is_group) {
  enum {
    BYTE_OP      = (1<<1), // 0x02
    IMM          = (1<<2), // 0x04
    IMM_BYTE     = (2<<2), // 0x08
    MEM_ABS      = (3<<2), // 0x0C
    MODE_MASK    = (7<<2), // 0x1C
    MOD_RM       = (1<<5), // 0x20
    STACK        = (1<<6), // 0x40
    GROUP        = (1<<7), // 0x80
    GROUP_MASK   = 0x7F,
  };

  static unsigned char opcode_types[512] = {
    0x23, 0x21, 0x23, 0x21, 0x09, 0x05, 0x01, 0x01, // 0x00  -  0x07
    0x23, 0x21, 0x23, 0x21, 0x09, 0x05, 0x01, 0x00, // 0x08  -  0x0F
    0x23, 0x21, 0x23, 0x21, 0x09, 0x05, 0x01, 0x01, // 0x10  -  0x17
    0x23, 0x21, 0x23, 0x21, 0x09, 0x05, 0x01, 0x01, // 0x18  -  0x1F
    0x23, 0x21, 0x23, 0x21, 0x09, 0x05, 0x00, 0x01, // 0x20  -  0x27
    0x23, 0x21, 0x23, 0x21, 0x09, 0x05, 0x00, 0x01, // 0x28  -  0x2F
    0x23, 0x21, 0x23, 0x21, 0x09, 0x05, 0x00, 0x01, // 0x30  -  0x37
    0x23, 0x21, 0x23, 0x21, 0x09, 0x05, 0x00, 0x01, // 0x38  -  0x3F
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, // 0x40  -  0x47
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, // 0x48  -  0x4F
    0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, // 0x50  -  0x57
    0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, // 0x58  -  0x5F
    0x01, 0x01, 0x21, 0x21, 0x00, 0x00, 0x00, 0x00, // 0x60  -  0x67
    0x45, 0x25, 0x49, 0x29, 0x03, 0x01, 0x03, 0x01, // 0x68  -  0x6F
    0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, // 0x70  -  0x77
    0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, // 0x78  -  0x7F
    0x27, 0x25, 0x27, 0x29, 0x23, 0x21, 0x23, 0x21, // 0x80  -  0x87
    0x23, 0x21, 0x23, 0x21, 0x21, 0x21, 0x21, 0x80, // 0x88  -  0x8F
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, // 0x90  -  0x97
    0x01, 0x01, 0x05, 0x01, 0x41, 0x41, 0x01, 0x01, // 0x98  -  0x9F
    0x0F, 0x0D, 0x0F, 0x0D, 0x03, 0x01, 0x03, 0x01, // 0xA0  -  0xA7
    0x09, 0x05, 0x03, 0x01, 0x03, 0x01, 0x03, 0x01, // 0xA8  -  0xAF
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, // 0xB0  -  0xB7
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, // 0xB8  -  0xBF
    0x27, 0x29, 0x01, 0x01, 0x21, 0x21, 0x27, 0x25, // 0xC0  -  0xC7
    0x01, 0x01, 0x01, 0x01, 0x01, 0x09, 0x01, 0x01, // 0xC8  -  0xCF
    0x23, 0x21, 0x23, 0x21, 0x09, 0x09, 0x01, 0x01, // 0xD0  -  0xD7
    0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, // 0xD8  -  0xDF
    0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, // 0xE0  -  0xE7
    0x05, 0x05, 0x05, 0x09, 0x03, 0x01, 0x03, 0x01, // 0xE8  -  0xEF
    0x00, 0x01, 0x00, 0x00, 0x01, 0x01, 0x88, 0x90, // 0xF0  -  0xF7
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x98, 0xA0, // 0xF8  -  0xFF
    0x00, 0xA8, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, // 0xF00 - 0xF07
    0x01, 0x01, 0x00, 0x01, 0x00, 0x21, 0x01, 0x00, // 0xF08 - 0xF0F
    0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, // 0xF10 - 0xF17
    0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, // 0xF18 - 0xF1F
    0x21, 0x21, 0x21, 0x21, 0x00, 0x00, 0x00, 0x00, // 0xF20 - 0xF27
    0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, // 0xF28 - 0xF2F
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, // 0xF30 - 0xF37
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 0xF38 - 0xF3F
    0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, // 0xF40 - 0xF47
    0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, // 0xF48 - 0xF4F
    0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, // 0xF50 - 0xF57
    0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, // 0xF58 - 0xF5F
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 0xF60 - 0xF67
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 0xF68 - 0xF6F
    0x21, 0x00, 0x00, 0x00, 0x21, 0x21, 0x21, 0x00, // 0xF70 - 0xF77
    0x21, 0x21, 0x00, 0x00, 0x21, 0x21, 0x21, 0x21, // 0xF78 - 0xF7F
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, // 0xF80 - 0xF87
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, // 0xF88 - 0xF8F
    0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, // 0xF90 - 0xF97
    0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, // 0xF98 - 0xF9F
    0x01, 0x01, 0x01, 0x21, 0x29, 0x21, 0x00, 0x00, // 0xFA0 - 0xFA7
    0x01, 0x01, 0x01, 0x21, 0x29, 0x21, 0x21, 0x21, // 0xFA8 - 0xFAF
    0x23, 0x21, 0x00, 0x21, 0x00, 0x00, 0x23, 0x21, // 0xFB0 - 0xFB7
    0x21, 0x00, 0x29, 0x21, 0x21, 0x21, 0x21, 0x21, // 0xFB8 - 0xFBF
    0x21, 0x21, 0x00, 0x21, 0x00, 0x00, 0x00, 0x21, // 0xFC0 - 0xFC7
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, // 0xFC8 - 0xFCF
    0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, // 0xFD0 - 0xFD7
    0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, // 0xFD8 - 0xFDF
    0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, // 0xFE0 - 0xFE7
    0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, // 0xFE8 - 0xFEF
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 0xFF0 - 0xFF7
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // 0xFF8 - 0xFFF
  };

  static unsigned char group_table[56] = {
    0x61, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Group 1A
    0x27, 0x27, 0x23, 0x23, 0x23, 0x23, 0x23, 0x23, // Group 3 (Byte)
    0x25, 0x25, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21, // Group 3
    0x23, 0x23, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Group 4
    0x21, 0x21, 0x61, 0x21, 0x61, 0x21, 0x61, 0x00, // Group 5
    0x00, 0x00, 0x21, 0x21, 0x21, 0x00, 0x21, 0x23, // Group 7
    0x21, 0x00, 0x00, 0x21, 0x21, 0x00, 0x21, 0x00, // Group 7 (Alternate)
  };

  const unsigned char *insn_ptr = reinterpret_cast<const unsigned char *>(*ip);
  int operand_width = 4;
  int address_width = 4;
  if (is64bit) {
    address_width = 8;
  }
  unsigned char byte, rex = 0;
  bool found_prefix = false;
  if (rex_ptr) {
    *rex_ptr = 0;
  }
  if (mod_rm_ptr) {
    *mod_rm_ptr = 0;
  }
  if (sib_ptr) {
    *sib_ptr = 0;
  }
  for (;; ++insn_ptr) {
    switch (byte = *insn_ptr) {
      case 0x66: // Operand width prefix
        operand_width ^= 6;
        break;
      case 0x67: // Address width prefix
        address_width ^= is64bit ? 12 : 6;
        break;
      case 0x26: // Segment selector prefixes
      case 0x2e:
      case 0x36:
      case 0x3e:
      case 0x64:
      case 0x65:
      case 0xF0:
      case 0xF2:
      case 0xF3:
        break;
      case 0x40: case 0x41: case 0x42: case 0x43: // 64 bit REX prefixes
      case 0x44: case 0x45: case 0x46: case 0x47:
      case 0x48: case 0x49: case 0x4A: case 0x4B:
      case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        if (is64bit) {
          if (rex_ptr) {
            *rex_ptr = (char *)insn_ptr;
          }
          rex = byte;
          found_prefix = true;
          continue;
        }
        // fall through
      default:
        ++insn_ptr;
        goto no_more_prefixes;
    }
    rex = 0;
    found_prefix = true;
  }
no_more_prefixes:
  if (has_prefix) {
    *has_prefix = found_prefix;
  }
  if (rex & REX_W) {
    operand_width = 8;
  }
  unsigned char type;
  unsigned short insn = byte;
  unsigned int idx = 0;
  if (byte == 0x0F) {
    byte = *insn_ptr++;
    insn = (insn << 8) | byte;
    idx  = 256;
  }
  type = opcode_types[idx + byte];
  bool found_mod_rm = false;
  bool found_group = false;
  bool found_sib = false;
  unsigned char mod_rm = 0;
  unsigned char sib = 0;
  if (type & GROUP) {
    found_mod_rm = true;
    found_group = true;
    mod_rm = *insn_ptr;
    if (mod_rm_ptr) {
      *mod_rm_ptr = (char *)insn_ptr;
    }
    unsigned char group = (type & GROUP_MASK) + ((mod_rm >> 3) & 0x7);
    if ((type & GROUP_MASK) == 40 && (mod_rm >> 6) == 3) {
      group += 8;
    }
    type = group_table[group];
  }
  if (!type) {
    // We know that we still don't decode some of the more obscure
    // instructions, but for all practical purposes that doesn't matter.
    // Compilers are unlikely to output them, and even if we encounter
    // hand-coded assembly, we will soon synchronize to the instruction
    // stream again.
    //
    // std::cerr << "Unsupported instruction at 0x" << std::hex <<
    //     std::uppercase << reinterpret_cast<long>(*ip) << " [ ";
    // for (const unsigned char *ptr =
    //          reinterpret_cast<const unsigned char *>(*ip);
    //      ptr < insn_ptr; ) {
    //   std::cerr << std::hex << std::uppercase << std::setw(2) <<
    //       std::setfill('0') << (unsigned int)*ptr++ << ' ';
    // }
    // std::cerr << "]" << std::endl;
  } else {
    if (is64bit && (type & STACK)) {
      operand_width = 8;
    }
    if (type & MOD_RM) {
      found_mod_rm = true;
      if (mod_rm_ptr) {
        *mod_rm_ptr = (char *)insn_ptr;
      }
      mod_rm = *insn_ptr++;
      int mod = (mod_rm >> 6) & 0x3;
      int rm  = 8*(rex & REX_B) + (mod_rm & 0x7);
      if (mod != 3) {
        if (address_width == 2) {
          switch (mod) {
            case 0:
              if (rm != 6 /* SI */) {
                break;
              }
              // fall through
            case 2:
              insn_ptr++;
              // fall through
            case 1:
              insn_ptr++;
              break;
          }
        } else {
          if ((rm & 0x7) == 4) {
            found_sib = true;
            if (sib_ptr) {
              *sib_ptr = (char *)insn_ptr;
            }
            sib = *insn_ptr++;
            if (!mod && (sib & 0x7) == 5 /* BP */) {
              insn_ptr += 4;
            }
          }
          switch (mod) {
            case 0:
              if (rm != 5 /* BP */) {
                break;
              }
              // fall through
            case 2:
              insn_ptr += 3;
              // fall through
            case 1:
              insn_ptr++;
              break;
          }
        }
      }
    }
    switch (insn) {
      case 0xC8: // ENTER
        insn_ptr++;
        // fall through
      case 0x9A: // CALL (far)
      case 0xC2: // RET (near)
      case 0xCA: // LRET
      case 0xEA: // JMP (far)
        insn_ptr += 2;
        break;
      case 0xF80: case 0xF81: case 0xF82: case 0xF83: // Jcc (rel)
      case 0xF84: case 0xF85: case 0xF86: case 0xF87:
      case 0xF88: case 0xF89: case 0xF8A: case 0xF8B:
      case 0xF8C: case 0xF8D: case 0xF8E: case 0xF8F:
        insn_ptr += operand_width;
        break;
    }
    switch (type & MODE_MASK) {
      case IMM:
        if (!(type & BYTE_OP)) {
          switch (insn) {
            case 0xB8: case 0xB9: case 0xBA: case 0xBB:
            case 0xBC: case 0xBD: case 0xBE: case 0xBF:
              // Allow MOV to/from 64bit addresses
              insn_ptr += operand_width;
              break;
            default:
              insn_ptr += (operand_width == 8) ? 4 : operand_width;
              break;
          }
          break;
        }
        // fall through
      case IMM_BYTE:
        insn_ptr++;
        break;
      case MEM_ABS:
        insn_ptr += address_width;
        break;
    }
  }
  if (is_group) {
    *is_group = found_group;
  }
  *ip = reinterpret_cast<const char *>(insn_ptr);
  return insn;
}

} // namespace