#!/usr/bin/env python3
"""Safety contracts for the bounded PTO v0.2 Core4 implementation."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
TRANSLATE = (ROOT / "target/linx/translate.c").read_text(encoding="utf-8")
HELPER = (ROOT / "target/linx/helper.c").read_text(encoding="utf-8")
CPU = (ROOT / "target/linx/cpu.c").read_text(encoding="utf-8")
VIRT = (ROOT / "hw/linx/virt.c").read_text(encoding="utf-8")


class V02Core4SafetyContractTest(unittest.TestCase):
    def test_group_descriptor_requires_all_four_pes(self) -> None:
        profile = HELPER[
            HELPER.index("static bool linx_tile_group_cube_profile") :
            HELPER.index("typedef struct LinxTileRegSnapshot")
        ]
        self.assertIn("desc.reg != 0xfu", profile)
        for pe in range(4):
            self.assertIn(f"cpu->core4->cpu[{pe}] == NULL", profile)

    def test_group_fp32_profile_uses_dynamic_pe_local_dimensions(self) -> None:
        sizes = HELPER[
            HELPER.index("static bool linx_tile_group_fp32_sizes") :
            HELPER.index("typedef enum LinxTileLayout")
        ]
        self.assertIn("m * k * sizeof(uint32_t)", sizes)
        self.assertIn("k * n * sizeof(uint32_t)", sizes)
        self.assertIn("m * n * sizeof(uint32_t)", sizes)
        self.assertIn("left > 8192u", sizes)
        self.assertIn("right > 32768u", sizes)
        self.assertIn("output > 8192u", sizes)

        profile = HELPER[
            HELPER.index("static bool linx_tile_group_cube_profile") :
            HELPER.index("typedef struct LinxTileRegSnapshot")
        ]
        self.assertNotIn("env->lb[0] != 32u", profile)
        self.assertIn("env->lb[0], env->lb[1]", profile)
        self.assertIn("env->lb[0], env->lb[2]", profile)

        commit = HELPER[
            HELPER.index("static bool linx_tile_group_mma_commit") :
            HELPER.index("void HELPER(linx_tile_commit)")
        ]
        self.assertIn("core4->collective_m = env->lb[0]", commit)
        self.assertIn("core4->collective_n = env->lb[1]", commit)
        self.assertIn("core4->collective_k = env->lb[2]", commit)
        self.assertIn("output_bytes = m * n * sizeof(uint32_t)", commit)

    def test_group_shared_right_is_four_pe_local_fragments(self) -> None:
        profile = HELPER[
            HELPER.index("static bool linx_tile_group_cube_profile") :
            HELPER.index("typedef struct LinxTileRegSnapshot")
        ]
        execute = HELPER[
            HELPER.index("static bool linx_tile_group_mma_commit") :
            HELPER.index("void HELPER(linx_tile_commit)")
        ]

        self.assertIn(
            "shared->bytes == right_bytes * LINX_CORE4_PE_COUNT", profile
        )
        self.assertIn("shared->data + i * right_bytes", execute)
        self.assertIn("right_bytes, shared->dtype", execute)

    def test_cube_preflight_uses_encoded_output_size(self) -> None:
        preflight = HELPER[
            HELPER.index("static bool linx_tile_preflight_cube") :
            HELPER.index("static bool linx_tile_group_cube_profile")
        ]

        self.assertIn("UINT64_C(1) << (size_code + 4u)", preflight)
        self.assertIn("linx_tile_cube_dim(env->lb[0])", preflight)
        self.assertIn("linx_tile_cube_dim(env->lb[1])", preflight)
        self.assertNotIn("env->tile_reg_capacity[dst_tile]", preflight)

    def test_finisher_direct_boot_illegal_exits_only_without_evbase(self) -> None:
        illegal = CPU[
            CPU.index("case LINX_EXCP_ILLEGAL_INST:") :
            CPU.index("case LINX_EXCP_HW_BREAKPOINT:")
        ]

        self.assertIn('linx_cpu_env_enabled("LINX_VIRT_TEST_FINISHER")', illegal)
        self.assertIn("LINX_SSR_EVBASE] == 0u", illegal)
        self.assertIn("qemu_system_shutdown_request_with_code", illegal)
        self.assertLess(
            illegal.index("qemu_system_shutdown_request_with_code"),
            illegal.index("linx_deliver_sync_trap"),
        )

    def test_shared_fp32_tload_uses_encoded_dynamic_shape(self) -> None:
        size_decode = HELPER[
            HELPER.index("static bool linx_tile_get_shared_tload_size") :
            HELPER.index("static LinxTileIOTDesc linx_tile_get_iot_desc")
        ]
        preflight = HELPER[
            HELPER.index("static bool linx_tile_preflight_tma") :
            HELPER.index("static bool linx_tile_preflight_cube")
        ]
        commit_start = HELPER.index(
            "case LINX_BLOCK_TMA:", HELPER.index("void HELPER(linx_tile_commit)")
        )
        commit = HELPER[
            HELPER.index("case LINX_TMA_TLOAD:", commit_start) :
            HELPER.index("case LINX_TMA_TMOV:", commit_start)
        ]

        self.assertIn("*size_code_out = size_class + 2u", size_decode)
        self.assertIn("env->lb[0] <= env->lb[2]", preflight)
        self.assertIn(
            "bytes == (uint64_t)env->lb[1] * env->lb[2] * sizeof(float)",
            preflight,
        )
        self.assertIn("bytes <= LINX_SHARED_TILE_MAX_BYTES", preflight)
        self.assertIn("local_bytes * LINX_CORE4_PE_COUNT", commit)
        self.assertIn("shared->bytes = aggregate_bytes", commit)
        self.assertNotIn(
            "dtype == 1u) && size_code == 8u &&\n"
            "                 env->lb[0] == 32u",
            preflight,
        )

    def test_group_acc_reads_explicit_c_before_publishing_new_d(self) -> None:
        profile = HELPER[
            HELPER.index("static bool linx_tile_group_cube_profile") :
            HELPER.index("typedef struct LinxTileRegSnapshot")
        ]
        self.assertIn("source_count != (func == LINX_CUBE_TMATMUL_ACC ? 2u : 1u)",
                      profile)
        self.assertIn("*src_c_out = sources[0]", profile)
        self.assertIn("*src_a_out = sources[1]", profile)

        commit = HELPER[
            HELPER.index("static bool linx_tile_group_mma_commit") :
            HELPER.index("void HELPER(linx_tile_commit)")
        ]
        copy_c = commit.index(
            "memcpy(peer->tile_acc, peer->tile_reg[core4->collective_acc[i]]"
        )
        compute = commit.index("linx_tile_cube_compute_shared_b_057")
        publish_d = commit.index("memcpy(peer->tile_reg[dst], peer->tile_acc")
        self.assertLess(copy_c, compute)
        self.assertLess(compute, publish_d)

    def test_collective_abort_wakes_arrived_pes(self) -> None:
        abort = HELPER[
            HELPER.index("static void linx_tile_group_fail_locked") :
            HELPER.index("static bool linx_tile_group_mma_commit")
        ]
        self.assertIn("core4->collective_arrived & (1u << i)", abort)
        self.assertIn("linx_tile_group_reset_block", abort)
        self.assertIn("waiting->halted = 0", abort)
        self.assertIn(
            "waiting->exception_index = LINX_EXCP_ILLEGAL_INST", abort
        )
        self.assertIn("qemu_cpu_kick(waiting)", abort)
        commit = HELPER[
            HELPER.index("static bool linx_tile_group_mma_commit") :
            HELPER.index("void HELPER(linx_tile_commit)")
        ]
        self.assertGreaterEqual(
            commit.count("linx_tile_group_fail_locked(core4)"), 3
        )
        mismatch = commit[
            commit.index("if (!valid)") :
            commit.index("core4->collective_arrived |= bit")
        ]
        self.assertIn("linx_tile_group_reset_block(env)", mismatch)
        self.assertIn("env->pc = env->bpc", mismatch)

    def test_profile_invalid_late_pe_aborts_existing_arrivals(self) -> None:
        profile = HELPER[
            HELPER.index("static bool linx_tile_group_cube_profile") :
            HELPER.index("typedef struct LinxTileRegSnapshot")
        ]
        self.assertIn("linx_tile_datr_applicable", profile)

        commit = HELPER[
            HELPER.index("static bool linx_tile_group_mma_commit") :
            HELPER.index("void HELPER(linx_tile_commit)")
        ]
        profile_failure = commit[
            commit.index("if (!linx_tile_group_cube_profile") :
            commit.index("const unsigned pe")
        ]
        self.assertIn("qemu_mutex_lock(&core4->lock)", profile_failure)
        self.assertIn("core4->collective_arrived != 0u", profile_failure)
        self.assertIn("linx_tile_group_fail_locked(core4)", profile_failure)
        self.assertIn("linx_tile_group_reset_block(env)", profile_failure)
        self.assertIn("env->pc = env->bpc", profile_failure)

        dispatch = HELPER[
            HELPER.index("void HELPER(linx_tile_commit)") :
            HELPER.index("void HELPER(linx_tile_commit_shared_ior)")
        ]
        group_dispatch = dispatch[
            dispatch.index("if (group_mma)") :
            dispatch.index("if (env->tile_iot_count")
        ]
        self.assertNotIn("txn_gate.datr_legal", group_dispatch)
        self.assertIn("linx_tile_group_mma_commit", group_dispatch)

    def test_reset_clears_machine_owned_core4_state(self) -> None:
        reset = CPU[
            CPU.index("void linx_core4_reset") :
            CPU.index("static void linx_cpu_reset_hold")
        ]
        self.assertIn("qemu_mutex_lock(&core4->lock)", reset)
        self.assertIn("memset(core4->shared_tile", reset)
        self.assertIn("core4->collective_arrived = 0", reset)
        self.assertIn("env->tile_shared_binder_count = 0", reset)
        self.assertIn(
            "env->acr_block_state[acr].tile_shared_binder_count = 0", reset
        )
        virt_reset = VIRT[
            VIRT.index("static void linx_virt_reset") :
            VIRT.index("static uint32_t linx_memwatch_pack_attrs")
        ]
        self.assertLess(
            virt_reset.index("linx_core4_reset"),
            virt_reset.index("cpu_reset"),
        )

    def test_core4_mttcg_is_locked_and_migration_is_fail_closed(self) -> None:
        self.assertIn(".mttcg_supported = true", CPU)
        self.assertNotIn("qemu_tcg_mttcg_enabled()", VIRT)
        self.assertIn("QemuMutex lock", (ROOT / "target/linx/cpu.h").read_text(
            encoding="utf-8"))
        collective = HELPER[
            HELPER.index("static bool linx_tile_group_mma_commit") :
            HELPER.index("void HELPER(linx_tile_commit)")
        ]
        self.assertIn("qemu_mutex_lock(&core4->lock)", collective)
        self.assertIn("qemu_mutex_unlock(&core4->lock)", collective)
        pre_save = CPU[
            CPU.index("static int linx_cpu_pre_save") :
            CPU.index("static bool linx_cpu_post_load")
        ]
        self.assertIn("linx_core4_migration_state_live", pre_save)
        migration = CPU[
            CPU.index("static bool linx_core4_cpu_binder_live") :
            CPU.index("static int linx_cpu_pre_save")
        ]
        self.assertIn("shared->ready", migration)
        self.assertIn("core4->collective_arrived", migration)
        self.assertIn("env->tile_shared_binder_count", migration)
        self.assertIn(
            "env->acr_block_state[acr].tile_shared_binder_count", migration
        )

    def test_four_stacks_are_reserved_below_the_fdt(self) -> None:
        self.assertIn("#define LINX_VIRT_PE_STACK_SIZE 0x10000u", VIRT)
        self.assertIn("sp - s->pe_count * LINX_VIRT_PE_STACK_SIZE", VIRT)
        self.assertIn("fdt_addr = (stack_bottom -", VIRT)
        self.assertIn("if (image_end > stack_bottom)", VIRT)
        self.assertIn("sp - pe * LINX_VIRT_PE_STACK_SIZE", VIRT)

    def test_ram_size_is_checked_before_trampoline_subtraction(self) -> None:
        placement = VIRT[
            VIRT.index("tramp =") - 160 : VIRT.index("sp = (tramp")
        ]
        self.assertIn("machine->ram_size < 8u", placement)
        self.assertLess(
            placement.index("machine->ram_size < 8u"),
            placement.index("tramp ="),
        )


if __name__ == "__main__":
    unittest.main()
