#include "vfh_srt.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *SIMPLE =
    "1\n"
    "00:00:01,000 --> 00:00:04,000\n"
    "Hello there\n"
    "\n"
    "2\n"
    "00:00:05,500 --> 00:00:08,000\n"
    "Two lines\n"
    "of dialogue\n"
    "\n";

int main(void) {
    /* Basic parse + lookup. */
    vfh_srt *srt = vfh_srt_parse(SIMPLE);
    assert(srt && vfh_srt_count(srt) == 2);
    assert(vfh_srt_text_at(srt, 0.5) == NULL);              /* before the first cue */
    assert(strcmp(vfh_srt_text_at(srt, 2.0), "Hello there") == 0);
    assert(vfh_srt_text_at(srt, 4.5) == NULL);              /* in the gap */
    assert(strcmp(vfh_srt_text_at(srt, 6.0), "Two lines\nof dialogue") == 0);
    assert(vfh_srt_text_at(srt, 99.0) == NULL);             /* past the end */
    /* Boundaries: start is inclusive, end is exclusive. */
    assert(vfh_srt_text_at(srt, 1.0) != NULL);
    assert(vfh_srt_text_at(srt, 4.0) == NULL);
    vfh_srt_free(srt);

    /* CRLF line endings and a UTF-8 BOM -- both common from Windows tools. */
    const char *crlf = "\xEF\xBB\xBF" "1\r\n00:00:02,000 --> 00:00:03,000\r\nCRLF ok\r\n\r\n";
    srt = vfh_srt_parse(crlf);
    assert(srt && vfh_srt_count(srt) == 1);
    assert(strcmp(vfh_srt_text_at(srt, 2.5), "CRLF ok") == 0);
    vfh_srt_free(srt);

    /* No index lines, and '.' instead of ',' as the decimal separator. */
    const char *terse = "00:00:01.000 --> 00:00:02.000\nNo index\n\n";
    srt = vfh_srt_parse(terse);
    assert(srt && vfh_srt_count(srt) == 1);
    assert(strcmp(vfh_srt_text_at(srt, 1.5), "No index") == 0);
    vfh_srt_free(srt);

    /* Final cue with no trailing blank line must still be kept. */
    const char *unterminated = "1\n00:00:01,000 --> 00:00:02,000\nLast cue";
    srt = vfh_srt_parse(unterminated);
    assert(srt && vfh_srt_count(srt) == 1);
    assert(strcmp(vfh_srt_text_at(srt, 1.5), "Last cue") == 0);
    vfh_srt_free(srt);

    /* Out-of-order cues get sorted, so the binary search stays valid. */
    const char *shuffled =
        "1\n00:00:10,000 --> 00:00:11,000\nLater\n\n"
        "2\n00:00:01,000 --> 00:00:02,000\nEarlier\n\n";
    srt = vfh_srt_parse(shuffled);
    assert(srt && vfh_srt_count(srt) == 2);
    assert(strcmp(vfh_srt_text_at(srt, 1.5), "Earlier") == 0);
    assert(strcmp(vfh_srt_text_at(srt, 10.5), "Later") == 0);
    vfh_srt_free(srt);

    /* Hours are honoured, not truncated to minutes. */
    const char *long_film = "1\n01:02:03,500 --> 01:02:05,000\nDeep in\n\n";
    srt = vfh_srt_parse(long_film);
    assert(srt && vfh_srt_count(srt) == 1);
    assert(vfh_srt_text_at(srt, 3723.6) != NULL);   /* 1h02m03.6s */
    assert(vfh_srt_text_at(srt, 3722.0) == NULL);
    vfh_srt_free(srt);

    /* Garbage in must not crash, and must not masquerade as cues. */
    assert(vfh_srt_parse("not a subtitle file at all\n\nreally not\n") == NULL);
    assert(vfh_srt_parse("") == NULL);
    assert(vfh_srt_parse(NULL) == NULL);
    /* A timing line with no text is not a cue. */
    assert(vfh_srt_parse("1\n00:00:01,000 --> 00:00:02,000\n\n") == NULL);
    /* NULL-safe accessors. */
    assert(vfh_srt_text_at(NULL, 1.0) == NULL);
    assert(vfh_srt_count(NULL) == 0);
    vfh_srt_free(NULL);

    /* Non-ASCII passes through untouched rather than being mangled. */
    const char *utf8 = "1\n00:00:01,000 --> 00:00:02,000\nvoilà — ça va\n\n";
    srt = vfh_srt_parse(utf8);
    assert(srt && strcmp(vfh_srt_text_at(srt, 1.5), "voilà — ça va") == 0);
    vfh_srt_free(srt);

    printf("vfh_srt_test: ok\n");
    return 0;
}
