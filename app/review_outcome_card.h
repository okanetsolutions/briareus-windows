// What a pull request's code review amounts to, drawn as a card (the Mac's ReviewOutcomeCard): the outcome and the rounds
// used, whether it is ready, stale or still to reach GitHub, why, and the security pass with its own reasons. The Code
// reviews tab draws it above the reviews and ◉ Status for a pull request waiting on a person. The words and tones are
// review_outcome.c's.
#ifndef BRIAREUS_REVIEW_OUTCOME_CARD_H
#define BRIAREUS_REVIEW_OUTCOME_CARD_H
#include "review_outcome.h"
#include "screens.h"

COLORREF review_tone_color(ReviewTone tone);
// The reasons that block or explain first, each in its tone, then the notes in grey; `gap` above them when there are any.
void review_reasons_draw(Doc *doc, int x, int w, const ReviewReasons *reasons, int gap);
// A titled fact, "Security  Passed · required", on a line of its own; takes `value`.
void review_fact_draw(Doc *doc, int x, int w, const char *title, char *value, ReviewTone tone);
// The card, boxed across `w`, with space under it.
void review_outcome_card(Doc *doc, int x, int w, const ReviewOutcome *outcome);
#endif
