/* Shared helpers: slug rule, HTML escaping, word count, long dates.
   Written fresh in this session. */
#include "common.h"
#include "unicode.h"

int is_mark(uint32_t cp) {
  int lo = 0, hi = MARK_RANGE_COUNT - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    if (cp < MARK_RANGES[mid][0]) hi = mid - 1;
    else if (cp > MARK_RANGES[mid][1]) lo = mid + 1;
    else return 1;
  }
  return 0;
}

/* One decomposed code point's contribution to a slug: marks are dropped
   silently, ASCII letters/digits kept, anything else opens a gap. */
static inline void slug_cp(Buf *out, uint32_t cp, int *gap) {
  if (is_mark(cp)) return;
  if (cp >= 'A' && cp <= 'Z') cp += 'a' - 'A';
  if ((cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9')) {
    if (*gap && out->len > 0) buf_addc(out, '-');
    *gap = 0;
    buf_addc(out, (char)cp);
    return;
  }
  if (out->len > 0) *gap = 1;
}

void slugify_into(Buf *out, Str text) {
  size_t start = out->len;
  int gap = 0;
  size_t i = 0;
  while (i < text.n) {
    unsigned char c = (unsigned char)text.p[i];
    if (c < 0x80) {
      slug_cp(out, c, &gap);
      i++;
      continue;
    }
    uint32_t cp;
    int n = utf8_decode(text.p + i, text.n - i, &cp);
    i += n;
    /* NFKD lookup (binary search); the table holds the full recursive
       decomposition as UTF-8. A code point with no entry keeps itself. */
    int lo = 0, hi = NFKD_COUNT - 1, found = -1;
    while (lo <= hi) {
      int mid = (lo + hi) >> 1;
      if (cp < NFKD_CP[mid]) hi = mid - 1;
      else if (cp > NFKD_CP[mid]) lo = mid + 1;
      else {
        found = mid;
        break;
      }
    }
    if (found < 0) {
      slug_cp(out, cp, &gap);
      continue;
    }
    size_t b0 = NFKD_OFF[found], b1 = NFKD_OFF[found + 1];
    size_t j = b0;
    while (j < b1) {
      uint32_t dcp;
      int dn = utf8_decode((const char *)NFKD_STR + j, b1 - j, &dcp);
      j += dn;
      slug_cp(out, dcp, &gap);
    }
  }
  /* Cut to 64 chars, drop a "-" left at the cut, "untitled" when empty. */
  size_t len = out->len - start;
  if (len > 64) {
    out->len = start + 64;
    len = 64;
  }
  while (len > 0 && out->buf[start + len - 1] == '-') {
    out->len--;
    len--;
  }
  if (len == 0) buf_addz(out, "untitled");
}

Str slugify_str(Arena *a, Str text) {
  Buf b;
  buf_init(&b);
  slugify_into(&b, text);
  char *d = arena_dup(a, b.buf, b.len);
  free(b.buf);
  return str_of(d, b.len);
}

void escape_html_into(Buf *out, Str s) { escape_html_into_cstr(out, s.p, s.n); }

void escape_html_into_cstr(Buf *out, const char *p, size_t n) {
  /* Fast scan: copy runs between the five escapable bytes. */
  size_t i = 0;
  while (i < n) {
    size_t j = i;
    while (j < n) {
      char c = p[j];
      if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'') break;
      j++;
    }
    if (j > i) buf_addn(out, p + i, j - i);
    if (j >= n) break;
    switch (p[j]) {
      case '&': buf_addz(out, "&amp;"); break;
      case '<': buf_addz(out, "&lt;"); break;
      case '>': buf_addz(out, "&gt;"); break;
      case '"': buf_addz(out, "&quot;"); break;
      default: buf_addz(out, "&#39;"); break;
    }
    i = j + 1;
  }
}

long words_of(Str text) {
  long count = 0;
  size_t i = 0;
  while (i < text.n) {
    unsigned char c = (unsigned char)text.p[i];
    uint32_t cp;
    int n;
    if (c < 0x80) {
      cp = c;
      n = 1;
    } else {
      n = utf8_decode(text.p + i, text.n - i, &cp);
    }
    if (js_is_space(cp)) {
      i += n;
      continue;
    }
    count++;
    i += n;
    while (i < text.n) {
      c = (unsigned char)text.p[i];
      if (c < 0x80) {
        cp = c;
        n = 1;
      } else {
        n = utf8_decode(text.p + i, text.n - i, &cp);
      }
      if (js_is_space(cp)) break;
      i += n;
    }
  }
  return count;
}

static const char *MONTHS[12] = {"January", "February", "March",     "April",
                                  "May",     "June",     "July",      "August",
                                  "September", "October", "November", "December"};

void long_date_into(Buf *out, Str date) {
  /* date is "YYYY-MM-DD" (validated by the front matter). */
  if (date.n < 10) return;
  int y = (date.p[0] - '0') * 1000 + (date.p[1] - '0') * 100 +
          (date.p[2] - '0') * 10 + (date.p[3] - '0');
  int m = (date.p[5] - '0') * 10 + (date.p[6] - '0');
  int d = (date.p[8] - '0') * 10 + (date.p[9] - '0');
  char tmp[32];
  int w = snprintf(tmp, sizeof tmp, "%d %s %d", d, MONTHS[m - 1], y);
  buf_addn(out, tmp, (size_t)w);
}
