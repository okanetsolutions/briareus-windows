#include "markdown.h"
#include "str.h"
#include "suites.h"
#include "test.h"
#include <stdlib.h>
#include <string.h>

#define DOT "\xE2\x80\xA2"

// Blocks written out as one string: <kind attrs>text for each, so a whole parse reads as one expectation.
static char *blocks_desc(const char *source) {
    size_t n; MdBlock *b = md_parse(source, &n);
    Str s; str_init(&s);
    for (size_t i = 0; i < n; i++) {
        const MdBlock *k = &b[i];
        switch (k->kind) {
        case MD_PARAGRAPH: str_appendz(&s, "<p>"); break;
        case MD_HEADING: str_appendf(&s, "<h%d>", k->level); break;
        case MD_BULLET: str_appendf(&s, "<li%d %s%s>", k->indent, k->marker, k->task == 1 ? " [ ]" : k->task == 2 ? " [x]" : ""); break;
        case MD_QUOTE: str_appendz(&s, "<quote>"); break;
        case MD_CODE: str_appendf(&s, "<code%s%s>", k->language ? " " : "", k->language ? k->language : ""); break;
        case MD_RULE: str_appendz(&s, "<hr>"); break;
        case MD_TABLE:
            str_appendf(&s, "<table %s %zux%zu>", k->aligns, k->rows, k->cols);
            for (size_t c = 0; c < k->rows * k->cols; c++) { if (c) str_appendc(&s, '|'); str_appendz(&s, k->cells[c]); }
            break;
        }
        if (k->text) str_appendz(&s, k->text);
    }
    md_free(b, n);
    return str_detach(&s);
}

// Spans written out as [flags:text@url], flags as B I C L S.
static char *spans_desc(const char *text) {
    size_t n; MdSpan *spans = md_inline(text, &n);
    Str s; str_init(&s);
    for (size_t i = 0; i < n; i++) {
        unsigned f = spans[i].flags;
        str_appendc(&s, '[');
        if (f & SPAN_BOLD) str_appendc(&s, 'B');
        if (f & SPAN_ITALIC) str_appendc(&s, 'I');
        if (f & SPAN_CODE) str_appendc(&s, 'C');
        if (f & SPAN_LINK) str_appendc(&s, 'L');
        if (f & SPAN_STRIKE) str_appendc(&s, 'S');
        str_appendc(&s, ':'); str_appendz(&s, spans[i].text);
        if (spans[i].url) { str_appendc(&s, '@'); str_appendz(&s, spans[i].url); }
        str_appendc(&s, ']');
    }
    md_spans_free(spans, n);
    return str_detach(&s);
}

// MARK: - Blocks

static void test_empty_and_null_sources_have_no_blocks(void) {
    size_t n = 99; MdBlock *b = md_parse(NULL, &n); CHECK_INT(n, 0); md_free(b, n);
    n = 99; b = md_parse("", &n); CHECK_INT(n, 0); md_free(b, n);
    n = 99; b = md_parse("\n\n  \n\t\n", &n); CHECK_INT(n, 0); md_free(b, n);
    md_free(NULL, 0);
}

static void test_headings_take_one_to_six_hashes_and_a_space(void) {
    CHECK_OWNED_STR(blocks_desc("# One\n## Two\n### Three\n#### Four\n##### Five\n###### Six"), "<h1>One<h2>Two<h3>Three<h4>Four<h5>Five<h6>Six");
    CHECK_OWNED_STR(blocks_desc("####### Seven"), "<p>####### Seven");
    CHECK_OWNED_STR(blocks_desc("#nospace"), "<p>#nospace");
    CHECK_OWNED_STR(blocks_desc("#\ttab"), "<p>#\ttab");
    CHECK_OWNED_STR(blocks_desc("#"), "<p>#");
    CHECK_OWNED_STR(blocks_desc("#   Spaced   out   "), "<h1>Spaced   out");
    CHECK_OWNED_STR(blocks_desc("   ## Indented"), "<h2>Indented");
    CHECK_OWNED_STR(blocks_desc("## Keeps **bold** markers"), "<h2>Keeps **bold** markers");
}

static void test_headings_and_rules_end_a_paragraph(void) {
    CHECK_OWNED_STR(blocks_desc("intro\n# Title\nbody"), "<p>intro<h1>Title<p>body");
    CHECK_OWNED_STR(blocks_desc("above\n---\nbelow"), "<p>above<hr><p>below");
}

static void test_paragraph_lines_join_and_blank_lines_split_them(void) {
    CHECK_OWNED_STR(blocks_desc("one\ntwo\nthree"), "<p>one\ntwo\nthree");
    CHECK_OWNED_STR(blocks_desc("one\n\ntwo"), "<p>one<p>two");
    CHECK_OWNED_STR(blocks_desc("one\n   \n\t\ntwo"), "<p>one<p>two");
    CHECK_OWNED_STR(blocks_desc("\n\none\n\n\n"), "<p>one");
    // Paragraph lines are kept as written, indentation included.
    CHECK_OWNED_STR(blocks_desc("  indented\nflush"), "<p>  indented\nflush");
}

static void test_crlf_sources_parse_like_lf(void) {
    CHECK_OWNED_STR(blocks_desc("# Title\r\none\r\ntwo\r\n\r\n- item\r\n```c\r\nx\r\n```\r\n"), "<h1>Title<p>one\ntwo<li0 " DOT ">item<code c>x");
}

static void test_rules_need_three_of_the_same_mark(void) {
    CHECK_OWNED_STR(blocks_desc("---"), "<hr>");
    CHECK_OWNED_STR(blocks_desc("***"), "<hr>");
    CHECK_OWNED_STR(blocks_desc("___"), "<hr>");
    CHECK_OWNED_STR(blocks_desc("- - -"), "<hr>");
    CHECK_OWNED_STR(blocks_desc("* * *"), "<hr>");
    CHECK_OWNED_STR(blocks_desc("----------"), "<hr>");
    CHECK_OWNED_STR(blocks_desc("  ---  "), "<hr>");
    CHECK_OWNED_STR(blocks_desc("--"), "<p>--");
    CHECK_OWNED_STR(blocks_desc("-*-"), "<p>-*-");
    CHECK_OWNED_STR(blocks_desc("--- x"), "<p>--- x");
    CHECK_OWNED_STR(blocks_desc("==="), "<p>===");
}

static void test_bullets_take_dash_star_or_plus_and_nest_by_two_spaces(void) {
    CHECK_OWNED_STR(blocks_desc("- dash\n* star\n+ plus"), "<li0 " DOT ">dash<li0 " DOT ">star<li0 " DOT ">plus");
    CHECK_OWNED_STR(blocks_desc("- a\n  - b\n    - c\n   - d\n\t- e\n\t\t- f"),
                    "<li0 " DOT ">a<li1 " DOT ">b<li2 " DOT ">c<li1 " DOT ">d<li0 " DOT ">e<li1 " DOT ">f");
    CHECK_OWNED_STR(blocks_desc("-nospace"), "<p>-nospace");
    CHECK_OWNED_STR(blocks_desc("- "), "<li0 " DOT ">");
    CHECK_OWNED_STR(blocks_desc("-  two spaces"), "<li0 " DOT "> two spaces");
}

static void test_ordered_markers_keep_their_number_with_a_dot(void) {
    CHECK_OWNED_STR(blocks_desc("1. one\n2) two\n10. ten\n999. big"), "<li0 1.>one<li0 2.>two<li0 10.>ten<li0 999.>big");
    CHECK_OWNED_STR(blocks_desc("  3. nested"), "<li1 3.>nested");
    CHECK_OWNED_STR(blocks_desc("1000. too long"), "<p>1000. too long");
    CHECK_OWNED_STR(blocks_desc("1.no space"), "<p>1.no space");
    CHECK_OWNED_STR(blocks_desc("1:x"), "<p>1:x");
    CHECK_OWNED_STR(blocks_desc("v1. not a number"), "<p>v1. not a number");
}

static void test_a_paragraph_ends_where_a_list_starts(void) {
    CHECK_OWNED_STR(blocks_desc("Steps:\n1. one\n2. two\nafter"), "<p>Steps:<li0 1.>one<li0 2.>two<p>after");
}

static void test_indented_lines_continue_the_list_item_above(void) {
    CHECK_OWNED_STR(blocks_desc("- one\n  more\n   and more\nflush"), "<li0 " DOT ">one\nmore\nand more<p>flush");
    // An indented line after a blank line still belongs to the item.
    CHECK_OWNED_STR(blocks_desc("- one\n\n  later"), "<li0 " DOT ">one\nlater");
    CHECK_OWNED_STR(blocks_desc("- one\n\tafter tab"), "<li0 " DOT ">one<p>\tafter tab");
}

static void test_task_items_carry_their_box(void) {
    CHECK_OWNED_STR(blocks_desc("- [ ] open\n- [x] done\n- [X] Done\n+ [ ]\n- [x]\n1. [x] numbered"),
                    "<li0 " DOT " [ ]>open<li0 " DOT " [x]>done<li0 " DOT " [x]>Done<li0 " DOT " [ ]><li0 " DOT " [x]><li0 1. [x]>numbered");
    CHECK_OWNED_STR(blocks_desc("- [ ]tight"), "<li0 " DOT ">[ ]tight");
    CHECK_OWNED_STR(blocks_desc("- [y] other"), "<li0 " DOT ">[y] other");
    CHECK_OWNED_STR(blocks_desc("- [] empty"), "<li0 " DOT ">[] empty");
    CHECK_OWNED_STR(blocks_desc("  - [ ] nested"), "<li1 " DOT " [ ]>nested");
    CHECK_OWNED_STR(blocks_desc("[ ] not a bullet"), "<p>[ ] not a bullet");
}

static void test_quotes_gather_lines_without_their_marker(void) {
    CHECK_OWNED_STR(blocks_desc("> one\n>two\n>   three  "), "<quote>one\ntwo\nthree");
    CHECK_OWNED_STR(blocks_desc("> a\n>\n> b"), "<quote>a\n\nb");
    CHECK_OWNED_STR(blocks_desc("> > nested"), "<quote>> nested");
    CHECK_OWNED_STR(blocks_desc("  > indented"), "<quote>indented");
    CHECK_OWNED_STR(blocks_desc("> # not a heading"), "<quote># not a heading");
}

static void test_quotes_end_at_blank_lines_and_other_blocks(void) {
    CHECK_OWNED_STR(blocks_desc("> q\n\n> r"), "<quote>q<quote>r");
    CHECK_OWNED_STR(blocks_desc("> q\nplain"), "<quote>q<p>plain");
    CHECK_OWNED_STR(blocks_desc("para\n> q"), "<p>para<quote>q");
    CHECK_OWNED_STR(blocks_desc("> q\n# H"), "<quote>q<h1>H");
    CHECK_OWNED_STR(blocks_desc("> q\n- item"), "<quote>q<li0 " DOT ">item");
    CHECK_OWNED_STR(blocks_desc("> q\n```\nc\n```"), "<quote>q<code>c");
    CHECK_OWNED_STR(blocks_desc("> q\n---"), "<quote>q<hr>");
}

static void test_fences_keep_their_lines_verbatim(void) {
    CHECK_OWNED_STR(blocks_desc("```\n  indented\n# not heading\n- not bullet\n\n> not quote\n```"), "<code>  indented\n# not heading\n- not bullet\n\n> not quote");
    CHECK_OWNED_STR(blocks_desc("```python  \nx = 1\n```"), "<code python>x = 1");
    CHECK_OWNED_STR(blocks_desc("```   js\nx\n```"), "<code js>x");
    CHECK_OWNED_STR(blocks_desc("~~~sh\nls\n~~~"), "<code sh>ls");
    CHECK_OWNED_STR(blocks_desc("```\n```"), "<code>");
    CHECK_OWNED_STR(blocks_desc("```\n\n```"), "<code>");
    CHECK_OWNED_STR(blocks_desc("  ```\nx\n  ```  "), "<code>x");
    CHECK_OWNED_STR(blocks_desc("```\ncode\n```\nafter"), "<code>code<p>after");
    CHECK_OWNED_STR(blocks_desc("before\n```\ncode\n```"), "<p>before<code>code");
}

static void test_a_fence_closes_only_on_its_own_mark_at_least_as_long(void) {
    CHECK_OWNED_STR(blocks_desc("````\n```\n````"), "<code>```");
    CHECK_OWNED_STR(blocks_desc("```\n~~~\n```"), "<code>~~~");
    CHECK_OWNED_STR(blocks_desc("~~~\n```\n~~~"), "<code>```");
    CHECK_OWNED_STR(blocks_desc("```\nx\n``````"), "<code>x");
    CHECK_OWNED_STR(blocks_desc("```\n``` not closing\n```"), "<code>``` not closing");
}

static void test_an_unterminated_fence_runs_to_the_end(void) {
    CHECK_OWNED_STR(blocks_desc("```go\nfunc a() {}\n\n# still code"), "<code go>func a() {}\n\n# still code");
    CHECK_OWNED_STR(blocks_desc("text\n```"), "<p>text<code>");
    CHECK_OWNED_STR(blocks_desc("````\n```"), "<code>```");
}

static void test_tables_read_alignments_from_the_delimiter_row(void) {
    CHECK_OWNED_STR(blocks_desc("| a | b | c | d |\n| --- | :--- | :---: | ---: |\n| 1 | 2 | 3 | 4 |"), "<table llcr 2x4>a|b|c|d|1|2|3|4");
    CHECK_OWNED_STR(blocks_desc("|a|b|\n|-|:-:|"), "<table lc 1x2>a|b");
    CHECK_OWNED_STR(blocks_desc("a | b\n--|--\n1 | 2"), "<table ll 2x2>a|b|1|2");
    CHECK_OWNED_STR(blocks_desc("| a |\n|---|\n| 1 |"), "<table l 2x1>a|1");
}

static void test_table_rows_are_padded_or_cut_to_the_header(void) {
    CHECK_OWNED_STR(blocks_desc("| a | b | c |\n|---|---|---|\n| 1 |\n| 1 | 2 | 3 | 4 | 5 |\n| | x | |"), "<table lll 4x3>a|b|c|1|||1|2|3||x|");
}

static void test_table_cells_unescape_pipes(void) {
    size_t n; MdBlock *b = md_parse("| code | note |\n|---|---|\n| `a \\| b` | trailing \\|\n", &n);
    CHECK_INT(n, 1);
    if (n == 1) {
        CHECK_INT(b[0].rows, 2); CHECK_INT(b[0].cols, 2);
        CHECK_STR(b[0].cells[2], "`a | b`");
        CHECK_STR(b[0].cells[3], "trailing |");
    }
    md_free(b, n);
}

static void test_tables_end_at_a_blank_line_or_a_line_without_a_pipe(void) {
    CHECK_OWNED_STR(blocks_desc("| a |\n|---|\n| 1 |\n\n| 2 |"), "<table l 2x1>a|1<p>| 2 |");
    CHECK_OWNED_STR(blocks_desc("| a |\n|---|\n| 1 |\nafter"), "<table l 2x1>a|1<p>after");
    CHECK_OWNED_STR(blocks_desc("| a |\n|---|"), "<table l 1x1>a");
    CHECK_OWNED_STR(blocks_desc("intro\n| a |\n|---|\n| 1 |"), "<p>intro<table l 2x1>a|1");
    CHECK_OWNED_STR(blocks_desc("> q\n| a |\n|---|"), "<quote>q<table l 1x1>a");
    CHECK_OWNED_STR(blocks_desc("| a | b |\n|---|---|\n| 1 | 2 |\n| 3 | 4 |\n# Next"), "<table ll 3x2>a|b|1|2|3|4<h1>Next");
}

static void test_pipes_without_a_matching_delimiter_row_are_text(void) {
    CHECK_OWNED_STR(blocks_desc("| a | b |"), "<p>| a | b |");
    CHECK_OWNED_STR(blocks_desc("| a | b |\n|---|\n"), "<p>| a | b |\n|---|");
    CHECK_OWNED_STR(blocks_desc("| a | b |\n| --- | x |"), "<p>| a | b |\n| --- | x |");
    CHECK_OWNED_STR(blocks_desc("| a |\n|:|"), "<p>| a |\n|:|");
    CHECK_OWNED_STR(blocks_desc("a | b\n---"), "<p>a | b<hr>");
    CHECK_OWNED_STR(blocks_desc("| a | b |\n\n|---|---|"), "<p>| a | b |<p>|---|---|");
}

static void test_a_full_reply_mixes_every_block(void) {
    const char *reply =
        "# Summary\n"
        "I changed **two** files:\n"
        "\n"
        "1. `core/a.c`\n"
        "   - [x] parse\n"
        "   - [ ] free\n"
        "2. `core/b.c`\n"
        "\n"
        "> Note: run the tests\n"
        "\n"
        "| File | Lines |\n"
        "|:-----|------:|\n"
        "| a.c | 12 |\n"
        "\n"
        "```diff\n"
        "-old\n"
        "+new\n"
        "```\n"
        "***\n"
        "Done.";
    CHECK_OWNED_STR(blocks_desc(reply),
                    "<h1>Summary<p>I changed **two** files:<li0 1.>`core/a.c`<li1 " DOT " [x]>parse<li1 " DOT " [ ]>free<li0 2.>`core/b.c`"
                    "<quote>Note: run the tests<table lr 2x2>File|Lines|a.c|12<code diff>-old\n+new<hr><p>Done.");
}

// MARK: - Inline

static void test_plain_text_is_one_span_without_a_url(void) {
    size_t n; MdSpan *s = md_inline("just words", &n);
    CHECK_INT(n, 1);
    if (n == 1) { CHECK_INT(s[0].flags, 0); CHECK_STR(s[0].text, "just words"); CHECK(s[0].url == NULL); }
    md_spans_free(s, n);
    n = 99; s = md_inline("", &n); CHECK_INT(n, 0); md_spans_free(s, n);
    n = 99; s = md_inline(NULL, &n); CHECK_INT(n, 0); md_spans_free(s, n);
    md_spans_free(NULL, 0);
}

static void test_newlines_stay_in_the_text(void) {
    CHECK_OWNED_STR(spans_desc("a\nb\n**c\nd**"), "[:a\nb\n][B:c\nd]");
}

static void test_code_spans_take_matching_backtick_runs(void) {
    CHECK_OWNED_STR(spans_desc("`x`"), "[C:x]");
    CHECK_OWNED_STR(spans_desc("``a ` b``"), "[C:a ` b]");
    CHECK_OWNED_STR(spans_desc("`` `ticks` ``"), "[C:`ticks`]");
    CHECK_OWNED_STR(spans_desc("` x`"), "[C: x]");
    CHECK_OWNED_STR(spans_desc("`**not bold** &amp; [no](https://x.y)`"), "[C:**not bold** &amp; [no](https://x.y)]");
    CHECK_OWNED_STR(spans_desc("a `b` c `d`"), "[:a ][C:b][: c ][C:d]");
}

static void test_unclosed_backticks_are_literal(void) {
    CHECK_OWNED_STR(spans_desc("`open"), "[:`open]");
    CHECK_OWNED_STR(spans_desc("``a`"), "[:``a`]");
    CHECK_OWNED_STR(spans_desc("a ``` b"), "[:a ``` b]");
    CHECK_OWNED_STR(spans_desc("``"), "[:``]");
}

static void test_bold_italic_and_strike_markers(void) {
    CHECK_OWNED_STR(spans_desc("**b** __b__ *i* _i_ ~~s~~"), "[B:b][: ][B:b][: ][I:i][: ][I:i][: ][S:s]");
    CHECK_OWNED_STR(spans_desc("**bold *it* x**"), "[B:bold ][BI:it][B: x]");
    CHECK_OWNED_STR(spans_desc("~~**gone**~~"), "[BS:gone]");
    CHECK_OWNED_STR(spans_desc("_**both**_"), "[BI:both]");
}

static void test_tripled_markers_are_bold_and_italic(void) {
    CHECK_OWNED_STR(spans_desc("***bi***"), "[BI:bi]");
    CHECK_OWNED_STR(spans_desc("a ___bi___ b"), "[:a ][BI:bi][: b]");
    CHECK_OWNED_STR(spans_desc("a___b___c"), "[:a___b___c]");
    CHECK_OWNED_STR(spans_desc("***open"), "[:***open]");
}

static void test_italics_step_over_bold_inside_them(void) {
    CHECK_OWNED_STR(spans_desc("*a **b** c*"), "[I:a ][BI:b][I: c]");
    CHECK_OWNED_STR(spans_desc("_a __b__ c_"), "[I:a ][BI:b][I: c]");
    CHECK_OWNED_STR(spans_desc("*a **b***"), "[I:a ][BI:b]");
    // A doubled marker that closes nothing is still the italic's end.
    CHECK_OWNED_STR(spans_desc("*a**"), "[I:a][:*]");
}

static void test_emphasis_needs_text_hugging_its_markers(void) {
    CHECK_OWNED_STR(spans_desc("2 * 3 * 4"), "[:2 * 3 * 4]");
    CHECK_OWNED_STR(spans_desc("* not*"), "[:* not*]");
    CHECK_OWNED_STR(spans_desc("*not *"), "[:*not *]");
    CHECK_OWNED_STR(spans_desc("** no**"), "[:** no**]");
    CHECK_OWNED_STR(spans_desc("~~ no~~"), "[:~~ no~~]");
    CHECK_OWNED_STR(spans_desc("~one~"), "[:~one~]");
    CHECK_OWNED_STR(spans_desc("**"), "[:**]");
    CHECK_OWNED_STR(spans_desc("*"), "[:*]");
    CHECK_OWNED_STR(spans_desc("~~~~"), "[:~~~~]");
}

static void test_unclosed_emphasis_is_literal(void) {
    CHECK_OWNED_STR(spans_desc("**open"), "[:**open]");
    CHECK_OWNED_STR(spans_desc("*open"), "[:*open]");
    CHECK_OWNED_STR(spans_desc("~~open"), "[:~~open]");
    CHECK_OWNED_STR(spans_desc("__open"), "[:__open]");
}

static void test_intraword_underscores_are_literal(void) {
    CHECK_OWNED_STR(spans_desc("snake_case_name"), "[:snake_case_name]");
    CHECK_OWNED_STR(spans_desc("a__b__c"), "[:a__b__c]");
    CHECK_OWNED_STR(spans_desc("x_1 and _real_"), "[:x_1 and ][I:real]");
    // Asterisks still work inside words.
    CHECK_OWNED_STR(spans_desc("un*frigging*believable"), "[:un][I:frigging][:believable]");
}

static void test_backslash_escapes_drop_the_backslash(void) {
    CHECK_OWNED_STR(spans_desc("\\*not\\*"), "[:*not*]");
    CHECK_OWNED_STR(spans_desc("\\- item \\+ item 1\\. item 2\\) item"), "[:- item + item 1. item 2) item]");
    CHECK_OWNED_STR(spans_desc("\\`x\\` \\[a\\](b) \\_ \\# \\~ \\! \\| \\< \\> \\( \\)"), "[:`x` [a](b) _ # ~ ! | < > ( )]");
    CHECK_OWNED_STR(spans_desc("\\\\"), "[:\\]");
    CHECK_OWNED_STR(spans_desc("\\a \\n"), "[:\\a \\n]");
    CHECK_OWNED_STR(spans_desc("end\\"), "[:end\\]");
    CHECK_OWNED_STR(spans_desc("*a\\*b*"), "[I:a*b]");
    CHECK_OWNED_STR(spans_desc("**a\\**b**"), "[B:a**b]");
}

static void test_links_carry_their_url_on_every_label_span(void) {
    CHECK_OWNED_STR(spans_desc("[text](https://a.example/x)"), "[L:text@https://a.example/x]");
    CHECK_OWNED_STR(spans_desc("[rel](docs/readme.md)"), "[L:rel@docs/readme.md]");
    CHECK_OWNED_STR(spans_desc("[a **b** c](u)"), "[L:a @u][BL:b@u][L: c@u]");
    CHECK_OWNED_STR(spans_desc("[`code`](u)"), "[CL:code@u]");
    CHECK_OWNED_STR(spans_desc("**[bold link](u)**"), "[BL:bold link@u]");
    CHECK_OWNED_STR(spans_desc("[t](https://a.example \"Title\")"), "[L:t@https://a.example]");
    CHECK_OWNED_STR(spans_desc("[a [b] c](u)"), "[L:a [b] c@u]");
    CHECK_OWNED_STR(spans_desc("go [here](u) now"), "[:go ][L:here@u][: now]");
}

static void test_link_urls_keep_balanced_parentheses(void) {
    CHECK_OWNED_STR(spans_desc("[w](https://en.wikipedia.org/wiki/A_(b))"), "[L:w@https://en.wikipedia.org/wiki/A_(b)]");
    CHECK_OWNED_STR(spans_desc("([w](u)) x"), "[:(][L:w@u][:) x]");
    CHECK_OWNED_STR(spans_desc("[w](u(v)"), "[:[w](u(v)]");
}

static void test_links_with_an_empty_label_show_their_url(void) {
    CHECK_OWNED_STR(spans_desc("see [](https://a.example) now"), "[:see ][L:https://a.example@https://a.example][: now]");
    CHECK_OWNED_STR(spans_desc("[]()"), "");
}

static void test_adjacent_links_stay_separate_spans(void) {
    CHECK_OWNED_STR(spans_desc("[a](u)[b](u)"), "[L:a@u][L:b@u]");
    CHECK_OWNED_STR(spans_desc("[a](u) [b](v)"), "[L:a@u][: ][L:b@v]");
}

static void test_brackets_that_are_not_links_are_literal(void) {
    CHECK_OWNED_STR(spans_desc("[just brackets]"), "[:[just brackets]]");
    CHECK_OWNED_STR(spans_desc("[text] (u)"), "[:[text] (u)]");
    CHECK_OWNED_STR(spans_desc("[text](unclosed"), "[:[text](unclosed]");
    CHECK_OWNED_STR(spans_desc("[open"), "[:[open]");
    CHECK_OWNED_STR(spans_desc("a]b(c)"), "[:a]b(c)]");
    CHECK_OWNED_STR(spans_desc("arr[0](x)"), "[:arr][L:0@x]");
}

static void test_images_show_their_alt_text_as_a_link(void) {
    CHECK_OWNED_STR(spans_desc("![diagram](https://x.example/d.png)"), "[L:diagram@https://x.example/d.png]");
    CHECK_OWNED_STR(spans_desc("Wow! [x](u)"), "[:Wow! ][L:x@u]");
    CHECK_OWNED_STR(spans_desc("!["), "[:[]");
    CHECK_OWNED_STR(spans_desc("!"), "[:!]");
}

static void test_angle_autolinks(void) {
    CHECK_OWNED_STR(spans_desc("<https://a.example/p?q=1>"), "[L:https://a.example/p?q=1@https://a.example/p?q=1]");
    CHECK_OWNED_STR(spans_desc("see <http://x.y>."), "[:see ][L:http://x.y@http://x.y][:.]");
    CHECK_OWNED_STR(spans_desc("<notaurl>"), "[:<notaurl>]");
    CHECK_OWNED_STR(spans_desc("<ftp://x.y>"), "[:<ftp://x.y>]");
    CHECK_OWNED_STR(spans_desc("<https://open"), "[:<][L:https://open@https://open]");
}

static void test_bare_urls_drop_trailing_punctuation(void) {
    CHECK_OWNED_STR(spans_desc("https://a.example/x"), "[L:https://a.example/x@https://a.example/x]");
    CHECK_OWNED_STR(spans_desc("See https://a.example/x."), "[:See ][L:https://a.example/x@https://a.example/x][:.]");
    CHECK_OWNED_STR(spans_desc("http://a.b/c?!,;:"), "[L:http://a.b/c@http://a.b/c][:?!,;:]");
    CHECK_OWNED_STR(spans_desc("(https://a.b/c)"), "[:(][L:https://a.b/c@https://a.b/c][:)]");
    CHECK_OWNED_STR(spans_desc("\"https://a.b\""), "[:\"][L:https://a.b@https://a.b][:\"]");
    CHECK_OWNED_STR(spans_desc("https://a.b/c d"), "[L:https://a.b/c@https://a.b/c][: d]");
    CHECK_OWNED_STR(spans_desc("https://a.b/q?x=1&y=2#frag"), "[L:https://a.b/q?x=1&y=2#frag@https://a.b/q?x=1&y=2#frag]");
    CHECK_OWNED_STR(spans_desc("**https://a.b**"), "[BL:https://a.b@https://a.b]");
}

static void test_url_schemes_inside_words_are_not_links(void) {
    CHECK_OWNED_STR(spans_desc("xhttps://a.b"), "[:xhttps://a.b]");
    CHECK_OWNED_STR(spans_desc("ftp://a.b"), "[:ftp://a.b]");
    CHECK_OWNED_STR(spans_desc("https:/a.b"), "[:https:/a.b]");
    CHECK_OWNED_STR(spans_desc("-https://a.b"), "[:-][L:https://a.b@https://a.b]");
}

static void test_html_entities_are_decoded(void) {
    CHECK_OWNED_STR(spans_desc("&amp; &lt; &gt; &quot; &#39; &apos; &nbsp;"), "[:& < > \" ' ' \xC2\xA0]");
    CHECK_OWNED_STR(spans_desc("&unknown; & &amp &#40;"), "[:&unknown; & &amp &#40;]");
    CHECK_OWNED_STR(spans_desc("&amp;lt;"), "[:&lt;]");
    CHECK_OWNED_STR(spans_desc("**&lt;tag&gt;**"), "[B:<tag>]");
    CHECK_OWNED_STR(spans_desc("&"), "[:&]");
}

static void test_spans_with_the_same_style_merge(void) {
    size_t n; MdSpan *s = md_inline("a &amp; b \\* c [x", &n);
    CHECK_INT(n, 1);
    if (n == 1) CHECK_STR(s[0].text, "a & b * c [x");
    md_spans_free(s, n);
    CHECK_OWNED_STR(spans_desc("**a****b**"), "[B:ab]");
    CHECK_OWNED_STR(spans_desc("*a**b*"), "[I:ab]");
    CHECK_OWNED_STR(spans_desc("**a** **b**"), "[B:a][: ][B:b]");
}

static void test_md_plain_strips_every_marker(void) {
    CHECK_OWNED_STR(md_plain("# not a block"), "# not a block");
    CHECK_OWNED_STR(md_plain("**b** *i* ~~s~~ `c` [l](u) ![img](u) <https://x.y> &amp;"), "b i s c l img https://x.y &");
    CHECK_OWNED_STR(md_plain("snake_case \\*lit\\*"), "snake_case *lit*");
    CHECK_OWNED_STR(md_plain("line\nbreak"), "line\nbreak");
    CHECK_OWNED_STR(md_plain(""), "");
    CHECK_OWNED_STR(md_plain(NULL), "");
}

void markdown_tests(void) {
    test_run("empty and null sources have no blocks", test_empty_and_null_sources_have_no_blocks);
    test_run("headings take one to six hashes and a space", test_headings_take_one_to_six_hashes_and_a_space);
    test_run("headings and rules end a paragraph", test_headings_and_rules_end_a_paragraph);
    test_run("paragraph lines join and blank lines split them", test_paragraph_lines_join_and_blank_lines_split_them);
    test_run("CRLF sources parse like LF", test_crlf_sources_parse_like_lf);
    test_run("rules need three of the same mark", test_rules_need_three_of_the_same_mark);
    test_run("bullets take dash, star or plus and nest by two spaces", test_bullets_take_dash_star_or_plus_and_nest_by_two_spaces);
    test_run("ordered markers keep their number with a dot", test_ordered_markers_keep_their_number_with_a_dot);
    test_run("a paragraph ends where a list starts", test_a_paragraph_ends_where_a_list_starts);
    test_run("indented lines continue the list item above", test_indented_lines_continue_the_list_item_above);
    test_run("task items carry their box", test_task_items_carry_their_box);
    test_run("quotes gather lines without their marker", test_quotes_gather_lines_without_their_marker);
    test_run("quotes end at blank lines and other blocks", test_quotes_end_at_blank_lines_and_other_blocks);
    test_run("fences keep their lines verbatim", test_fences_keep_their_lines_verbatim);
    test_run("a fence closes only on its own mark at least as long", test_a_fence_closes_only_on_its_own_mark_at_least_as_long);
    test_run("an unterminated fence runs to the end", test_an_unterminated_fence_runs_to_the_end);
    test_run("tables read alignments from the delimiter row", test_tables_read_alignments_from_the_delimiter_row);
    test_run("table rows are padded or cut to the header", test_table_rows_are_padded_or_cut_to_the_header);
    test_run("table cells unescape pipes", test_table_cells_unescape_pipes);
    test_run("tables end at a blank line or a line without a pipe", test_tables_end_at_a_blank_line_or_a_line_without_a_pipe);
    test_run("pipes without a matching delimiter row are text", test_pipes_without_a_matching_delimiter_row_are_text);
    test_run("a full reply mixes every block", test_a_full_reply_mixes_every_block);
    test_run("plain text is one span without a URL", test_plain_text_is_one_span_without_a_url);
    test_run("newlines stay in the text", test_newlines_stay_in_the_text);
    test_run("code spans take matching backtick runs", test_code_spans_take_matching_backtick_runs);
    test_run("unclosed backticks are literal", test_unclosed_backticks_are_literal);
    test_run("bold, italic and strike markers", test_bold_italic_and_strike_markers);
    test_run("tripled markers are bold and italic", test_tripled_markers_are_bold_and_italic);
    test_run("italics step over bold inside them", test_italics_step_over_bold_inside_them);
    test_run("emphasis needs text hugging its markers", test_emphasis_needs_text_hugging_its_markers);
    test_run("unclosed emphasis is literal", test_unclosed_emphasis_is_literal);
    test_run("intraword underscores are literal", test_intraword_underscores_are_literal);
    test_run("backslash escapes drop the backslash", test_backslash_escapes_drop_the_backslash);
    test_run("links carry their URL on every label span", test_links_carry_their_url_on_every_label_span);
    test_run("link urls keep balanced parentheses", test_link_urls_keep_balanced_parentheses);
    test_run("links with an empty label show their url", test_links_with_an_empty_label_show_their_url);
    test_run("adjacent links stay separate spans", test_adjacent_links_stay_separate_spans);
    test_run("brackets that are not links are literal", test_brackets_that_are_not_links_are_literal);
    test_run("images show their alt text as a link", test_images_show_their_alt_text_as_a_link);
    test_run("angle autolinks", test_angle_autolinks);
    test_run("bare URLs drop trailing punctuation", test_bare_urls_drop_trailing_punctuation);
    test_run("URL schemes inside words are not links", test_url_schemes_inside_words_are_not_links);
    test_run("HTML entities are decoded", test_html_entities_are_decoded);
    test_run("spans with the same style merge", test_spans_with_the_same_style_merge);
    test_run("md_plain strips every marker", test_md_plain_strips_every_marker);
}
