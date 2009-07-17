#define XOPEN_SOURCE 500
#include <algorithm>
#include <elf.h>
#include <errno.h>
#include <errno.h>
#include <fcntl.h>
#include <iostream>
#include <linux/unistd.h>
#include <set>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>

//#include "valgrind/valgrind.h"
#define RUNNING_ON_VALGRIND 0 // TODO(markus): remove

#include "library.h"
#include "syscall.h"
#include "syscall_table.h"
#include "x86_decode.h"

#define NOINTR(x) ({ int i__; while ((i__ = (x)) < 0 && errno == EINTR); i__;})

#if __WORDSIZE == 64
typedef Elf64_Phdr    Elf_Phdr;
typedef Elf64_Rela    Elf_Rel;

typedef Elf64_Half    Elf_Half;
typedef Elf64_Word    Elf_Word;
typedef Elf64_Sword   Elf_Sword;
typedef Elf64_Xword   Elf_Xword;
typedef Elf64_Sxword  Elf_Sxword;
typedef Elf64_Off     Elf_Off;
typedef Elf64_Section Elf_Section;
typedef Elf64_Versym  Elf_Versym;

#define ELF_ST_BIND   ELF64_ST_BIND
#define ELF_ST_TYPE   ELF64_ST_TYPE
#define ELF_ST_INFO   ELF64_ST_INFO
#define ELF_R_SYM     ELF64_R_SYM
#define ELF_R_TYPE    ELF64_R_TYPE
#define ELF_R_INFO    ELF64_R_INFO

#define ELF_REL_PLT   ".rela.plt"
#define ELF_JUMP_SLOT R_X86_64_JUMP_SLOT
#elif __WORDSIZE == 32
typedef Elf32_Phdr    Elf_Phdr;
typedef Elf32_Rel     Elf_Rel;

typedef Elf32_Half    Elf_Half;
typedef Elf32_Word    Elf_Word;
typedef Elf32_Sword   Elf_Sword;
typedef Elf32_Xword   Elf_Xword;
typedef Elf32_Sxword  Elf_Sxword;
typedef Elf32_Off     Elf_Off;
typedef Elf32_Section Elf_Section;
typedef Elf32_Versym  Elf_Versym;

#define ELF_ST_BIND   ELF32_ST_BIND
#define ELF_ST_TYPE   ELF32_ST_TYPE
#define ELF_ST_INFO   ELF32_ST_INFO
#define ELF_R_SYM     ELF32_R_SYM
#define ELF_R_TYPE    ELF32_R_TYPE
#define ELF_R_INFO    ELF32_R_INFO

#define ELF_REL_PLT   ".rel.plt"
#define ELF_JUMP_SLOT R_386_JMP_SLOT
#else
#error "Undefined word size"
#endif

namespace playground {

class SysCalls {
 public:
  #define SYS_CPLUSPLUS
  #define SYS_ERRNO     my_errno
  #define SYS_INLINE    inline
  #define SYS_PREFIX    -1
  #undef  SYS_LINUX_SYSCALL_SUPPORT_H
  #include "linux_syscall_support.h"
  SysCalls() : my_errno(0) { }
  int my_errno;
};
#define ERRNO sys.my_errno

static void die(const char *msg) __attribute__((noreturn));
static void die(const char *msg) {
  SysCalls sys;
  sys.write(2, msg, strlen(msg));
  sys.write(2, "\n", 1);
  _exit(1);
}

char *Library::get(Elf_Addr offset, char *buf, size_t len) {
  if (!valid_) {
    memset(buf, 0, len);
    return NULL;
  }
  RangeMap::const_iterator iter = memory_ranges_.lower_bound(offset);
  if (iter == memory_ranges_.end()) {
    memset(buf, 0, len);
    return NULL;
  }
  offset -= iter->first;
  long size = reinterpret_cast<char *>(iter->second.stop) -
              reinterpret_cast<char *>(iter->second.start);
  if (offset > size - len) {
    if (!maps_ && memory_ranges_.size() == 1 &&
        !memory_ranges_.begin()->first) {
      // We are in the child and have exactly one mapping covering the whole
      // library. We are trying to read data past the end of what is currently
      // mapped. Check if we can expand the memory mapping to recover the
      // needed data
      SysCalls sys;
      long new_size = (offset + len + 4095) & ~4095;
      void *new_start = sys.mremap(iter->second.start, size, new_size,
                                   MREMAP_MAYMOVE);
      if (new_start != MAP_FAILED) {
        memory_ranges_.clear();
        memory_ranges_.insert(std::make_pair(0,
          Range(new_start, reinterpret_cast<void *>(
                    reinterpret_cast<char *>(new_start) + new_size),
                PROT_READ)));
        iter = memory_ranges_.begin();
        goto ok;
      }
    }
    memset(buf, 0, len);
    return NULL;
  }
ok:
  char *src = reinterpret_cast<char *>(iter->second.start) + offset;
  memcpy(buf, src, len);
  return buf;
}

std::string Library::get(Elf_Addr offset) {
  if (!valid_) {
    return "";
  }
  RangeMap::const_iterator iter = memory_ranges_.lower_bound(offset);
  if (iter == memory_ranges_.end()) {
    return "";
  }
  offset -= iter->first;
  size_t size = reinterpret_cast<char *>(iter->second.stop) -
                reinterpret_cast<char *>(iter->second.start);
  if (offset > size - 4096) {
    if (!maps_ && memory_ranges_.size() == 1 &&
        !memory_ranges_.begin()->first) {
      // We are in the child and have exactly one mapping covering the whole
      // library. We are trying to read data past the end of what is currently
      // mapped. Check if we can expand the memory mapping to recover the
      // needed data. We assume that strings are never long than 4kB.
      SysCalls sys;
      long new_size = (offset + 4096 + 4095) & ~4095;
      void *new_start = sys.mremap(iter->second.start, size, new_size,
                                   MREMAP_MAYMOVE);
      if (new_start != MAP_FAILED) {
        memory_ranges_.clear();
        memory_ranges_.insert(std::make_pair(0,
          Range(new_start, reinterpret_cast<void *>(
                    reinterpret_cast<char *>(new_start) + new_size),
                PROT_READ)));
        iter = memory_ranges_.begin();
        goto ok;
      }
    }
  }
ok:
  const char *start = reinterpret_cast<char *>(iter->second.start) + offset;
  const char *stop  = start;
  while (stop < reinterpret_cast<char *>(iter->second.stop) && *stop) {
    ++stop;
  }
  std::string s = stop > start ? std::string(start, stop - start) : "";
  return s;
}

char *Library::getOriginal(Elf_Addr offset, char *buf, size_t len) {
  if (!valid_) {
    memset(buf, 0, len);
    return NULL;
  }
  if (maps_) {
    return maps_->forwardGetRequest(this, offset, buf, len);
  }
  return get(offset, buf, len);
}

std::string Library::getOriginal(Elf_Addr offset) {
  if (!valid_) {
    return "";
  }
  if (maps_) {
    return maps_->forwardGetRequest(this, offset);
  }
  return get(offset);
}

const Elf_Ehdr* Library::getEhdr() {
  if (!valid_) {
    return NULL;
  }
  return &ehdr_;
}

const Elf_Shdr* Library::getSection(const std::string& section) {
  if (!valid_) {
    return NULL;
  }
  SectionTable::const_iterator iter = section_table_.find(section);
  if (iter == section_table_.end()) {
    return NULL;
  }
  return &iter->second.second;
}

const int Library::getSectionIndex(const std::string& section) {
  if (!valid_) {
    return -1;
  }
  SectionTable::const_iterator iter = section_table_.find(section);
  if (iter == section_table_.end()) {
    return -1;
  }
  return iter->second.first;
}

void **Library::getRelocation(const std::string& symbol) {
  PltTable::const_iterator iter = plt_entries_.find(symbol);
  if (iter == plt_entries_.end()) {
    return NULL;
  }
  return reinterpret_cast<void **>(asr_offset_ + iter->second);
}

void *Library::getSymbol(const std::string& symbol) {
  SymbolTable::const_iterator iter = symbols_.find(symbol);
  if (iter == symbols_.end() || !iter->second.st_value) {
    return NULL;
  }
  return asr_offset_ + iter->second.st_value;
}

void Library::makeWritable(bool state) const {
  for (RangeMap::const_iterator iter = memory_ranges_.begin();
       iter != memory_ranges_.end(); ++iter) {
    const Range& range = iter->second;
    long length = reinterpret_cast<char *>(range.stop) -
                  reinterpret_cast<char *>(range.start);
    SysCalls sys;
    sys.mprotect(range.start, length,
                 range.prot | (state ? PROT_WRITE : 0));
  }
}

bool Library::isSafeInsn(unsigned short insn) {
  // Check if the instruction has no unexpected side-effects. If so, it can
  // be safely relocated from the function that we are patching into the
  // out-of-line scratch space that we are setting up. This is often necessary
  // to make room for the JMP into the scratch space.
  return ((insn & 0x7) < 0x6 && (insn & 0xF0) < 0x40
          /* ADD, OR, ADC, SBB, AND, SUB, XOR, CMP */) ||
         #if __WORDSIZE == 64
         insn == 0x63 /* MOVSXD */ ||
         #endif
         (insn >= 0x80 && insn <= 0x8E /* ADD, OR, ADC,
         SBB, AND, SUB, XOR, CMP, TEST, XCHG, MOV, LEA */) ||
         (insn == 0x90) || /* NOP */
         (insn >= 0xA0 && insn <= 0xA9) /* MOV, TEST */ ||
         (insn >= 0xB0 && insn <= 0xBF /* MOV */) ||
         (insn >= 0xC0 && insn <= 0xC1) || /* Bit Shift */
         (insn >= 0xD0 && insn <= 0xD3) || /* Bit Shift */
         (insn >= 0xC6 && insn <= 0xC7 /* MOV */) ||
         (insn == 0xF7) /* TEST, NOT, NEG, MUL, IMUL, DIV, IDIV */;
}

char* Library::getScratchSpace(const Maps* maps, char* near, int needed,
                               char** extraSpace, int* extraLength) {
  if (needed > *extraLength ||
      labs(*extraSpace - reinterpret_cast<char *>(near)) > (1536 << 20)) {
    if (*extraSpace) {
      // Start a new scratch page and mark any previous page as write-protected
      SysCalls sys;
      sys.mprotect(*extraSpace, 4096, PROT_READ|PROT_EXEC);
    }
    // Our new scratch space is initially executable and writable.
    *extraLength = 4096;
    *extraSpace = maps->allocNearAddr(near, *extraLength,
                                      PROT_READ|PROT_WRITE|PROT_EXEC);
  }
  if (*extraSpace) {
    *extraLength -= needed;
    return *extraSpace + *extraLength;
  }
  die("Insufficient space to intercept system call");
}

void Library::patchSystemCallsInFunction(const Maps* maps, char *start,
                                         char *end, char** extraSpace,
                                         int* extraLength) {
  std::set<char *> branch_targets;
  for (char *ptr = start; ptr < end; ) {
    unsigned short insn = next_inst((const char **)&ptr, __WORDSIZE == 64);
    char *target;
    if ((insn >= 0x70 && insn <= 0x7F) /* Jcc */ || insn == 0xEB /* JMP */) {
      target = ptr + (reinterpret_cast<signed char *>(ptr))[-1];
    } else if (insn == 0xE8 /* CALL */ || insn == 0xE9 /* JMP */ ||
               (insn >= 0x0F80 && insn <= 0x0F8F) /* Jcc */) {
      target = ptr + (reinterpret_cast<int *>(ptr))[-1];
    } else {
      continue;
    }
    branch_targets.insert(target);
  }
  struct Code {
    char*          addr;
    int            len;
    unsigned short insn;
    bool           is_ip_relative;
  } code[5] = { { 0 } };
  int codeIdx = 0;
  char *ptr = start;
  while (ptr < end) {
    // Keep a ring-buffer of the last few instruction in order to find the
    // correct place to patch the code.
    char *mod_rm;
    code[codeIdx].addr = ptr;
    code[codeIdx].insn = next_inst((const char **)&ptr, __WORDSIZE == 64,
                                   0, 0, &mod_rm, 0, 0);
    code[codeIdx].len = ptr - code[codeIdx].addr;
    code[codeIdx].is_ip_relative = mod_rm && (*mod_rm & 0xC7) == 0x5;

    // Whenever we find a system call, we patch it with a jump to out-of-line
    // code that redirects to our system call wrapper.
    #if __WORDSIZE == 64
    bool is_indirect_call = false;
    if (code[codeIdx].insn == 0x0F05 /* SYSCALL */ ||
        // In addition, on x86-64, we need to redirect all CALLs between the
        // VDSO and the VSyscalls page. We want these to jump to our own
        // modified copy of the VSyscalls. As we know that the VSyscalls are
        // always more than 2GB away from the VDSO, the compiler has to
        // generate some form of indirect jumps. We can find all indirect
        // CALLs and redirect them to a separate scratch area, where we can
        // inspect the destination address. If it indeed points to the
        // VSyscall area, we then adjust the destination address accordingly.
        (is_indirect_call =
         (isVDSO_ && vsys_offset_ && code[codeIdx].insn == 0xFF &&
          !code[codeIdx].is_ip_relative &&
          mod_rm && (*mod_rm & 0x38) == 0x10 /* CALL (indirect) */))) {
    #else
    if (code[codeIdx].insn == 0xCD &&
        code[codeIdx].addr[1] == '\x80' /* INT $0x80 */) {
    #endif
      // Found a system call. Search backwards to figure out how to redirect
      // the code. We will need to overwrite a couple of instructions and,
      // of course, move these instructions somewhere else.
      int startIdx = codeIdx;
      int endIdx = codeIdx;
      int length = code[codeIdx].len;
      for (int idx = codeIdx;
           (idx = (idx + (sizeof(code) / sizeof(struct Code)) - 1) %
                  (sizeof(code) / sizeof(struct Code))) != codeIdx; ) {
        std::set<char *>::const_iterator iter =
            std::upper_bound(branch_targets.begin(), branch_targets.end(),
                             code[idx].addr);
        if (iter != branch_targets.end() && *iter < ptr) {
          // Found a branch pointing to somewhere past our instruction. This
          // instruction cannot be moved safely. Leave it in place.
          break;
        }
        if (code[idx].addr && !code[idx].is_ip_relative &&
            isSafeInsn(code[idx].insn)) {
          // These are all benign instructions with no side-effects and no
          // dependency on the program counter. We should be able to safely
          // relocate them.
          startIdx = idx;
          length   = ptr - code[startIdx].addr;
        } else {
          break;
        }
      }
      // Search forward past the system call, too. Sometimes, we can only
      // find relocatable instructions following the system call.
      #if __WORDSIZE == 32
   findEndIdx:
      #endif
      char *next = ptr;
      for (int i = codeIdx;
           (i = (i + 1) % (sizeof(code) / sizeof(struct Code))) != startIdx;
           ) {
        std::set<char *>::const_iterator iter =
            std::lower_bound(branch_targets.begin(), branch_targets.end(),
                             next);
        if (iter != branch_targets.end() && *iter == next) {
          // Found branch target pointing to our instruction
          break;
        }
        char *tmp_rm;
        code[i].addr = next;
        code[i].insn = next_inst((const char **)&next, __WORDSIZE == 64,
                                 0, 0, &tmp_rm, 0, 0);
        code[i].len = next - code[i].addr;
        code[i].is_ip_relative = tmp_rm && (*tmp_rm & 0xC7) == 0x5;
        if (!code[i].is_ip_relative && isSafeInsn(code[i].insn)) {
          endIdx = i;
          length = next - code[startIdx].addr;
        } else {
          break;
        }
      }
      // We now know, how many instructions neighboring the system call we
      // can safely overwrite. We need five bytes to insert a JMP/CALL and a
      // 32bit address. We then jump to a code fragment that safely forwards
      // to our system call wrapper. On x86-64, this is complicated by
      // the fact that the API allows up to 128 bytes of red-zones below the
      // current stack pointer. So, we cannot write to the stack until we
      // have adjusted the stack pointer.
      //
      // .. .. .. .. ; any leading instructions copied from original code
      // 48 81 EC 80 00 00 00        SUB  $0x80, %rsp
      // 50                          PUSH %rax
      // 48 8D 05 .. .. .. ..        LEA  ...(%rip), %rax
      // 50                          PUSH %rax
      // 48 B8 .. .. .. ..           MOV  $syscallWrapper, %rax
      // .. .. .. ..
      // 50                          PUSH %rax
      // 48 8D 05 06 00 00 00        LEA  6(%rip), %rax
      // 48 87 44 24 10              XCHG %rax, 16(%rsp)
      // C3                          RETQ
      // 48 81 C4 80 00 00 00        ADD  $0x80, %rsp
      // .. .. .. .. ; any trailing instructions copied from original code
      // E9 .. .. .. ..              JMPQ ...
      //
      // Total: 52 bytes + any bytes that were copied
      //
      // On x86-32, the stack is available and we can do:
      //
      // TODO(markus): Try to maintain frame pointers on x86-32
      //
      // .. .. .. .. ; any leading instructions copied from original code
      // 68 .. .. .. ..              PUSH return_addr
      // 68 .. .. .. ..              PUSH $syscallWrapper
      // C3                          RET
      // .. .. .. .. ; any trailing instructions copied from original code
      // C3                          RET
      //
      // Total: 12 bytes + any bytes that were copied
      //
      // For indirect jumps from the VDSO to the VSyscall page, we instead
      // replace the following code (this is only necessary on x86-64). This
      // time, we don't have to worry about red zones:
      //
      // .. .. .. .. ; any leading instructions copied from original code
      // E8 00 00 00 00              CALL .
      // 48 83 04 24 ..              ADDQ $.., (%rsp)
      // FF .. .. ..                 PUSH ..  ; from original CALL instruction
      // 48 81 3C 24 00 00 00 FF     CMPQ $0xFFFFFFFFFF000000, 0(%rsp)
      // 72 0F                       JB   . + 15
      // 81 2C 24 .. .. .. ..        SUBL ..., 0(%rsp)
      // C7 44 24 04 00 00 00 00     MOVL $0, 4(%rsp)
      // C3                          RETQ
      // .. .. .. .. ; any trailing instructions copied from original code
      // E9 .. .. .. ..              JMPQ ...
      //
      // Total: 41 bytes + any bytes that were copied

      if (length < 5) {
        // There are a very small number of instruction sequences that we
        // cannot easily intercept, and that have been observed in real world
        // examples. Handle them here:
        #if __WORDSIZE == 32
        int diff;
        if (!memcmp(code[codeIdx].addr, "\xCD\x80\xEB", 3) &&
            (diff = *reinterpret_cast<signed char *>(
                 code[codeIdx].addr + 3)) < 0 && diff >= -6) {
          // We have seen...
          //   for (;;) {
          //      _exit(0);
          //   }
          // ..get compiled to:
          //   B8 01 00 00 00      MOV  $__NR_exit, %eax
          //   66 90               XCHG %ax, %ax
          //   31 DB             0:XOR  %ebx, %ebx
          //   CD 80               INT  $0x80
          //   EB FA               JMP  0b
          // The JMP is really superfluous as the system call never returns.
          // And there are in fact no returning system calls that need to be
          // unconditionally repeated in an infinite loop.
          // If we replace the JMP with NOPs, the system call can successfully
          // be intercepted.
          *reinterpret_cast<unsigned short *>(code[codeIdx].addr + 2) = 0x9090;
          goto findEndIdx;
        }
        #endif
        die("Cannot intercept system call");
      }
      int needed = 5 - code[codeIdx].len;
      int first = codeIdx;
      while (needed > 0 && first != startIdx) {
        first = (first + (sizeof(code) / sizeof(struct Code)) - 1) %
                (sizeof(code) / sizeof(struct Code));
        needed -= code[first].len;
      }
      int second = codeIdx;
      while (needed > 0) {
        second = (second + 1) % (sizeof(code) / sizeof(struct Code));
        needed -= code[second].len;
      }
      int preamble = code[codeIdx].addr - code[first].addr;
      int postamble = code[second].addr + code[second].len -
                      code[codeIdx].addr - code[codeIdx].len;

      // The following is all the code that construct the various bits of
      // assembly code.
      #if __WORDSIZE == 64
      if (is_indirect_call) {
        needed = 41 + preamble + code[codeIdx].len + postamble;
      } else {
        needed = 52 + preamble + postamble;
      }
      #else
      needed = 12 + preamble + postamble;
      #endif

      // Allocate scratch space and copy the preamble of code that was moved
      // from the function that we are patching.
      char* dest = getScratchSpace(maps, code[first].addr, needed,
                                   extraSpace, extraLength);
      memcpy(dest, code[first].addr, preamble);

      // For indirect calls, we need to copy the actual CALL instruction and
      // turn it into a PUSH instruction.
      #if __WORDSIZE == 64
      if (is_indirect_call) {
        memcpy(dest + preamble, "\xE8\x00\x00\x00\x00\x48\x83\x04\x24", 9);
        dest[preamble + 9] = code[codeIdx].len + 31;
        memcpy(dest + preamble + 10, code[codeIdx].addr, code[codeIdx].len);

        // Convert CALL -> PUSH
        dest[preamble + 10 + (mod_rm - code[codeIdx].addr)] |= 0x20;
        preamble += 10 + code[codeIdx].len;
      }
      #endif

      // Copy the static body of the assembly code.
      memcpy(dest + preamble,
           #if __WORDSIZE == 64
           is_indirect_call ?
           "\x48\x81\x3C\x24\x00\x00\x00\xFF\x72\x0F\x81\x2C\x24\x00\x00\x00"
           "\x00\xC7\x44\x24\x04\x00\x00\x00\x00\xC3" :
           "\x48\x81\xEC\x80\x00\x00\x00\x50\x48\x8D\x05\x00\x00\x00\x00\x50"
           "\x48\xB8\x00\x00\x00\x00\x00\x00\x00\x00\x50\x48\x8D\x05\x06\x00"
           "\x00\x00\x48\x87\x44\x24\x10\xC3\x48\x81\xC4\x80\x00\x00",
           is_indirect_call ? 26 : 47
           #else
           "\x68\x00\x00\x00\x00\x68\x00\x00\x00\x00\xC3", 11
           #endif
           );

      // Copy the postamble that was moved from the function that we are
      // patching.
      memcpy(dest + preamble +
             #if __WORDSIZE == 64
             (is_indirect_call ? 26 : 47),
             #else
             11,
             #endif
             code[codeIdx].addr + code[codeIdx].len,
             postamble);

      // Patch up the various computed values
      #if __WORDSIZE == 64
      int post = preamble + (is_indirect_call ? 26 : 47) + postamble;
      dest[post] = '\xE9';
      *reinterpret_cast<int *>(dest + post + 1) =
          (code[second].addr + code[second].len) - (dest + post + 5);
      if (is_indirect_call) {
        *reinterpret_cast<int *>(dest + preamble + 13) = vsys_offset_;
      } else {
        *reinterpret_cast<int *>(dest + preamble + 11) =
            (code[second].addr + code[second].len) - (dest + preamble + 15);
        *reinterpret_cast<void **>(dest + preamble + 18) =
            reinterpret_cast<void *>(&syscallWrapper);
      }
      #else
      *(dest + preamble + 11 + postamble) = '\xC3';
      *reinterpret_cast<char **>(dest + preamble + 1) =
          dest + preamble + 11;
      *reinterpret_cast<void (**)()>(dest + preamble + 6) = syscallWrapper;
      #endif

      // Pad unused space in the original function with NOPs
      memset(code[first].addr, 0x90 /* NOP */,
             code[second].addr + code[second].len - code[first].addr);

      // Replace the system call with an unconditional jump to our new code.
      *code[first].addr = __WORDSIZE == 64 ? '\xE9' : // JMPQ
                                             '\xE8';  // CALL
      *reinterpret_cast<int *>(code[first].addr + 1) =
          dest - (code[first].addr + 5);
    }
    codeIdx = (codeIdx + 1) % (sizeof(code) / sizeof(struct Code));
  }
}

void Library::patchVDSO(char** extraSpace, int* extraLength){
  #if __WORDSIZE == 32
  // x86-32 has a small number of well-defined functions in the VDSO library.
  // These functions do not easily lend themselves to be rewritten by the
  // automatic code. Instead, we explicitly find new definitions for them.
  //
  // We don't bother with optimizing the syscall instruction instead always
  // use INT $0x80, no matter whether the hardware supports more modern
  // calling conventions.
  //
  // TODO(markus): Investigate whether it is worthwhile to optimize this
  // code path and use the platform-specific entry code.
  SymbolTable::const_iterator iter = symbols_.find("__kernel_vsyscall");
  if (iter != symbols_.end() && iter->second.st_value) {
    char* start = asr_offset_ + iter->second.st_value;
    // Replace the kernel entry point with:
    //
    // E9 .. .. .. ..    JMP syscallWrapper
    *start = '\xE9';
    *reinterpret_cast<long *>(start + 1) =
        reinterpret_cast<char *>(&syscallWrapper) -
        reinterpret_cast<char *>(start + 5);
  }
  iter = symbols_.find("__kernel_sigreturn");
  if (iter != symbols_.end() && iter->second.st_value) {
    // Replace the sigreturn() system call with a jump to code that does:
    //
    // 58                POP %eax
    // B8 77 00 00 00    MOV $0x77, %eax
    // E9 .. .. .. ..    JMP syscallWrapper
    char* start = asr_offset_ + iter->second.st_value;
    char* dest = getScratchSpace(maps_, start, 11, extraSpace, extraLength);
    memcpy(dest, "\x58\xB8\x77\x00\x00\x00\xE9", 7);
    *reinterpret_cast<char *>(dest + 7) =
        reinterpret_cast<char *>(&syscallWrapper) -
        reinterpret_cast<char *>(dest + 11);
    *start = '\xE9';
    *reinterpret_cast<char *>(start + 1) =
        dest - reinterpret_cast<char *>(start + 5);
  }
  iter = symbols_.find("__kernel_rt_sigreturn");
  if (iter != symbols_.end() && iter->second.st_value) {
    // Replace the rt_sigreturn() system call with a jump to code that does:
    //
    // B8 AD 00 00 00    MOV $0xAD, %eax
    // E9 .. .. .. ..    JMP syscallWrapper
    char* start = asr_offset_ + iter->second.st_value;
    char* dest = getScratchSpace(maps_, start, 10, extraSpace, extraLength);
    memcpy(dest, "\xB8\xAD\x00\x00\x00\xE9", 6);
    *reinterpret_cast<char *>(dest + 6) =
        reinterpret_cast<char *>(&syscallWrapper) -
        reinterpret_cast<char *>(dest + 10);
    *start = '\xE9';
    *reinterpret_cast<char *>(start + 1) =
        dest - reinterpret_cast<char *>(start + 5);
  }
  #endif
}

int Library::patchVSystemCalls() {
  #if __WORDSIZE == 64
  // VSyscalls live in a shared 4kB page at the top of the address space. This
  // page cannot be unmapped nor remapped. We have to create a copy within
  // 2GB of the page, and rewrite all IP-relative accesses to shared variables.
  // As the top of the address space is not accessible by mmap(), this means
  // that we need to wrap around addresses to the bottom 2GB of the address
  // space.
  // Only x86-64 has VSyscalls.
  if (maps_->vsyscall()) {
    char* copy = maps_->allocNearAddr(maps_->vsyscall(), 0x1000,
                                    PROT_READ|PROT_WRITE);
    char* extraSpace = copy;
    int extraLength = 0x1000;
    memcpy(copy, maps_->vsyscall(), 0x1000);
    long adjust = (long)maps_->vsyscall() - (long)copy;
    for (int vsys = 0; vsys < 0x1000; vsys += 0x400) {
      char* start = copy + vsys;
      char* end   = start + 0x400;

      // There can only be up to four VSyscalls starting at an offset of
      // n*0x1000, each. VSyscalls are invoked by functions in the VDSO
      // and provide fast implementations of a time source. We don't exactly
      // know where the code and where the data is in the VSyscalls page.
      // So, we disassemble the code for each function and find all branch
      // targets within the function in order to find the last address of
      // function.
      for (char *last = start, *vars = end, *ptr = start; ptr < end; ) {
     new_function:
        char* mod_rm;
        unsigned short insn = next_inst((const char **)&ptr, true, 0, 0,
                                        &mod_rm, 0, 0);
        if (mod_rm && (*mod_rm & 0xC7) == 0x5) {
          // Instruction has IP relative addressing mode. Adjust to reference
          // the variables in the original VSyscall segment.
          long offset = *reinterpret_cast<int *>(mod_rm + 1);
          char* var = ptr + offset;
          if (var >= ptr && var < vars) {
            // Variables are stored somewhere past all the functions. Remember
            // the first variable in the VSyscall slot, so that we stop
            // scanning for instructions once we reach that address.
            vars = var;
          }
          offset += adjust;
          if ((offset >> 32) && (offset >> 32) != -1) {
            die("Cannot patch [vsystemcall]");
          }
          *reinterpret_cast<int *>(mod_rm + 1) = offset;
        }

        // Check for jump targets to higher addresses (but within our own
        // VSyscall slot). They extend the possible end-address of this
        // function.
        char *target = 0;
        if ((insn >= 0x70 && insn <= 0x7F) /* Jcc */ ||
            insn == 0xEB /* JMP */) {
          target = ptr + (reinterpret_cast<signed char *>(ptr))[-1];
        } else if (insn == 0xE8 /* CALL */ || insn == 0xE9 /* JMP */ ||
                   (insn >= 0x0F80 && insn <= 0x0F8F) /* Jcc */) {
          target = ptr + (reinterpret_cast<int *>(ptr))[-1];
        }

        // The function end is found, once the loop reaches the last valid
        // address in the VSyscall slot, or once it finds a RET instruction
        // that is not followed by any jump targets. Unconditional jumps that
        // point backwards are treated the same as a RET instruction.
        if (insn == 0xC3 /* RET */ ||
            (target < ptr &&
             (insn == 0xEB /* JMP */ || insn == 0xE9 /* JMP */))) {
          if (last >= ptr) {
            continue;
          } else {
            // The function can optionally be followed by more functions in
            // the same VSyscall slot. Allow for alignment to a 16 byte
            // boundary. If we then find more non-zero bytes, and if this is
            // not the known start of the variables, assume a new function
            // started.
            for (; ptr < vars; ++ptr) {
              if ((long)ptr & 0xF) {
                if (*ptr && *ptr != '\x90' /* NOP */) {
                  goto new_function;
                }
                *ptr = '\x90'; // NOP
              } else {
                if (*ptr && *ptr != '\x90' /* NOP */) {
                  goto new_function;
                }
                break;
              }
            }

            // Translate all SYSCALLs to jumps into our system call handler.
            patchSystemCallsInFunction(NULL, start, ptr,
                                       &extraSpace, &extraLength);
            break;
          }
        }

        // Adjust assumed end address for this function, if a valid jump
        // target has been found that originates from the current instruction.
        if (target > last && target < start + 0x100) {
          last = target;
        }
      }
    }

    // We are done. Write-protect our code and make it executable.
    SysCalls sys;
    sys.mprotect(copy, 0x1000, PROT_READ|PROT_EXEC);
    return maps_->vsyscall() - copy;
  }
  #endif
  return 0;
}

void Library::patchSystemCalls() {
  if (!valid_) {
    return;
  }
  int extraLength = 0;
  char* extraSpace = NULL;
  if (isVDSO_) {
    // patchVDSO() calls patchSystemCallsInFunction() which needs vsys_offset_
    // iff processing the VDSO library. So, make sure we call
    // patchVSystemCalls() first.
    vsys_offset_ = patchVSystemCalls();
    patchVDSO(&extraSpace, &extraLength);
  }
  SectionTable::const_iterator iter;
  if ((iter = section_table_.find(".text")) == section_table_.end()) {
    return;
  }
  const Elf_Shdr& shdr = iter->second.second;
  char* start = reinterpret_cast<char *>(shdr.sh_addr + asr_offset_);
  char* stop = start + shdr.sh_size;
  char* func = start;
  int nopcount = 0;
  bool has_syscall = false;
  for (char *ptr = start; ptr < stop; ptr++) {
    #if __WORDSIZE == 64
    if (*ptr == '\x0F' && *++ptr == '\x05' /* SYSCALL */) {
    #else
    if (*ptr == '\xCD' && *++ptr == '\x80' /* INT $0x80 */) {
    #endif
      has_syscall = true;
    } else if (*ptr == '\x90' /* NOP */) {
      nopcount++;
    } else if (!(reinterpret_cast<long>(ptr) & 0xF)) {
      if (nopcount > 2) {
        // This is very likely the beginning of a new function. Functions
        // are aligned on 16 byte boundaries and the preceding function is
        // padded out with NOPs.
        //
        // For performance reasons, we quickly scan the entire text segment
        // for potential SYSCALLs, and then patch the code in increments of
        // individual functions.
        if (has_syscall) {
          has_syscall = false;
          // Our quick scan of the function found a potential system call.
          // Do a more thorough scan, now.
          patchSystemCallsInFunction(maps_, func, ptr, &extraSpace,
                                     &extraLength);
        }
        func = ptr;
      }
      nopcount = 0;
    }
  }
  if (has_syscall) {
    // Patch any remaining system calls that were in the last function before
    // the loop terminated.
    patchSystemCallsInFunction(maps_, func, stop, &extraSpace, &extraLength);
  }

  // Mark our scratch space as write-protected and executable.
  if (extraSpace) {
    SysCalls sys;
    sys.mprotect(extraSpace, 4096, PROT_READ|PROT_EXEC);
  }
}

bool Library::parseElf() {
  valid_ = true;

  // Verify ELF header
  Elf_Shdr str_shdr;
  if (!getOriginal(0, &ehdr_) ||
      ehdr_.e_ehsize < sizeof(Elf_Ehdr) ||
      ehdr_.e_phentsize < sizeof(Elf_Phdr) ||
      ehdr_.e_shentsize < sizeof(Elf_Shdr) ||
      !getOriginal(ehdr_.e_shoff + ehdr_.e_shstrndx * ehdr_.e_shentsize,
                   &str_shdr)) {
    // Not all memory mappings are necessarily ELF files. Skip memory
    // mappings that we cannot identify.
    valid_ = false;
    return false;
  }

  // Find PT_DYNAMIC segment. This is what our PLT entries and symbols will
  // point to. This information is probably incorrect in the child, as it
  // requires access to the original memory mappings.
  for (int i = 0; i < ehdr_.e_phnum; i++) {
    Elf_Phdr phdr;
    if (getOriginal(ehdr_.e_phoff + i*ehdr_.e_phentsize, &phdr) &&
        phdr.p_type == PT_DYNAMIC) {
      RangeMap::const_iterator iter =
          memory_ranges_.lower_bound(phdr.p_offset);
      if (iter != memory_ranges_.end()) {
        asr_offset_ = reinterpret_cast<char *>(iter->second.start) -
            (phdr.p_vaddr - (phdr.p_offset - iter->first));
      }
      break;
    }
  }

  // Parse section table and find all sections in this ELF file
  for (int i = 0; i < ehdr_.e_shnum; i++) {
    Elf_Shdr shdr;
    if (!getOriginal(ehdr_.e_shoff + i*ehdr_.e_shentsize, &shdr)) {
      continue;
    }
    section_table_.insert(
       std::make_pair(getOriginal(str_shdr.sh_offset + shdr.sh_name),
                      std::make_pair(i, shdr)));
  }

  // Find PLT and symbol tables
  const Elf_Shdr* plt = getSection(ELF_REL_PLT);
  const Elf_Shdr* symtab = getSection(".dynsym");
  Elf_Shdr strtab = { 0 };
  if (symtab) {
    if (symtab->sh_link >= ehdr_.e_shnum ||
        !getOriginal(ehdr_.e_shoff + symtab->sh_link * ehdr_.e_shentsize,
                     &strtab)) {
      std::cout << "Cannot find valid symbol table" << std::endl;
      valid_ = false;
      return false;
    }
  }

  if (plt && symtab) {
    // Parse PLT table and add its entries
    for (int i = plt->sh_size/sizeof(Elf_Rel); --i >= 0; ) {
      Elf_Rel rel;
      if (!getOriginal(plt->sh_offset + i * sizeof(Elf_Rel), &rel) ||
          ELF_R_SYM(rel.r_info)*sizeof(Elf_Sym) >= symtab->sh_size) {
        std::cout << "Encountered invalid plt entry" << std::endl;
        valid_ = false;
        return false;
      }

      if (ELF_R_TYPE(rel.r_info) != ELF_JUMP_SLOT) {
        continue;
      }
      Elf_Sym sym;
      if (!getOriginal(symtab->sh_offset +
                       ELF_R_SYM(rel.r_info)*sizeof(Elf_Sym), &sym) ||
          sym.st_shndx >= ehdr_.e_shnum) {
        std::cout << "Encountered invalid symbol for plt entry" << std::endl;
        valid_ = false;
        return false;
      }
      std::string name = getOriginal(strtab.sh_offset + sym.st_name);
      if (name.empty()) {
        continue;
      }
      plt_entries_.insert(std::make_pair(name, rel.r_offset));
    }
  }

  if (symtab) {
    // Parse symbol table and add its entries
    for (Elf_Addr addr = 0; addr < symtab->sh_size; addr += sizeof(Elf_Sym)) {
      Elf_Sym sym;
      if (!getOriginal(symtab->sh_offset + addr, &sym) ||
          (sym.st_shndx >= ehdr_.e_shnum &&
           sym.st_shndx < SHN_LORESERVE)) {
        std::cout << "Encountered invalid symbol" << std::endl;
        valid_ = false;
        return false;
      }
      std::string name = getOriginal(strtab.sh_offset + sym.st_name);
      if (name.empty()) {
        continue;
      }
      symbols_.insert(std::make_pair(name, sym));
    }
  }

  return true;
}

void Library::recoverOriginalDataParent(Maps* maps) {
  maps_ = maps;
}

void Library::recoverOriginalDataChild(const std::string& filename) {
  if (RUNNING_ON_VALGRIND) {
    // TODO(markus): Dereferencing the file name from /proc/self/maps is
    // unreliable. For now, leave this code enabled, as valgrind doesn't
    // understand the correct solution involving m(re)map(). We should
    // eventually remove valgrind support, as it just doesn't work for
    // the sandbox.
    memory_ranges_.clear();
    int fd = open(filename.c_str(), O_RDONLY);
    if (fd >= 0) {
      struct stat sb;
      fstat(fd, &sb);
      size_t len = (sb.st_size + 4095) & ~4095;
      void *start = mmap(0, len, PROT_READ, MAP_SHARED, fd, 0);
      NOINTR(close(fd));
      memory_ranges_.insert(
          std::make_pair(0, Range(start, reinterpret_cast<char *>(start) + len,
                                  PROT_READ)));
    }
    valid_ = true;
  } else {
    if (memory_ranges_.empty() || memory_ranges_.rbegin()->first) {
   failed:
      memory_ranges_.clear();
    } else {
      const Range& range = memory_ranges_.rbegin()->second;
      struct Args {
        void* old_addr;
        long  old_length;
        void* new_addr;
        long  new_length;
        long  prot;
      } args = {
        range.start,
        (reinterpret_cast<long>(range.stop) -
         reinterpret_cast<long>(range.start) + 4095) & ~4095,
        0,
        (memory_ranges_.begin()->first +
         (reinterpret_cast<long>(memory_ranges_.begin()->second.stop) -
          reinterpret_cast<long>(memory_ranges_.begin()->second.start)) +
         4095) & ~4095,
        range.prot
      };
      // We find the memory mapping that starts at file offset zero and
      // extend it to cover the entire file. This is a little difficult to
      // do, as the mapping needs to be moved to a different address. But
      // we potentially running code that is inside of this mapping at the
      // time when it gets moved.
      //
      // We have to write the code in assembly. We allocate temporary
      // storage and copy the critical code into this page. We then execute
      // from this page, while we relocate the mapping. Finally, we allocate
      // memory at the original location and copy the original data into it.
      // The program can now resume execution.
      #if __WORDSIZE == 64
      asm volatile(
          // new_addr = 4096 + mmap(0, new_length + 4096,
          //                        PROT_READ|PROT_WRITE|PROT_EXEC,
          //                        MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
          "mov  $0, %%r9\n"
          "mov  $-1, %%r8\n"
          "mov  $0x22, %%r10\n"
          "mov  $7, %%rdx\n"
          "mov  0x18(%0), %%rsi\n"
          "add  $4096, %%rsi\n"
          "mov  $0, %%rdi\n"
          "mov  $9, %%rax\n"
          "syscall\n"
          "cmp  $-4096, %%rax\n"
          "ja   6f\n"
          "mov  %%rax, %%r12\n"
          "add  $4096, %%r12\n"

          // memcpy(new_addr - 4096, &&asm, asm_length)
          "lea  2f(%%rip), %%rsi\n"
          "lea  6f(%%rip), %%rdi\n"
          "sub  %%rsi, %%rdi\n"
        "0:sub  $1, %%rdi\n"
          "test %%rdi, %%rdi\n"
          "js   1f\n"
          "movzbl (%%rsi, %%rdi, 1), %%ebx\n"
          "mov  %%bl, (%%rax, %%rdi, 1)\n"
          "jmp  0b\n"
       "1:\n"

          // ((void (*)())new_addr - 4096)()
          "lea  6f(%%rip), %%rbx\n"
          "push %%rbx\n"
          "jmp  *%%rax\n"

          // mremap(old_addr, old_length, new_length,
          //        MREMAP_MAYMOVE|MREMAP_FIXED, new_addr)
        "2:mov  %%r12, %%r8\n"
          "mov  $3, %%r10\n"
          "mov  0x18(%0), %%rdx\n"
          "mov  0x8(%0), %%rsi\n"
          "mov  0(%0), %%rdi\n"
          "mov  $25, %%rax\n"
          "syscall\n"
          "cmp  $-4096, %%rax\n"
          "ja   5f\n"

          // mmap(old_addr, old_length, PROT_WRITE,
          //      MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0)
          "mov  $0, %%r9\n"
          "mov  $-1, %%r8\n"
          "mov  $0x32, %%r10\n"
          "mov  $2, %%rdx\n"
          "mov  0x8(%0), %%rsi\n"
          "mov  0(%0), %%rdi\n"
          "mov  $9, %%rax\n"
          "syscall\n"
          "cmp  $-12, %%eax\n"
          "jz   4f\n"
          "cmp  $-4096, %%rax\n"
          "ja   5f\n"

          // memcpy(old_addr, new_addr, old_length)
          "mov  0x8(%0), %%rdi\n"
        "3:sub  $1, %%rdi\n"
          "test %%rdi, %%rdi\n"
          "js   4f\n"
          "movzbl (%%r12, %%rdi, 1), %%ebx\n"
          "mov  %%bl, (%%rax, %%rdi, 1)\n"
          "jmp  3b\n"
        "4:\n"

          // mprotect(old_addr, old_length, prot)
          "mov  0x20(%0), %%rdx\n"
          "mov  0x8(%0), %%rsi\n"
          "mov  %%rax, %%rdi\n"
          "mov  $10, %%rax\n"
          "syscall\n"

          // args.new_addr = new_addr
          "mov  %%r12, 0x10(%0)\n"
        "5:retq\n"

          // munmap(new_addr - 4096, 4096)
        "6:mov  $4096, %%rsi\n"
          "mov  %%r12, %%rdi\n"
          "sub  %%rsi, %%rdi\n"
          "mov  $11, %%rax\n"
          "syscall\n"
          :
          : "q"(&args)
          : "rax", "rbx", "rcx", "rdx", "rsi", "rdi",
            "r8", "r9", "r10", "r11", "r12", "memory");
      #else
      asm volatile(
          "push %%ebp\n"
          "push %%ebx\n"
          "push %%edi\n"

          // new_addr = 4096 + mmap(0, new_length + 4096,
          //                        PROT_READ|PROT_WRITE|PROT_EXEC,
          //                        MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
          "mov  $0, %%ebp\n"
          "mov  $0x22, %%esi\n"
          "mov  $7, %%edx\n"
          "mov  12(%%edi), %%ecx\n"
          "add  $4096, %%ecx\n"
          "mov  $-1, %%edi\n"
          "mov  $0, %%ebx\n"
          "mov  $192, %%eax\n"
          "int  $0x80\n"
          "cmp  $-4096, %%eax\n"
          "ja   6f\n"
          "mov  %%eax, %%ebp\n"
          "add  $4096, %%ebp\n"

          // memcpy(new_addr - 4096, &&asm, asm_length)
          "lea  2f, %%ecx\n"
          "lea  6f, %%ebx\n"
          "sub  %%ecx, %%ebx\n"
        "0:dec  %%ebx\n"
          "test %%ebx, %%ebx\n"
          "js   1f\n"
          "movzbl (%%ecx, %%ebx, 1), %%edx\n"
          "mov  %%dl, (%%eax, %%ebx, 1)\n"
          "jmp  0b\n"
        "1:\n"

          // ((void (*)())new_addr - 4096)()
          "lea  6f, %%ebx\n"
          "push %%ebx\n"
          "jmp  *%%eax\n"

          // mremap(old_addr, old_length, new_length,
          //        MREMAP_MAYMOVE|MREMAP_FIXED, new_addr)
        "2:push %%ebp\n"
          "mov  $3, %%esi\n"
          "mov  8(%%esp), %%edi\n"
          "mov  12(%%edi), %%edx\n"
          "mov  4(%%edi), %%ecx\n"
          "mov  0(%%edi), %%ebx\n"
          "mov  %%ebp, %%edi\n"
          "mov  $163, %%eax\n"
          "int  $0x80\n"
          "cmp  $-4096, %%eax\n"
          "ja   5f\n"

          // mmap(old_addr, old_length, PROT_WRITE,
          //      MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0)
          "mov  $0, %%ebp\n"
          "mov  $0x32, %%esi\n"
          "mov  $2, %%edx\n"
          "mov  8(%%esp), %%edi\n"
          "mov  4(%%edi), %%ecx\n"
          "mov  0(%%edi), %%ebx\n"
          "mov  $-1, %%edi\n"
          "mov  $192, %%eax\n"
          "int  $0x80\n"
          "cmp  $-12, %%eax\n"
          "jz   4f\n"
          "cmp  $-4096, %%eax\n"
          "ja   5f\n"

          // memcpy(old_addr, new_addr, old_length)
          "mov  0(%%esp), %%ecx\n"
          "mov  8(%%esp), %%edi\n"
          "mov  4(%%edi), %%ebx\n"
        "3:dec  %%ebx\n"
          "test %%ebx, %%ebx\n"
          "js   4f\n"
          "movzbl (%%ecx, %%ebx, 1), %%edx\n"
          "mov  %%dl, (%%eax, %%ebx, 1)\n"
          "jmp  3b\n"
        "4:\n"

          // mprotect(old_addr, old_length, prot)
          "mov  8(%%esp), %%edi\n"
          "mov  16(%%edi), %%edx\n"
          "mov  4(%%edi), %%ecx\n"
          "mov  %%eax, %%ebx\n"
          "mov  $125, %%eax\n"
          "int  $0x80\n"

          // args.new_addr = new_addr
          "mov  8(%%esp), %%edi\n"
          "mov  0(%%esp), %%ebp\n"
          "mov  %%ebp, 0x8(%%edi)\n"

        "5:pop  %%ebx\n"
          "ret\n"

          // munmap(new_addr - 4096, 4096)
        "6:mov  $4096, %%ecx\n"
          "sub  %%ecx, %%ebx\n"
          "mov  $91, %%eax\n"
          "int  $0x80\n"
          "pop  %%edi\n"
          "pop  %%ebx\n"
          "pop  %%ebp\n"
          :
          : "D"(&args)
          : "eax", "ecx", "edx", "esi", "memory");
      #endif
      if (!args.new_addr) {
        goto failed;
      }

      memory_ranges_.clear();
      memory_ranges_.insert(std::make_pair(0, Range(args.new_addr,
                   reinterpret_cast<char *>(args.new_addr) + args.new_length,
                   PROT_READ)));
      valid_ = true;
    }
  }
}

} // namespace