"""Behavior checks for the repository gate, runnable without the Windows SDK."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from check_structure import check, condition_findings, tokens


CHECKER = Path(__file__).resolve().parents[1] / "scripts/check_structure.py"


class StructureRules(unittest.TestCase):
    def errors(self, source, old="", path="app/helper.c"):
        return check({path: old}, {path: source})

    def test_core_cannot_include_app_or_ui_headers(self):
        before = {"app/store.h": "", "core/a.c": ""}
        for header in ['"store.h"', '"../app/store.h"', '<commctrl.h>', '"..\\app\\store.h"']:
            with self.subTest(header=header):
                errors = check(before, {**before, "core/a.c": f"#include {header}\n"})
                self.assertTrue(any("core includes UI header" in error for error in errors))
        errors = check(before, {**before, "core/a.c": "#include <windows.h>\n#include <winhttp.h>\n"})
        self.assertFalse(any("UI header" in error for error in errors))

    def test_comments_and_literals_are_not_calls_or_includes(self):
        source = '/* #include "../app/store.h" */\n// api_call(c);\nconst char *s = "WinHttpOpen(x)";\n'
        self.assertEqual([], self.errors(source))
        macro = '#define X /*\n#include "../app/store.h"\n*/ 1\n'
        errors = self.errors(macro, path="core/a.c")
        self.assertFalse(any("UI header" in error for error in errors))

    def test_direct_api_and_http_calls_fail_outside_adapters(self):
        for call in ["api_call", "api_stream", "api_upload", "WinHttpOpen"]:
            with self.subTest(call=call):
                self.assertTrue(self.errors(f"void f(void) {{ {call}(c); }}"))
        self.assertEqual([], self.errors("void f(void) { api_call(c); }", path="app/store.c"))
        self.assertEqual([], self.errors("void f(void) { WinHttpOpen(c); }", path="app/ws.c"))

    def test_if_line_limit_and_exact_boundary(self):
        self.assertEqual([], self.errors("if (a &&\n b &&\n c) f();"))
        self.assertTrue(any("spans 4 lines" in error for error in self.errors("if (a &&\n b &&\n c &&\n d) f();")))

    def test_five_operands_pass_six_fail_including_nested_groups(self):
        self.assertEqual([], self.errors("if (a && b && c && (d || e)) f();"))
        self.assertTrue(any("6 logical operands" in error for error in self.errors("if (!(a && b) || (c && d && e && f)) g();")))

    def test_call_arguments_and_subscripts_are_one_operand(self):
        source = "if (fn(a && b && c && d && e && f) && values[a || b || c] && z) f();"
        self.assertEqual([], self.errors(source))

    def test_comments_strings_and_directives_do_not_create_conditions(self):
        source = '/* if (a && b && c && d && e && f) */\nconst char *s = "if (a && b && c && d && e && f)";\n'
        source += '#define GUARD(x) if (a && b && \\\n c && d && e && f) x\n'
        self.assertEqual([], list(condition_findings(tokens(source))))

    def test_literals_with_escapes_and_comment_markers(self):
        self.assertEqual([], self.errors(r'''const char *s = "escaped \" if (a && b && c && d && e && f) //";'''))

    def test_old_debt_may_move_but_not_gain_another_copy(self):
        debt = "if (a && b && c && d && e && f) g();"
        self.assertEqual([], self.errors("\n" + debt, old=debt))
        self.assertEqual(1, len(self.errors(debt + debt, old=debt)))
        self.assertTrue(self.errors(debt.replace("f)", "g)"), old=debt))

    def test_existing_long_condition_cannot_span_more_lines(self):
        old = "if (a &&\n b &&\n c &&\n d) f();"
        new = old.replace(" d)", "\n d)")
        self.assertTrue(any("spans 5 lines" in error for error in self.errors(new, old=old)))

    def test_presentation_size_allows_only_existing_debt(self):
        for path in ["app/screen_new.c", "app/project_files.c", "app/forge_site.c"]:
            with self.subTest(path=path):
                self.assertEqual([], self.errors("\n" * 300, path=path))
                self.assertTrue(self.errors("\n" * 301, path=path))
                self.assertEqual([], self.errors("\n" * 399, old="\n" * 400, path=path))
                self.assertTrue(self.errors("\n" * 401, old="\n" * 400, path=path))

    def test_comment_only_core_changes_do_not_need_tests(self):
        self.assertEqual([], self.errors("/* explanation */\nint x = 1;\n", old="int x=1;", path="core/a.c"))

    def test_core_logic_and_macro_changes_need_a_real_test_delta(self):
        before = {"core/a.c": "int x = 1;", "tests/core_a_tests.c": "void test(void) { CHECK_INT(x, 1); }"}
        after = {**before, "core/a.c": "int x = 2;"}
        self.assertTrue(any("core test gate" in error for error in check(before, after)))
        after["tests/core_a_tests.c"] += "// a dummy comment\n"
        self.assertTrue(any("core test gate" in error for error in check(before, after)))
        after["tests/core_a_tests.c"] = "void test(void) { CHECK_INT(x, 2); }"
        self.assertEqual([], check(before, after))
        self.assertTrue(self.errors("#define X 2\n", old="#define X 1\n", path="core/a.h"))

    def test_changed_c_operators_are_not_mistaken_for_whitespace(self):
        self.assertTrue(self.errors("int f(void) { return a + ++b; }", old="int f(void) { return a++ + b; }", path="core/a.c"))

    def test_multiline_macro_comment_change_is_exempt(self):
        old = "#define X 1 /* old\n explanation */\n"
        self.assertEqual([], self.errors(old.replace("explanation", "reason"), old=old, path="core/a.h"))

    def test_continued_line_comment_does_not_create_api_call(self):
        source = "// comment " + "\\\n" + "api_call(c);\n"
        self.assertEqual([], self.errors(source))

    def test_deleted_tests_and_runner_only_edits_do_not_satisfy_gate(self):
        before = {"core/a.c": "int x;", "tests/core_a_tests.c": "void t(void) {}"}
        after = {"core/a.c": "int y;", "tests/core_tests.c": "int main(void) { return 0; }"}
        self.assertTrue(any("core test gate" in error for error in check(before, after)))

    def test_new_suppressions_fail_and_existing_ones_may_shrink(self):
        for suppression in ["// cppcheck-suppress memleak\n", "// cppcheck-suppress-file memleak\n",
                            '#pragma GCC diagnostic ignored "-Wshadow"\n',
                            '#pragma clang diagnostic ignored "-Wshadow"\n',
                            "#pragma warning(disable: 4100)\n", "#pragma warning(push, 0)\n"]:
            with self.subTest(suppression=suppression):
                self.assertTrue(self.errors(suppression))
                self.assertEqual([], self.errors("", old=suppression))
        self.assertEqual([], self.errors('const char *s = "cppcheck-suppress memleak";'))

    def test_makefile_suppressions_and_coverage_cannot_weaken(self):
        old = "COVERAGE_MIN ?= 90\nlint:\n\tcppcheck --suppress=missingIncludeSystem\n"
        for new in [old.replace("90", "89"), old.replace("90", "0"), old.replace("COVERAGE_MIN", "OLD_MIN"),
                    old + "\tcppcheck --suppress=memleak\n"]:
            with self.subTest(new=new):
                self.assertTrue(self.errors(new, old=old, path="Makefile"))
        self.assertEqual([], self.errors(old.replace("90", "91"), old=old, path="Makefile"))
        self.assertEqual([], self.errors(old + "# example --suppress=memleak\n", old=old, path="Makefile"))


class GitIntegration(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.git("init", "-b", "main")
        self.git("config", "user.name", "Structure tests")
        self.git("config", "user.email", "structure@example.test")
        self.write("core/a.c", "int value = 1;\n")
        self.write("tests/core_a_tests.c", "void test(void) { CHECK_INT(value, 1); }\n")
        self.commit()
        self.git("switch", "-c", "feature")

    def git(self, *args):
        return subprocess.run(["git", *args], cwd=self.root, check=True, capture_output=True, text=True).stdout.strip()

    def write(self, path, text):
        target = self.root / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")

    def commit(self):
        self.git("add", ".")
        self.git("commit", "-qm", "fixture")

    def run_check(self, base="main"):
        return subprocess.run([sys.executable, str(CHECKER), base], cwd=self.root, capture_output=True, text=True)

    def test_clean_branch_passes(self):
        result = self.run_check()
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("Structure checks passed", result.stdout)

    def test_unstaged_and_staged_core_changes_fail_without_test_changes(self):
        self.write("core/a.c", "int value = 2;\n")
        self.assertEqual(1, self.run_check().returncode)
        self.git("add", "core/a.c")
        self.assertEqual(1, self.run_check().returncode)
        self.write("tests/core_a_tests.c", "void test(void) { CHECK_INT(value, 2); }\n")
        self.assertEqual(0, self.run_check().returncode)

    def test_committed_changes_are_checked(self):
        self.write("app/screen_bad.c", "void f(void) { api_call(c); }\n")
        self.commit()
        result = self.run_check()
        self.assertEqual(1, result.returncode)
        self.assertIn("bypasses store", result.stderr)

    def test_untracked_source_is_checked(self):
        self.write("app/untracked.c", "void f(void) { WinHttpOpen(c); }\n")
        result = self.run_check()
        self.assertEqual(1, result.returncode)
        self.assertIn("WinHTTP belongs", result.stderr)

    def test_missing_ref_and_non_repository_fail_closed(self):
        self.assertEqual(2, self.run_check("missing-base").returncode)
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run([sys.executable, str(CHECKER)], cwd=directory, capture_output=True)
            self.assertEqual(2, result.returncode)

    def test_merge_base_ignores_changes_only_on_base_branch(self):
        self.git("switch", "main")
        self.write("app/screen_unrelated.c", "void f(void) { api_call(c); }\n")
        self.commit()
        self.git("switch", "feature")
        result = self.run_check()
        self.assertEqual(0, result.returncode, result.stderr)

    def test_explicit_stack_parent_excludes_inherited_changes(self):
        self.write("core/a.c", "int value = 2;\n")
        self.commit()
        self.git("branch", "parent")
        self.write("app/helper.c", "int helper(void) { return 1; }\n")
        result = self.run_check("parent")
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(1, self.run_check("main").returncode)


if __name__ == "__main__":
    unittest.main()
