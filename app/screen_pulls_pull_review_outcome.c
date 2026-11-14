#include "screen_pulls_internal.h"

// What the pull request's code review amounts to, at the top of the Code reviews tab (review_outcome_card.c). The
// security pass and the approvals the change's risk asks for follow in blocks of their own
// (screen_pulls_pull_review_security.c and review_approvals_card.c).

// Keeps the outcome and rounds `answer` carries, as `source` decides, beside the reviews.
void screen_pulls_review_outcome_take(PullReviews *r, const Json *answer, ReviewOutcomeSource source) {
    review_outcome_keep(&r->outcome_state, &r->outcome, answer, source);
}
void screen_pulls_review_outcome_clear(PullReviews *r) { review_outcome_drop(&r->outcome_state, &r->outcome); }
// The card, while the server has sent an outcome; nothing from an older server. The security pass has its own block.
void screen_pulls_review_outcome(PullScreen *s, Doc *doc, Col c) {
    if (s->reviews.outcome) review_outcome_card(doc, c.ix, c.iw, s->reviews.outcome, false);
}
