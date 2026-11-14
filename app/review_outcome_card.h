// What a pull request's code review amounts to, drawn as a card (the Mac's ReviewOutcomeCard), and the pieces the review
// cards share: tones in the theme's colours, the server's reasons, titled facts and optional lines. The Code reviews tab
// draws the card above the reviews, and Status for a pull request waiting on a person's approval. The words and tones
// are review_outcome.c's.
#ifndef BRIAREUS_REVIEW_OUTCOME_CARD_H
#define BRIAREUS_REVIEW_OUTCOME_CARD_H
#include "review_outcome.h"
#include "screens.h"

COLORREF review_tone_color(ReviewTone tone);
// The reasons that hold it back first, each in its tone, then the notes in grey; `gap` above them when there are any.
void review_reasons_draw(Doc *doc, int x, int w, const ReviewReasons *reasons, int gap);
// A titled fact, "Security  Passed · required", on a line of its own; frees `value`.
void review_fact_draw(Doc *doc, int x, int w, const char *title, char *value, ReviewTone tone);
// Wrapped text, or nothing for NULL or an empty string.
void review_line_draw(Doc *doc, int x, int w, const char *text, FontId f, COLORREF color);
// The card, boxed across `w`, with space under it: the outcome and the rounds used, whether it is ready, stale or still to
// reach GitHub, why, and with `security` the security pass in a line with its reasons (where no block of its own shows).
void review_outcome_card(Doc *doc, int x, int w, const ReviewOutcome *outcome, bool security);
#endif
