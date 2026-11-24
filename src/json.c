/* JSON parsing (arena DOM) and string escaping. Written fresh. */
#include "json.h"

typedef struct {
  const char *p;
  size_t n;
  size_t i;
  Arena *a;
  int err;
} Parser;

static void skip_ws(Parser *ps) {
  while (ps->i < ps->n) {
    char c = ps->p[ps->i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ps->i++;
    else break;
  }
}

static JVal *parse_value(Parser *ps);

static int hex4_at(const char *p, size_t n, size_t i, uint32_t *out) {
  if (i + 4 > n) return 0;
  uint32_t v = 0;
  for (int k = 0; k < 4; k++) {
    char c = p[i + k];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
    else return 0;
  }
  *out = v;
  return 1;
}

/* Parse a string starting at the opening quote; ps->i ends past the close.
   Strings without escapes point straight into the input; with escapes the
   unescaped copy goes to the arena. */
static Str parse_string(Parser *ps) {
  /* ps->p[ps->i] == '"' */
  size_t start = ++ps->i;
  size_t j = start;
  /* fast scan for the next '"' or '\\' */
  while (j < ps->n) {
    char c = ps->p[j];
    if (c == '"' || c == '\\') break;
    j++;
  }
  if (j < ps->n && ps->p[j] == '"') {
    ps->i = j + 1;
    return str_of(ps->p + start, j - start);
  }
  /* slow path: escapes present */
  Buf b;
  buf_init(&b);
  size_t k = start;
  int closed = 0;
  while (k < ps->n) {
    char c = ps->p[k];
    if (c == '"') {
      k++;
      closed = 1;
      break;
    }
    if (c != '\\') {
      buf_addc(&b, c);
      k++;
      continue;
    }
    k++;
    if (k >= ps->n) break;
    char e = ps->p[k++];
    switch (e) {
      case '"': buf_addc(&b, '"'); break;
      case '\\': buf_addc(&b, '\\'); break;
      case '/': buf_addc(&b, '/'); break;
      case 'b': buf_addc(&b, '\b'); break;
      case 'f': buf_addc(&b, '\f'); break;
      case 'n': buf_addc(&b, '\n'); break;
      case 'r': buf_addc(&b, '\r'); break;
      case 't': buf_addc(&b, '\t'); break;
      case 'u': {
        uint32_t cp;
        if (!hex4_at(ps->p, ps->n, k, &cp)) {
          ps->err = 1;
          break;
        }
        k += 4;
        if (cp >= 0xd800 && cp <= 0xdbff) {
          /* try a low surrogate */
          uint32_t lo;
          if (k + 5 < ps->n && ps->p[k] == '\\' && ps->p[k + 1] == 'u' &&
              hex4_at(ps->p, ps->n, k + 2, &lo) && lo >= 0xdc00 && lo <= 0xdfff) {
            cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
            k += 6;
          } else {
            cp = 0xfffd;
          }
        } else if (cp >= 0xdc00 && cp <= 0xdfff) {
          cp = 0xfffd;
        }
        char tmp[4];
        int w = utf8_encode(cp, tmp);
        buf_addn(&b, tmp, (size_t)w);
        break;
      }
      default:
        ps->err = 1;
        break;
    }
    if (ps->err) break;
  }
  if (!closed) ps->err = 1;
  ps->i = k;
  char *d = arena_dup(ps->a, b.buf, b.len);
  Str s = str_of(d, b.len);
  free(b.buf);
  return s;
}

static JVal *new_val(Parser *ps, JKind k) {
  JVal *v = (JVal *)arena_alloc(ps->a, sizeof(JVal));
  memset(v, 0, sizeof *v);
  v->k = k;
  return v;
}

static JVal *parse_value(Parser *ps) {
  skip_ws(ps);
  if (ps->i >= ps->n) {
    ps->err = 1;
    return NULL;
  }
  char c = ps->p[ps->i];
  if (c == '{') {
    ps->i++;
    JVal *v = new_val(ps, J_OBJ);
    size_t cap = 0;
    skip_ws(ps);
    if (ps->i < ps->n && ps->p[ps->i] == '}') {
      ps->i++;
      return v;
    }
    for (;;) {
      skip_ws(ps);
      if (ps->i >= ps->n || ps->p[ps->i] != '"') {
        ps->err = 1;
        return NULL;
      }
      Str key = parse_string(ps);
      if (ps->err) return NULL;
      skip_ws(ps);
      if (ps->i >= ps->n || ps->p[ps->i] != ':') {
        ps->err = 1;
        return NULL;
      }
      ps->i++;
      JVal *val = parse_value(ps);
      if (ps->err) return NULL;
      if (v->nfields == cap) {
        size_t nc = cap == 0 ? 4 : cap * 2;
        Str *nk = (Str *)arena_alloc(ps->a, nc * sizeof(Str));
        JVal **nv = (JVal **)arena_alloc(ps->a, nc * sizeof(JVal *));
        if (v->nfields > 0) {
          memcpy(nk, v->keys, v->nfields * sizeof(Str));
          memcpy(nv, v->vals, v->nfields * sizeof(JVal *));
        }
        v->keys = nk;
        v->vals = nv;
        cap = nc;
      }
      v->keys[v->nfields] = key;
      v->vals[v->nfields] = val;
      v->nfields++;
      skip_ws(ps);
      if (ps->i >= ps->n) {
        ps->err = 1;
        return NULL;
      }
      if (ps->p[ps->i] == ',') {
        ps->i++;
        continue;
      }
      if (ps->p[ps->i] == '}') {
        ps->i++;
        return v;
      }
      ps->err = 1;
      return NULL;
    }
  }
  if (c == '[') {
    ps->i++;
    JVal *v = new_val(ps, J_ARR);
    size_t cap = 0;
    skip_ws(ps);
    if (ps->i < ps->n && ps->p[ps->i] == ']') {
      ps->i++;
      return v;
    }
    for (;;) {
      JVal *item = parse_value(ps);
      if (ps->err) return NULL;
      if (v->count == cap) {
        size_t nc = cap == 0 ? 8 : cap * 2;
        JVal **ni = (JVal **)arena_alloc(ps->a, nc * sizeof(JVal *));
        if (v->count > 0) memcpy(ni, v->items, v->count * sizeof(JVal *));
        v->items = ni;
        cap = nc;
      }
      v->items[v->count++] = item;
      skip_ws(ps);
      if (ps->i >= ps->n) {
        ps->err = 1;
        return NULL;
      }
      if (ps->p[ps->i] == ',') {
        ps->i++;
        continue;
      }
      if (ps->p[ps->i] == ']') {
        ps->i++;
        return v;
      }
      ps->err = 1;
      return NULL;
    }
  }
  if (c == '"') {
    JVal *v = new_val(ps, J_STR);
    v->s = parse_string(ps);
    return ps->err ? NULL : v;
  }
  if (c == 't') {
    if (ps->i + 4 <= ps->n && memcmp(ps->p + ps->i, "true", 4) == 0) {
      ps->i += 4;
      JVal *v = new_val(ps, J_BOOL);
      v->boolean = 1;
      return v;
    }
    ps->err = 1;
    return NULL;
  }
  if (c == 'f') {
    if (ps->i + 5 <= ps->n && memcmp(ps->p + ps->i, "false", 5) == 0) {
      ps->i += 5;
      JVal *v = new_val(ps, J_BOOL);
      v->boolean = 0;
      return v;
    }
    ps->err = 1;
    return NULL;
  }
  if (c == 'n') {
    if (ps->i + 4 <= ps->n && memcmp(ps->p + ps->i, "null", 4) == 0) {
      ps->i += 4;
      return new_val(ps, J_NULL);
    }
    ps->err = 1;
    return NULL;
  }
  /* number */
  {
    size_t start = ps->i;
    if (ps->i < ps->n && ps->p[ps->i] == '-') ps->i++;
    int is_int = 1;
    while (ps->i < ps->n) {
      char d = ps->p[ps->i];
      if (d >= '0' && d <= '9') {
        ps->i++;
        continue;
      }
      if (d == '.' || d == 'e' || d == 'E' || d == '+' || d == '-') is_int = 0;
      break;
    }
    /* consume the rest of a non-integer number */
    while (ps->i < ps->n) {
      char d = ps->p[ps->i];
      if ((d >= '0' && d <= '9') || d == '.' || d == 'e' || d == 'E' || d == '+' || d == '-')
        ps->i++;
      else break;
    }
    if (ps->i == start || (ps->i == start + 1 && ps->p[start] == '-')) {
      ps->err = 1;
      return NULL;
    }
    JVal *v = new_val(ps, J_NUM);
    if (is_int) {
      long long num = 0;
      size_t k = start;
      int neg = 0;
      if (ps->p[k] == '-') {
        neg = 1;
        k++;
      }
      int ok = 1;
      for (; k < ps->i; k++) {
        int dgt = ps->p[k] - '0';
        if (num > (9223372036854775807LL - dgt) / 10) {
          ok = 0;
          break;
        }
        num = num * 10 + dgt;
      }
      if (ok) {
        v->num_is_int = 1;
        v->num = neg ? -num : num;
        return v;
      }
    }
    /* fall back to a double */
    char tmp[64];
    size_t ln = ps->i - start;
    if (ln >= sizeof tmp) {
      ps->err = 1;
      return NULL;
    }
    memcpy(tmp, ps->p + start, ln);
    tmp[ln] = 0;
    v->num_is_int = 0;
    v->dbl = strtod(tmp, NULL);
    return v;
  }
}

JVal *json_parse(Arena *a, const char *p, size_t n) {
  Parser ps = {p, n, 0, a, 0};
  JVal *v = parse_value(&ps);
  if (ps.err) return NULL;
  skip_ws(&ps);
  if (ps.i != ps.n) return NULL;
  return v;
}

JVal *json_get(JVal *obj, const char *key) {
  if (!obj || obj->k != J_OBJ) return NULL;
  size_t kl = strlen(key);
  for (size_t i = 0; i < obj->nfields; i++) {
    if (obj->keys[i].n == kl && memcmp(obj->keys[i].p, key, kl) == 0) return obj->vals[i];
  }
  return NULL;
}

void json_escape_into(Buf *out, Str s) {
  buf_addc(out, '"');
  size_t i = 0;
  while (i < s.n) {
    unsigned char c = (unsigned char)s.p[i];
    if (c >= 0x20 && c != '"' && c != '\\') {
      size_t j = i + 1;
      while (j < s.n) {
        unsigned char d = (unsigned char)s.p[j];
        if (d < 0x20 || d == '"' || d == '\\') break;
        j++;
      }
      buf_addn(out, s.p + i, j - i);
      i = j;
      continue;
    }
    switch (c) {
      case '"': buf_addz(out, "\\\""); break;
      case '\\': buf_addz(out, "\\\\"); break;
      case '\b': buf_addz(out, "\\b"); break;
      case '\f': buf_addz(out, "\\f"); break;
      case '\n': buf_addz(out, "\\n"); break;
      case '\r': buf_addz(out, "\\r"); break;
      case '\t': buf_addz(out, "\\t"); break;
      default: {
        char tmp[8];
        int w = snprintf(tmp, sizeof tmp, "\\u%04x", c);
        buf_addn(out, tmp, (size_t)w);
        break;
      }
    }
    i++;
  }
  buf_addc(out, '"');
}
