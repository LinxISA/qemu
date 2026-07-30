#!/usr/bin/env python3
"""QEMU-side v0.57 LinxISA decode/metadata contract checks."""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
TARGET = ROOT / "target" / "linx"


def require(text: str, needle: str, path: Path) -> None:
    if needle not in text:
        raise AssertionError(f"{path}: missing {needle!r}")


def main() -> None:
    insn32 = (TARGET / "insn32.decode").read_text(encoding="utf-8")
    meta = (TARGET / "linx_opcode_meta_gen.h").read_text(encoding="utf-8")
    helper = (TARGET / "helper.c").read_text(encoding="utf-8")
    translate = (TARGET / "translate.c").read_text(encoding="utf-8")
    cpu_h = (TARGET / "cpu.h").read_text(encoding="utf-8")
    cpu_c = (TARGET / "cpu.c").read_text(encoding="utf-8")

    for decode_name in (
        "bstart_tprefetch",
        "bstart_mgather",
        "bstart_mscatter",
        "bstart_mgather_mask",
        "bstart_mscatter_mask",
        "casb",
        "cash",
        "casw",
        "casd",
        "dma",
    ):
        require(insn32, decode_name, TARGET / "insn32.decode")
        require(meta, f'"{decode_name}"', TARGET / "linx_opcode_meta_gen.h")

    expected_matches = {
        "bstart_tprefetch": "0x311181",
        "bstart_mgather": "0x411181",
        "bstart_mscatter": "0x511181",
        "bstart_mgather_mask": "0x611181",
        "bstart_mscatter_mask": "0x711181",
        "casb": "0x1b",
        "cash": "0x101b",
        "casw": "0x201b",
        "casd": "0x301b",
        "dma": "0x700b",
    }
    for name, match in expected_matches.items():
        require(meta, f'.mnemonic="{name}"', TARGET / "linx_opcode_meta_gen.h")
        require(meta, f".match=UINT64_C({match})", TARGET / "linx_opcode_meta_gen.h")

    require(translate, "trans_bstart_tprefetch", TARGET / "translate.c")
    require(translate, "trans_casb", TARGET / "translate.c")
    require(translate, "trans_dma", TARGET / "translate.c")

    require(helper, "LINX_TMA_TPREFETCH = 3", TARGET / "helper.c")
    require(helper, "linx_tile_prefetch", TARGET / "helper.c")
    require(helper, "linx_tile_collect_sources", TARGET / "helper.c")
    require(helper, "linx_tile_set_elem_bytes", TARGET / "helper.c")
    require(cpu_h, "tile_reg_elem_bytes", TARGET / "cpu.h")
    require(cpu_c, "VMSTATE_UINT8_ARRAY_V(env.tile_reg_elem_bytes", TARGET / "cpu.c")

    tile_output_expr = helper[
        helper.index("const bool tile_output =") :
        helper.index("const bool tile_output =") + 600
    ]
    if "LINX_TMA_TPREFETCH" in tile_output_expr:
        raise AssertionError("TPREFETCH must not reserve or publish a tile output")

    print("ok: QEMU v0.57 decode metadata contract")


if __name__ == "__main__":
    main()
