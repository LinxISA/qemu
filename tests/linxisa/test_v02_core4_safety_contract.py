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

    def test_shared_cube_preserves_versions_and_uses_descriptors(self) -> None:
        profile = HELPER[
            HELPER.index("static bool linx_tile_shared_cube_operand_legal") :
            HELPER.index("typedef struct LinxTileRegSnapshot")
        ]
        self.assertIn("shared->valid_rows != rows", profile)
        self.assertIn("shared->valid_cols != cols", profile)
        self.assertNotIn("32u * 32u", profile)
        commit = HELPER[
            HELPER.index("static bool linx_tile_group_mma_commit") :
            HELPER.index("void HELPER(linx_tile_commit)")
        ]
        self.assertNotIn("memset(shared_right", commit)
        self.assertNotIn("memset(shared_left", commit)

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

    def test_core4_migration_and_mttcg_are_fail_closed(self) -> None:
        self.assertIn(".mttcg_supported = false", CPU)
        self.assertIn("qemu_tcg_mttcg_enabled()", VIRT)
        self.assertIn("use -accel tcg,thread=single", VIRT)
        pre_save = CPU[
            CPU.index("static int linx_cpu_pre_save") :
            CPU.index("static bool linx_cpu_post_load")
        ]
        self.assertIn("linx_core4_migration_state_live", pre_save)
        migration = CPU[
            CPU.index("static bool linx_core4_cpu_binder_live") :
            CPU.index("static int linx_cpu_pre_save")
        ]
        self.assertIn("shared->allocation_mask", migration)
        self.assertIn("shared->initialized_mask", migration)
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
