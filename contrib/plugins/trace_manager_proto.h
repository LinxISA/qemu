#ifndef STANDALONE_OEX_TRACE_MANAGER_PROTO_H_
#define STANDALONE_OEX_TRACE_MANAGER_PROTO_H_

#include <stddef.h>
#include <stdint.h>

typedef struct oex_ctx oex_ctx_t;

typedef struct {
  uint64_t seq;
  uint64_t pc;
  uint64_t raw;
  uint8_t len;
} oex_inst_event_t;

typedef enum {
  OEX_TRACE_STAGE_F0 = 0,
  OEX_TRACE_STAGE_F1 = 1,
  OEX_TRACE_STAGE_F2 = 2,
  OEX_TRACE_STAGE_F3 = 3,
  OEX_TRACE_STAGE_F4 = 4,
  OEX_TRACE_STAGE_D1 = 5,
  OEX_TRACE_STAGE_D2 = 6,
  OEX_TRACE_STAGE_D3 = 7,
  OEX_TRACE_STAGE_IQ = 8,
  OEX_TRACE_STAGE_S1 = 9,
  OEX_TRACE_STAGE_S2 = 10,
  OEX_TRACE_STAGE_P1 = 11,
  OEX_TRACE_STAGE_I1 = 12,
  OEX_TRACE_STAGE_I2 = 13,
  OEX_TRACE_STAGE_E1 = 14,
  OEX_TRACE_STAGE_E2 = 15,
  OEX_TRACE_STAGE_E3 = 16,
  OEX_TRACE_STAGE_E4 = 17,
  OEX_TRACE_STAGE_W1 = 18,
  OEX_TRACE_STAGE_W2 = 19,
  OEX_TRACE_STAGE_LIQ = 20,
  OEX_TRACE_STAGE_LHQ = 21,
  OEX_TRACE_STAGE_STQ = 22,
  OEX_TRACE_STAGE_SCB = 23,
  OEX_TRACE_STAGE_MDB = 24,
  OEX_TRACE_STAGE_L1D = 25,
  OEX_TRACE_STAGE_BISQ = 26,
  OEX_TRACE_STAGE_BCTRL = 27,
  OEX_TRACE_STAGE_TMU = 28,
  OEX_TRACE_STAGE_TMA = 29,
  OEX_TRACE_STAGE_CUBE = 30,
  OEX_TRACE_STAGE_VEC = 31,
  OEX_TRACE_STAGE_TAU = 32,
  OEX_TRACE_STAGE_BROB = 33,
  OEX_TRACE_STAGE_ROB = 34,
  OEX_TRACE_STAGE_CMT = 35,
  OEX_TRACE_STAGE_FLS = 36,
  OEX_TRACE_STAGE_XCHK = 37,
  OEX_TRACE_STAGE_IB = 38,
} oex_trace_stage_t;

typedef enum {
  OEX_TRACE_LANE_FRONT = 0,
  OEX_TRACE_LANE_ALU0 = 1,
  OEX_TRACE_LANE_ALU1 = 2,
  OEX_TRACE_LANE_BRU0 = 3,
  OEX_TRACE_LANE_AGU0 = 4,
  OEX_TRACE_LANE_AGU1 = 5,
  OEX_TRACE_LANE_STD0 = 6,
  OEX_TRACE_LANE_STD1 = 7,
  OEX_TRACE_LANE_FSU0 = 8,
  OEX_TRACE_LANE_CMD0 = 9,
  OEX_TRACE_LANE_TPL0 = 10,
  OEX_TRACE_LANE_MEM = 11,
  OEX_TRACE_LANE_BLK = 12,
  OEX_TRACE_LANE_RET = 13,
  OEX_TRACE_LANE_AUX = 14,
} oex_trace_lane_t;

typedef struct {
  uint64_t cycle;
  uint64_t seq;
  uint64_t uop_uid;
  uint64_t parent_uop_uid;
  uint64_t pc;
  uint64_t raw;
  uint8_t len;
  uint16_t stage_id;
  uint16_t lane_id;
  uint8_t stall;
  uint32_t cause;
  uint8_t is_gen_uop;
} oex_trace_occ_t;

typedef enum { OEX_MEM_LOAD = 0, OEX_MEM_STORE = 1 } oex_mem_kind_t;

typedef struct {
  uint64_t seq;
  uint16_t mem_idx;
  oex_mem_kind_t kind;
  uint64_t pc;
  uint64_t addr;
  uint8_t size;
  uint64_t data;
} oex_mem_req_t;

typedef struct {
  uint64_t seq;
  uint16_t mem_idx;
  uint8_t ok;
  uint64_t data;
} oex_mem_rsp_t;

typedef struct {
  uint64_t seq;
  uint64_t pc;
  uint64_t raw;
  uint8_t len;
  uint8_t src0_valid;
  uint8_t src0_reg;
  uint64_t src0_data;
  uint8_t src1_valid;
  uint8_t src1_reg;
  uint64_t src1_data;
  uint8_t dst_valid;
  uint8_t dst_reg;
  uint64_t dst_data;
  uint8_t mem_valid;
  uint8_t mem_is_store;
  uint64_t mem_addr;
  uint64_t mem_wdata;
  uint64_t mem_rdata;
  uint8_t mem_size;
  uint8_t trap_valid;
  uint32_t trap_cause;
  uint64_t traparg0;
  uint64_t next_pc;
} oex_retire_row_t;

typedef oex_ctx_t* (*oex_create_fn)(const char* profile_name, const char* cfg_path);
typedef void (*oex_destroy_fn)(oex_ctx_t* ctx);
typedef int (*oex_push_inst_fn)(oex_ctx_t* ctx, const oex_inst_event_t* ev);
typedef int (*oex_tick_fn)(oex_ctx_t* ctx, uint32_t cycles);
typedef size_t (*oex_take_mem_req_fn)(oex_ctx_t* ctx, oex_mem_req_t* out, size_t cap);
typedef int (*oex_push_mem_rsp_fn)(oex_ctx_t* ctx, const oex_mem_rsp_t* rsp);
typedef size_t (*oex_pop_retire_fn)(oex_ctx_t* ctx, oex_retire_row_t* out, size_t cap);
typedef size_t (*oex_pop_trace_occ_fn)(oex_ctx_t* ctx, oex_trace_occ_t* out, size_t cap);
typedef int (*oex_last_error_fn)(oex_ctx_t* ctx, char* buf, size_t cap);

#endif
