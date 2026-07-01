#ifndef LINX_OPCODE_META_H
#define LINX_OPCODE_META_H

#include <stdint.h>

#include "linx_opcode_meta_gen.h"

const LinxOpcodeMeta *linx_opcode_meta_lookup(uint64_t insn_word, unsigned insn_len);

#endif /* LINX_OPCODE_META_H */
