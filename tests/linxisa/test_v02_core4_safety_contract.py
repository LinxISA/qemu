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
            HELPER.index("static void linx_core4_abort_collective_locked") :
            HELPER.index("void linx_core4_reset")
        ]
        self.assertIn("collective_resume_pc[i]", abort)
        self.assertIn("halted = 0", abort)
        self.assertIn("qemu_cpu_kick", abort)
        commit = HELPER[
            HELPER.index("static bool linx_tile_group_mma_commit") :
            HELPER.index("void HELPER(linx_tile_commit)")
        ]
        self.assertGreaterEqual(
            commit.count("linx_core4_abort_collective_locked"), 3
        )

    def test_reset_clears_machine_owned_core4_state(self) -> None:
        reset = HELPER[
            HELPER.index("void linx_core4_reset") :
            HELPER.index("static bool linx_tile_group_mma_commit")
        ]
        self.assertIn("memset(core4->shared_tile", reset)
        self.assertIn("linx_core4_abort_collective_locked", reset)
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
        self.assertIn("cpu->core4->cpu[1] != NULL", pre_save)
        self.assertIn("env->tile_shared_binder_count != 0u", pre_save)

    def test_four_stacks_are_reserved_below_the_fdt(self) -> None:
        self.assertIn("#define LINX_VIRT_PE_STACK_SIZE 0x10000u", VIRT)
        self.assertIn("sp - s->pe_count * LINX_VIRT_PE_STACK_SIZE", VIRT)
        self.assertIn("fdt_addr = (stack_bottom -", VIRT)
        self.assertIn("if (image_end > stack_bottom)", VIRT)
        self.assertIn("sp - pe * LINX_VIRT_PE_STACK_SIZE", VIRT)


if __name__ == "__main__":
    unittest.main()
