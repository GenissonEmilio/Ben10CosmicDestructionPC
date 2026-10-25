import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "tools/desktop")]
from compact_register import compact
from import_disc import Budget, ImportError_, candidates, extract_zip, inspect_disc, safe_name


class ImportTests(unittest.TestCase):
    def test_unsafe_paths(self):
        for name in ("../escape.iso", "/tmp/escape", "C:/escape", "x\\..\\escape", "x/../../escape"):
            with self.subTest(name=name), self.assertRaises(ImportError_):
                safe_name(name)

    def test_symlink_and_duplicate_entries(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            archive = root / "link.zip"
            info = zipfile.ZipInfo("disc.iso")
            info.create_system = 3
            info.external_attr = 0o120777 << 16
            with zipfile.ZipFile(archive, "w") as out:
                out.writestr(info, "/etc/passwd")
            with self.assertRaises(ImportError_):
                extract_zip(archive, root / "out", Budget())
            with zipfile.ZipFile(archive, "w") as out:
                out.writestr("disc.iso", "first")
                out.writestr("DISC.ISO", "second")
            (root / "out").mkdir()
            with self.assertRaises(ImportError_):
                extract_zip(archive, root / "out", Budget())

    def test_nested_budget_and_no_execution(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            inner = root / "inner.zip"
            with zipfile.ZipFile(inner, "w") as out:
                out.writestr("disc.iso", b"x" * 500)
                out.writestr("readme.sh", "exit 1")
            with self.assertRaises(ImportError_):
                list(candidates(inner, root, Budget(300)))
            items = list(candidates(inner, root, Budget()))
            self.assertEqual([item.name for item in items], ["disc.iso"])

    def test_truncated_disc(self):
        with tempfile.TemporaryDirectory() as temp:
            image = Path(temp) / "disc.iso"
            image.write_bytes(b"CD001")
            with self.assertRaises(ImportError_):
                inspect_disc(image)


class DispatchTests(unittest.TestCase):
    def fixture(self, assignments):
        return ('#include "ps2_runtime.h"\n'
                'PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[8u] = {};\n'
                'namespace {\nstruct GeneratedFunctionTableInitializer {\n'
                '    GeneratedFunctionTableInitializer() {\n' + assignments +
                '    }\n};\nstatic const GeneratedFunctionTableInitializer g_generatedFunctionTableInitializer;\n}\n')

    def test_holes_and_different_functions_preserved(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "register.cpp"
            path.write_text(self.fixture(''.join(
                f'        g_ps2RecompiledFunctionTable[{slot}] = {fn}; // 0x1000\n'
                for slot, fn in ((0, "one"), (1, "one"), (3, "one"), (4, "two")))))
            compact(path)
            result = path.read_text()
            self.assertIn("{0u, 2u, one}", result)
            self.assertIn("{3u, 1u, one}", result)
            self.assertIn("{4u, 1u, two}", result)
            compact(path)
            self.assertEqual(result, path.read_text())

    def test_unknown_statement_is_not_discarded(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "register.cpp"
            original = self.fixture("        unknown_operation();\n")
            path.write_text(original)
            with self.assertRaises(ValueError):
                compact(path)
            self.assertEqual(path.read_text(), original)

    def test_duplicate_and_outside_slots_rejected(self):
        for slots in ((1, 1), (8,)):
            with tempfile.TemporaryDirectory() as temp:
                path = Path(temp) / "register.cpp"
                path.write_text(self.fixture(''.join(
                    f'        g_ps2RecompiledFunctionTable[{slot}] = one; // 0x1000\n'
                    for slot in slots)))
                with self.assertRaises(ValueError):
                    compact(path)


if __name__ == "__main__":
    unittest.main()
