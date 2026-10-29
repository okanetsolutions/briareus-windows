#include "screen_pulls_internal.h"

char *screen_pulls_review_amount(CodeReviewAmount amount, bool money, bool estimated, size_t unpriced) {
    char *value;
    if (!amount.known) value = xstrdup("Unknown");
    else if (!money) value = format_tokens(amount.value);
    else value = amount.value > 0 && amount.value < 0.01 ? xstrdup("<$0.01") : format_cost(amount.value);
    Str text; str_init(&text); str_appendz(&text, value); free(value);
    if (money && amount.known && estimated) str_appendz(&text, " estimated");
    if (amount.known && amount.unknown) str_appendf(&text, "\npartial · %zu unknown", amount.unknown);
    if (unpriced) str_appendf(&text, "\n%s%zu unpriced turns", amount.known ? "partial · " : "", unpriced);
    return str_detach(&text);
}
