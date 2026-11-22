import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def function(source: str, name: str) -> str:
    marker = f"HELPER({name})" if f"HELPER({name})" in source else name
    match = re.search(rf"\b{re.escape(marker)}\s*\(", source)
    if match is None:
        raise AssertionError(f"missing function {name}")
    start = source.rfind("\n", 0, match.start()) + 1
    opening = source.index("{", match.end())
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start : index + 1]
    raise AssertionError(f"unterminated function {name}")


class ScopedAtomicContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.helper = (ROOT / "target/linx/helper.c").read_text(encoding="utf-8")
        cls.translate = (ROOT / "target/linx/translate.c").read_text(
            encoding="utf-8"
        )
        cls.workflow = (
            ROOT / ".github/workflows/linx-scoped-scalar.yml"
        ).read_text(encoding="utf-8")

    def test_rmw_preflights_equal_ram_mappings_before_effects(self) -> None:
        probe = function(self.helper, "linx_scoped_atomic_probe_rw")
        self.assertIn("hosts.read != hosts.write", probe)
        self.assertLess(probe.index("hosts.read ="), probe.index("hosts.write ="))
        self.assertLess(
            probe.index("hosts.write ="), probe.index("hosts.read != hosts.write")
        )
        self.assertNotIn("cpu_ld", probe)
        self.assertNotIn("cpu_st", probe)

        for name in ("linx_scoped_rmw", "linx_scoped_cas"):
            body = function(self.helper, name)
            self.assertLess(
                body.index("linx_scoped_atomic_probe_rw"),
                body.index("linx_scoped_atomic_load"),
            )
            self.assertLess(
                body.index("linx_scoped_atomic_load"),
                body.index("linx_scoped_atomic_store"),
            )

    def test_lr_and_sc_keep_narrow_fault_contracts(self) -> None:
        lr = function(self.helper, "linx_scoped_lr")
        self.assertIn("linx_scoped_atomic_probe(env, addr, size, false", lr)
        self.assertLess(lr.index("linx_scoped_atomic_load"), lr.index("env->lr_addr"))

        sc = function(self.helper, "linx_scoped_sc")
        self.assertLess(sc.index("linx_lr_clear"), sc.index("if (!succeeds)"))
        self.assertLess(sc.index("return 1"), sc.index("linx_scoped_atomic_probe"))
        self.assertIn("linx_scoped_atomic_probe(env, addr, size, true", sc)

    def test_mmio_is_rejected_before_callbacks(self) -> None:
        probe = function(self.helper, "linx_scoped_atomic_probe")
        self.assertIn("probe_access", probe)
        self.assertIn("host == NULL", probe)
        self.assertIn("linx_scoped_atomic_raise_access", probe)
        self.assertNotIn("cpu_ld", probe)
        self.assertNotIn("cpu_st", probe)

    def test_scoped_atomic_discard_destinations_do_not_touch_queues(self) -> None:
        discard = function(self.translate, "linx_set_scoped_atomic_dest")
        self.assertIn("dst >= 24u && dst < 30u", discard)
        self.assertIn("#define linx_set_dest(DST, VALUE)", self.translate)
        self.assertIn("#undef linx_set_dest", self.translate)

    def test_hosted_lane_runs_decoded_core4_and_negative_contract(self) -> None:
        self.assertIn("run-scoped-atomic-core4-contract.sh", self.workflow)


if __name__ == "__main__":
    unittest.main()
