"""Behavior checks for the repository gate, runnable without the Windows SDK."""

from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

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

    def test_existing_long_condition_may_shrink(self):
        old = "if (a &&\n b &&\n c &&\n d &&\n e) f();"
        new = old.replace(" d &&\n e", " d && e")
        self.assertEqual([], self.errors(new, old=old))

    def test_duplicate_condition_debt_cannot_be_reused(self):
        short = "if (a &&\n b &&\n c &&\n d && e) f();"
        long = short.replace(" d && e", " d &&\n e")
        self.assertEqual([], self.errors(short + long, old=long + short))
        self.assertTrue(self.errors(long + long, old=long + short))
        self.assertTrue(self.errors(short + short, old=long))

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
                            "#pragma warning(disable: 4100)\n", "#pragma warning(push, 0)\n",
                            "#pragma warning(suppress: 4100)\n",
                            "__pragma(warning(disable: 4100))\n",
                            "__pragma(warning(suppress: 4100))\n", "__pragma(warning(push, 0))\n"]:
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
        self.git("config", "commit.gpgsign", "false")
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

    def run_check(self, base="main", *options):
        return subprocess.run([sys.executable, str(CHECKER), base, *options], cwd=self.root, capture_output=True, text=True)

    def test_clean_branch_passes(self):
        result = self.run_check()
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("Structure checks passed", result.stdout)

    def test_fixture_commits_ignore_inherited_signing(self):
        config = self.root / "global.gitconfig"
        config.write_text('[commit]\n\tgpgsign = true\n[gpg]\n\tprogram = missing-structure-test-signer\n',
                          encoding="utf-8")
        fixture = GitIntegration("test_clean_branch_passes")
        self.addCleanup(fixture.doCleanups)
        with patch.dict(os.environ, {"GIT_CONFIG_GLOBAL": str(config)}):
            fixture.setUp()
            fixture.test_clean_branch_passes()

    def test_header_path_changes_recheck_unchanged_core_includes(self):
        self.write("core/a.c", '#include "thing.h"\nint value = OWNER_CORE;\n')
        self.write("core/thing.h", "#define OWNER_CORE 1\n")
        self.write("app/thing.h", "#define OWNER_CORE 9\n")
        self.commit()
        (self.root / "core/thing.h").unlink()
        self.write("tests/core_a_tests.c", "void test(void) { CHECK_INT(value, 9); }\n")
        result = self.run_check("HEAD")
        self.assertEqual(1, result.returncode, result.stderr)
        self.assertIn("core/a.c:1: core includes UI header thing.h", result.stderr)
        self.assertNotIn("core test gate", result.stderr)
        self.write("core/thing.h", "#define OWNER_CORE 1\n")
        self.assertEqual(0, self.run_check("HEAD").returncode)

    def test_added_app_header_rechecks_unchanged_core_includes(self):
        self.write("core/a.c", '#include <thing.h>\nint value = 1;\n')
        self.commit()
        self.write("app/thing.h", "")
        result = self.run_check("HEAD")
        self.assertEqual(1, result.returncode, result.stderr)
        self.assertIn("core/a.c:1: core includes UI header thing.h", result.stderr)
        self.assertNotIn("core test gate", result.stderr)
        self.commit()
        self.write("app/unrelated.h", "")
        self.assertEqual(0, self.run_check("HEAD").returncode)

    def test_raw_strings_exclude_contents_and_preserve_live_code(self):
        contents = ('" api_call(c); WinHttpOpen(c);\n'
                    'if (a && b && c && d && e && f) g();\n'
                    '#include <commctrl.h>\n'
                    '#pragma GCC diagnostic ignored "-Wshadow"\n'
                    '// cppcheck-suppress memleak\n'
                    '/* sample */ )other"')
        for prefix in ["", "u8", "u", "U", "L"]:
            for delimiter in ["", "tag", "abcdefghijklmnop"]:
                with self.subTest(prefix=prefix, delimiter=delimiter):
                    source = f'auto s = {prefix}R"{delimiter}({contents}){delimiter}";\n'
                    self.write("app/canvas.cpp", source)
                    result = self.run_check()
                    self.assertEqual(0, result.returncode, result.stderr)
                    self.write("app/canvas.cpp", source + "api_call(c);\n")
                    result = self.run_check()
                    self.assertEqual(1, result.returncode, result.stderr)
                    line = source.count("\n") + 1
                    self.assertIn(f"app/canvas.cpp:{line}: api_call bypasses store", result.stderr)
        self.write("app/canvas.cpp", '#define SAMPLE R"tag(' + contents + ')tag"\n')
        self.assertEqual(0, self.run_check().returncode)

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

    def test_audit_reports_unchanged_debt_and_clears_after_repair(self):
        self.write("app/screen_legacy.c", "void f(void) { api_call(c); }\n" + "\n" * 300)
        self.commit()
        self.assertEqual(0, self.run_check("HEAD").returncode)
        result = self.run_check("HEAD", "--audit")
        self.assertEqual(1, result.returncode)
        self.assertIn("presentation file has 301 lines", result.stderr)
        self.assertIn("api_call bypasses store", result.stderr)
        self.write("app/screen_legacy.c", "void f(void) { store_call(c); }\n")
        result = self.run_check("HEAD", "--audit")
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("Full-tree structure audit passed", result.stdout)

    def test_untracked_source_is_checked(self):
        self.write("app/untracked.c", "void f(void) { WinHttpOpen(c); }\n")
        result = self.run_check()
        self.assertEqual(1, result.returncode)
        self.assertIn("WinHTTP belongs", result.stderr)

    def test_msvc_warning_suppressions_fail(self):
        for source in ["#pragma warning(suppress: 4100)\n",
                       "__pragma(warning(disable: 4100))\n",
                       "__pragma(warning(suppress: 4100))\n",
                       "__pragma(warning(push, 0))\n",
                       "#define QUIET __pragma(warning(disable: 4100))\n"]:
            with self.subTest(source=source):
                self.write("app/helper.c", source)
                result = self.run_check()
                self.assertEqual(1, result.returncode, result.stderr)
                self.assertIn("warning suppressions increased", result.stderr)

    def test_warning_examples_and_non_suppressing_pragmas_pass(self):
        source = ('// __pragma(warning(disable: 4100))\n'
                  '/* #pragma warning(suppress: 4100) */\n'
                  'const char *s = "__pragma(warning(disable: 4100))";\n'
                  'const char *t = "\\\n#pragma warning(suppress: 4100)";\n'
                  '#define EXAMPLE "__pragma(warning(disable: 4100))"\n'
                  '__pragma(warning(push))\n__pragma(warning(pop))\n'
                  '#pragma warning(default: 4100)\n')
        self.write("app/helper.c", source)
        result = self.run_check()
        self.assertEqual(0, result.returncode, result.stderr)

    def test_all_literal_coverage_assignments_preserve_floor(self):
        self.write("Makefile", "COVERAGE_MIN ?= 90\n")
        self.commit()
        for operator in ["?=", "=", ":=", "+="]:
            for prefix in ["", "override ", "export "]:
                with self.subTest(operator=operator, prefix=prefix):
                    self.write("Makefile", f"COVERAGE_MIN ?= 90\n{prefix}COVERAGE_MIN {operator} 0\n")
                    result = self.run_check("HEAD")
                    self.assertEqual(1, result.returncode, result.stderr)
                    self.assertIn("COVERAGE_MIN was removed or lowered", result.stderr)
        self.write("Makefile", "COVERAGE_MIN := 91\nCOVERAGE_MIN = 92\n# COVERAGE_MIN = 0\n")
        self.assertEqual(0, self.run_check("HEAD").returncode)

    def test_target_specific_decimal_and_immediate_coverage_assignments(self):
        base = "COVERAGE_MIN ?= 90\n"
        self.write("Makefile", base)
        self.commit()
        for assignment in ["coverage: COVERAGE_MIN = 0",
                           "coverage: override COVERAGE_MIN := 89.5",
                           "override COVERAGE_MIN := 89.5",
                           "override COVERAGE_MIN ::= 0"]:
            with self.subTest(assignment=assignment):
                self.write("Makefile", base + assignment + "\n")
                result = self.run_check("HEAD")
                self.assertEqual(1, result.returncode, result.stderr)
                self.assertIn("COVERAGE_MIN was removed or lowered", result.stderr)
        for assignment in ["coverage: COVERAGE_MIN = 90",
                           "override COVERAGE_MIN := 90.5",
                           "override COVERAGE_MIN ::= 91",
                           "# coverage: COVERAGE_MIN = 0"]:
            with self.subTest(assignment=assignment):
                self.write("Makefile", base + assignment + "\n")
                self.assertEqual(0, self.run_check("HEAD").returncode)
        self.write("Makefile", "COVERAGE_MIN := 90.5\n")
        self.commit()
        self.write("Makefile", "COVERAGE_MIN := 90.4\n")
        self.assertEqual(1, self.run_check("HEAD").returncode)

    def test_continued_literal_coverage_assignments_preserve_floor(self):
        base = "COVERAGE_MIN ?= 90\n"
        self.write("Makefile", base)
        self.commit()
        for newline in ["\n", "\r\n"]:
            for prefix in ["", "override ", "coverage: "]:
                with self.subTest(newline=newline, prefix=prefix):
                    assignment = prefix + "COVERAGE_MIN := \\" + newline + "\t"
                    self.write("Makefile", base + assignment + "0\n")
                    result = self.run_check("HEAD")
                    self.assertEqual(1, result.returncode, result.stderr)
                    self.assertIn("COVERAGE_MIN was removed or lowered", result.stderr)
                    for value in ["90", "91"]:
                        self.write("Makefile", assignment + value + "\n")
                        self.assertEqual(0, self.run_check("HEAD").returncode)
                    self.write("Makefile", base + "# example \\" + newline
                               + "COVERAGE_MIN := 0\n")
                    self.assertEqual(0, self.run_check("HEAD").returncode)
        self.write("Makefile", "COVERAGE_MIN := \\\n90\n")
        self.commit()
        self.write("Makefile", "COVERAGE_MIN := \\\n89.5\n")
        self.assertEqual(1, self.run_check("HEAD").returncode)

    def test_comment_boundaries_preserve_live_code_and_directives(self):
        for code, diagnostic in [("api_call(0);", "bypasses store"),
                                 ("if (a && b && c && d && e && f) g();", "6 logical operands")]:
            with self.subTest(code=code):
                self.write("app/helper.c", "/* license */\nvoid f(void) { " + code
                           + " }\n/* settings */ #define FLAG 1\n")
                result = self.run_check()
                self.assertEqual(1, result.returncode, result.stderr)
                self.assertIn(diagnostic, result.stderr)
        self.write("app/helper.c", "")
        self.write("tests/core_a_tests.c", "void test(void) { CHECK_INT(value, 2); }\n")
        for prefix in ["/* note */\n/* reason\n*/ ", "/* first */\n\n/* second */ "]:
            with self.subTest(prefix=prefix):
                self.write("core/a.c", prefix + '#include "../app/store.h"\n')
                result = self.run_check()
                self.assertEqual(1, result.returncode, result.stderr)
                line = prefix.count("\n") + 1
                self.assertIn(f"core/a.c:{line}: core includes UI header", result.stderr)
                self.write("core/a.c", "int value = 1;\n")
                self.write("app/helper.c", prefix + '#pragma GCC diagnostic ignored "-Wshadow"\n')
                self.assertIn("warning suppressions increased", self.run_check().stderr)
                self.write("app/helper.c", prefix + "#define GUARD if (a && b && c && d && e && f) g();\n")
                self.assertEqual(0, self.run_check().returncode)
        self.write("app/helper.c", '/* license */\nconst char *s = "api_call(0);";\n'
                   '/* settings */ #define FLAG 1\n')
        self.assertEqual(0, self.run_check().returncode)

    def test_macro_name_adjacency_requires_core_test_delta(self):
        before = "int F=10;\n#define F(x) +1\nint f(void) { int x=2; return F + (x); }\n"
        self.write("core/a.c", before)
        self.commit()
        for replacement in ["F (x)", "F\t(x)", "F/**/(x)", "F \\\n(x)"]:
            with self.subTest(replacement=replacement):
                self.write("core/a.c", before.replace("F(x)", replacement))
                result = self.run_check("HEAD")
                self.assertEqual(1, result.returncode, result.stderr)
                self.assertIn("core test gate", result.stderr)
        for replacement in ["F( x )", "F(/**/x)", "F\\\n(x)", "F\\\r\n(x)"]:
            with self.subTest(replacement=replacement):
                self.write("core/a.c", before.replace("F(x)", replacement))
                self.assertEqual(0, self.run_check("HEAD").returncode)
        self.write("core/a.c", before.replace("F(x)", "F (x)"))
        self.write("tests/core_a_tests.c", "void test(void) { CHECK_INT(f(), 5); }\n")
        self.assertEqual(0, self.run_check("HEAD").returncode)

    def test_quoted_core_local_headers_precede_app_basename_fallback(self):
        self.write("core/thing.h", "#define OWNER_CORE 1\n")
        self.write("app/thing.h", "#error APP_WAS_INCLUDED\n")
        self.commit()
        self.write("tests/core_a_tests.c", "void test(void) { CHECK_INT(value, 2); }\n")
        for path, header in [("core/a.c", "thing.h"), ("core/a.c", "./thing.h"),
                             ("core/nested/a.c", "../thing.h")]:
            with self.subTest(path=path, header=header):
                self.write(path, f'#include "{header}"\nint value = OWNER_CORE;\n')
                result = self.run_check("HEAD")
                self.assertEqual(0, result.returncode, result.stderr)
                self.write(path, "")
        self.write("core/commctrl.h", "")
        for header in ['<thing.h>', '"../app/thing.h"', '"commctrl.h"',
                       '"missing/thing.h"', '"../app/../core/thing.h"']:
            with self.subTest(header=header):
                self.write("core/a.c", f"#include {header}\n")
                result = self.run_check("HEAD")
                self.assertEqual(1, result.returncode, result.stderr)
                self.assertIn("core includes UI header", result.stderr)
        self.write("core/a.c", '#include "thing.h"\nint value = OWNER_CORE;\n')
        self.commit()
        self.write("core/a.c", '#include "thing.h"\nint value = OWNER_CORE + 1;\n')
        self.write("tests/core_a_tests.c", "void test(void) { CHECK_INT(value, 3); }\n")
        (self.root / "core/thing.h").unlink()
        self.assertIn("core includes UI header", self.run_check("HEAD").stderr)

    def test_lowered_msvc_warning_levels_count_suppressions(self):
        for level in range(4):
            for source in [f"#pragma warning(push, {level})",
                           f"__pragma(warning(push, {level}))",
                           f'_Pragma("warning(push, {level})")']:
                with self.subTest(source=source):
                    self.write("app/helper.c", source + "\n")
                    result = self.run_check()
                    self.assertEqual(1, result.returncode, result.stderr)
                    self.assertIn("warning suppressions increased", result.stderr)
                    self.write("app/helper.c", '/* ' + source + ' */\n// ' + source + '\n')
                    self.assertEqual(0, self.run_check().returncode)
        for operand in ["push", "push, 4"]:
            self.write("app/helper.c", f'#pragma warning({operand})\n'
                       f'__pragma(warning({operand}))\n_Pragma("warning({operand})")\n')
            self.assertEqual(0, self.run_check().returncode)
        self.write("app/helper.c", 'const char *s = "__pragma(warning(push, 1))";\n'
                   'const char *t = "_Pragma(\\\"warning(push, 1)\\\")";\n')
        self.assertEqual(0, self.run_check().returncode)

    def test_builtin_casts_do_not_hide_boolean_operands(self):
        expression = "a && b && c && d && e && f"
        for typename in ["int", "char", "short", "long", "signed", "unsigned", "float",
                         "double", "_Bool", "bool", "wchar_t", "char16_t", "char32_t",
                         "unsigned long", "const int *"]:
            with self.subTest(typename=typename):
                self.write("app/helper.c", f"if (({typename})({expression})) g();\n")
                result = self.run_check()
                self.assertEqual(1, result.returncode, result.stderr)
                self.assertIn("6 logical operands", result.stderr)
        self.write("app/helper.c", "if ((int)(a && b && c && d && e)) g();\n")
        self.assertEqual(0, self.run_check().returncode)
        self.write("app/helper.c", f"if ((*callback)({expression})) g();\n")
        self.assertEqual(0, self.run_check().returncode)

    def test_directive_boundaries_require_core_test_delta(self):
        before = "int f(void) {\nint x=0;\n#define UNUSED 1\nx=1;\nreturn x;\n}\n#define LAST 2\n"
        self.write("core/a.c", before)
        self.commit()
        for after in [before.replace("UNUSED 1\nx=1;", "UNUSED 1 x=1;"),
                      before.replace("UNUSED 1\nx=1;", "UNUSED 1 \\\nx=1;")]:
            with self.subTest(after=after):
                self.write("core/a.c", after)
                result = self.run_check("HEAD")
                self.assertEqual(1, result.returncode, result.stderr)
                self.assertIn("core test gate", result.stderr)
        for after in [before.replace("UNUSED 1", "UNUSED   1 /* explanation */"),
                      before.replace("UNUSED 1", "UNUSED \\\n1"),
                      before.replace("\n", "\r\n"), before.rstrip()]:
            with self.subTest(after=after):
                self.write("core/a.c", after)
                self.assertEqual(0, self.run_check("HEAD").returncode)
        self.write("core/a.c", before.replace("UNUSED 1\nx=1;", "UNUSED 1 x=1;"))
        self.write("tests/core_a_tests.c", "void test(void) { CHECK_INT(f(), 0); }\n")
        self.assertEqual(0, self.run_check("HEAD").returncode)

    def test_literal_pragma_operators_count_suppressions(self):
        suppression = r'_Pragma("GCC diagnostic ignored \"-Wunused-variable\"")'
        for source in [suppression, suppression.replace("GCC", "clang"),
                       suppression.replace('_Pragma("', '_Pragma(L"'),
                       "#define QUIET " + suppression,
                       '_Pragma("warning(disable: 4100)")']:
            with self.subTest(source=source):
                self.write("app/helper.c", source + "\n")
                result = self.run_check()
                self.assertEqual(1, result.returncode, result.stderr)
                self.assertIn("warning suppressions increased", result.stderr)
        examples = ('// ' + suppression + '\n/* ' + suppression + ' */\n'
                    'const char *s = "_Pragma(\\\"GCC diagnostic ignored\\\")";\n'
                    'const char *t = "GCC diagnostic ignored";\n'
                    '_Pragma("GCC diagnostic push")\n_Pragma("GCC diagnostic pop")\n')
        self.write("app/helper.c", examples)
        self.assertEqual(0, self.run_check().returncode)
        self.write("app/helper.c", suppression + "\n")
        self.commit()
        self.write("app/helper.c", suppression + "\n" + suppression + "\n")
        result = self.run_check("HEAD")
        self.assertEqual(1, result.returncode, result.stderr)
        self.write("app/helper.c", "")
        self.assertEqual(0, self.run_check("HEAD").returncode)

    def test_continued_diagnostic_pragmas_count_suppressions(self):
        for newline in ["\n", "\r\n"]:
            for compiler in ["GCC", "clang"]:
                with self.subTest(newline=newline, compiler=compiler):
                    source = f'#pragma {compiler} \\' + newline + 'diagnostic ignored "-Wshadow"\n'
                    self.write("app/helper.c", source)
                    result = self.run_check()
                    self.assertEqual(1, result.returncode, result.stderr)
                    self.assertIn("warning suppressions increased", result.stderr)
                    examples = ('/* ' + source + ' */\n// #pragma ' + compiler + ' \\' + newline
                                + 'diagnostic ignored "-Wshadow"\n'
                                + 'const char *s = "\\' + newline
                                + '#pragma ' + compiler + ' diagnostic ignored \\\"-Wshadow\\\"";\n')
                    self.write("app/helper.c", examples)
                    self.assertEqual(0, self.run_check().returncode)

    def test_comment_prefixed_directives_are_checked(self):
        self.write("tests/core_a_tests.c", "void test(void) { CHECK_INT(value, 2); }\n")
        for prefix in ["/* explained */ ", "\t/* first */ /* second */ ", "/* # multi\nline */ "]:
            with self.subTest(prefix=prefix):
                self.write("core/a.c", prefix + '#include "../app/store.h"\n')
                result = self.run_check()
                self.assertEqual(1, result.returncode, result.stderr)
                line = prefix.count("\n") + 1
                self.assertIn(f"core/a.c:{line}: core includes UI header ../app/store.h", result.stderr)
                self.assertNotIn("core test gate", result.stderr)
        self.write("app/helper.c", '/* explained */ #pragma GCC diagnostic ignored "-Wshadow"\n')
        self.write("core/a.c", "int value = 1;\n")
        result = self.run_check()
        self.assertEqual(1, result.returncode, result.stderr)
        self.assertIn("warning suppressions increased", result.stderr)
        self.write("app/helper.c", "/* explanation */ #define GUARD if (a && b && c && d && e && f) g();\n")
        self.assertEqual(0, self.run_check().returncode)

    def test_parenthesized_callees_use_call_rules(self):
        for callee, diagnostic, adapter in [("api_call", "bypasses store", "app/store.c"),
                                             ("WinHttpOpen", "WinHTTP belongs", "app/ws.c")]:
            for expression in [f"({callee})(c)", f"(({callee}))(c)", f"(*{callee})(c)"]:
                with self.subTest(expression=expression):
                    self.write("app/helper.c", expression + ";\n")
                    result = self.run_check()
                    self.assertEqual(1, result.returncode, result.stderr)
                    self.assertIn(diagnostic, result.stderr)
                    self.write("app/helper.c", "")
                    self.write(adapter, expression + ";\n")
                    self.assertEqual(0, self.run_check().returncode)
                    self.write(adapter, "")
        for callee in ["(callback)", "(*callback)", "((callback))", "(*((callback)))"]:
            with self.subTest(callee=callee):
                self.write("app/helper.c", f"if ({callee}(a && b && c && d && e && f)) g();\n")
                self.assertEqual(0, self.run_check().returncode)
        self.write("app/helper.c", "if ((a && b && c) && (d && e && f)) g();\n")
        result = self.run_check()
        self.assertEqual(1, result.returncode, result.stderr)
        self.assertIn("6 logical operands", result.stderr)

    def test_continued_includes_fail_with_original_line_numbers(self):
        self.write("tests/core_a_tests.c", "void test(void) { CHECK_INT(value, 2); }\n")
        for prefix, newline in [("", "\n"), ("", "\r\n"), ("    ", "\n"), ("\t", "\n")]:
            with self.subTest(prefix=prefix, newline=newline):
                self.write("core/a.c", "/* heading */\n\n" + prefix + "#include \\" + newline + '"../app/store.h"\n')
                result = self.run_check()
                self.assertEqual(1, result.returncode, result.stderr)
                self.assertIn("core/a.c:3: core includes UI header ../app/store.h", result.stderr)
                self.assertNotIn("core test gate", result.stderr)

    def test_include_text_in_continued_strings_passes(self):
        self.write("core/a.c", 'const char *s = "\\\n#include <commctrl.h>";\n')
        self.write("tests/core_a_tests.c", "void test(void) { CHECK_INT(value, 2); }\n")
        result = self.run_check()
        self.assertEqual(0, result.returncode, result.stderr)

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
