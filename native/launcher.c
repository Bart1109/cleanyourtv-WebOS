/* CleanYourTV native launcher: the same Home as app/index.html, drawn with SDL2 on the TV's GPU (OpenGL ES 2).
 * Data comes from the CleanYourTV service on 127.0.0.1:8765, exactly like the web launcher.
 * Draws nothing while nothing moves. Built with native/build.sh. */
#include <SDL.h>
#include <SDL_ttf.h>
#include <SDL_image.h>
#define SDL_VIDEO_DRIVER_WAYLAND 1   /* for the wl member of SDL_SysWMinfo */
#include <SDL_syswm.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define VERSION "0.1.0"
#define W 1920
#define H 1080
#define PORT 8765

/* ---------------------------------------------------------------- JSON (just enough for the service's replies) */
enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };
typedef struct J { int t; double n; char *s; struct J **v; char **k; int len, cap; } J;
static J *jnew(int t) { J *j = calloc(1, sizeof *j); j->t = t; return j; }
static void jfree(J *j) {
  if (!j) return;
  for (int i = 0; i < j->len; i++) { jfree(j->v[i]); if (j->k) free(j->k[i]); }
  free(j->v); free(j->k); free(j->s); free(j);
}
static void jgrow(J *j) { if (j->len == j->cap) { j->cap = j->cap ? j->cap * 2 : 8; j->v = realloc(j->v, j->cap * sizeof *j->v); if (j->t == J_OBJ) j->k = realloc(j->k, j->cap * sizeof *j->k); } }
static void jpush(J *a, J *v) { jgrow(a); a->v[a->len++] = v; }
static J *jstrn(const char *s) { J *j = jnew(J_STR); j->s = strdup(s); return j; }
static J *jget(J *o, const char *key) { if (!o || o->t != J_OBJ) return NULL; for (int i = 0; i < o->len; i++) if (!strcmp(o->k[i], key)) return o->v[i]; return NULL; }
static void jset(J *o, const char *key, J *v) {
  for (int i = 0; i < o->len; i++) if (!strcmp(o->k[i], key)) { jfree(o->v[i]); o->v[i] = v; return; }
  jgrow(o); o->k[o->len] = strdup(key); o->v[o->len++] = v;
}
static void jdel(J *o, const char *key) {
  for (int i = 0; i < o->len; i++) if (!strcmp(o->k[i], key)) { jfree(o->v[i]); free(o->k[i]); memmove(o->v + i, o->v + i + 1, (o->len - i - 1) * sizeof *o->v); memmove(o->k + i, o->k + i + 1, (o->len - i - 1) * sizeof *o->k); o->len--; return; }
}
static const char *jstr(J *j) { return j && j->t == J_STR ? j->s : NULL; }

static const char *skip(const char *p) { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; return p; }
static J *jparse_v(const char **pp);
static char *jparse_s(const char **pp) {
  const char *p = *pp + 1; size_t cap = 32, n = 0; char *o = malloc(cap);
  while (*p && *p != '"') {
    if (n + 8 >= cap) { cap *= 2; o = realloc(o, cap); }
    if (*p == '\\') {
      p++;
      switch (*p) {
        case 'n': o[n++] = '\n'; break; case 't': o[n++] = '\t'; break; case 'r': o[n++] = '\r'; break;
        case 'b': o[n++] = '\b'; break; case 'f': o[n++] = '\f'; break;
        case 'u': {
          unsigned c = 0; for (int i = 1; i <= 4 && p[i]; i++) { char h = p[i]; c = c * 16 + (h <= '9' ? h - '0' : (h | 32) - 'a' + 10); }
          p += 4;
          if (c >= 0xD800 && c < 0xDC00 && p[1] == '\\' && p[2] == 'u') { unsigned d = 0; for (int i = 3; i <= 6; i++) { char h = p[i]; d = d * 16 + (h <= '9' ? h - '0' : (h | 32) - 'a' + 10); } c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00); p += 6; }
          if (c < 0x80) o[n++] = c; else if (c < 0x800) { o[n++] = 0xC0 | (c >> 6); o[n++] = 0x80 | (c & 63); }
          else if (c < 0x10000) { o[n++] = 0xE0 | (c >> 12); o[n++] = 0x80 | ((c >> 6) & 63); o[n++] = 0x80 | (c & 63); }
          else { o[n++] = 0xF0 | (c >> 18); o[n++] = 0x80 | ((c >> 12) & 63); o[n++] = 0x80 | ((c >> 6) & 63); o[n++] = 0x80 | (c & 63); }
          break; }
        default: o[n++] = *p;
      }
      if (*p) p++;
    } else o[n++] = *p++;
  }
  o[n] = 0; *pp = *p ? p + 1 : p; return o;
}
static J *jparse_v(const char **pp) {
  const char *p = skip(*pp); J *j;
  if (*p == '{') {
    j = jnew(J_OBJ); p = skip(p + 1);
    while (*p && *p != '}') {
      if (*p != '"') break;
      char *k = jparse_s(&p); p = skip(p); if (*p == ':') p++;
      *pp = p; J *v = jparse_v(pp); p = skip(*pp);
      jgrow(j); j->k[j->len] = k; j->v[j->len++] = v;
      if (*p == ',') p = skip(p + 1);
    }
    if (*p == '}') p++;
  } else if (*p == '[') {
    j = jnew(J_ARR); p = skip(p + 1);
    while (*p && *p != ']') { *pp = p; J *v = jparse_v(pp); p = skip(*pp); jpush(j, v); if (*p == ',') p = skip(p + 1); }
    if (*p == ']') p++;
  } else if (*p == '"') { j = jnew(J_STR); j->s = jparse_s(&p); }
  else if (!strncmp(p, "true", 4)) { j = jnew(J_BOOL); j->n = 1; p += 4; }
  else if (!strncmp(p, "false", 5)) { j = jnew(J_BOOL); p += 5; }
  else if (!strncmp(p, "null", 4)) { j = jnew(J_NULL); p += 4; }
  else { char *e; j = jnew(J_NUM); j->n = strtod(p, &e); if (e == p) e = (char *)p + 1; p = e; }
  *pp = p; return j;
}
static J *jparse(const char *s) { const char *p = s; return jparse_v(&p); }

typedef struct { char *b; size_t n, cap; } Buf;
static void bput(Buf *b, const char *s, size_t n) { if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->b = realloc(b->b, b->cap); } memcpy(b->b + b->n, s, n); b->n += n; b->b[b->n] = 0; }
static void bstr(Buf *b, const char *s) { bput(b, s, strlen(s)); }
static void jdump(J *j, Buf *b) {
  char t[40];
  switch (j->t) {
    case J_NULL: bstr(b, "null"); break;
    case J_BOOL: bstr(b, j->n ? "true" : "false"); break;
    case J_NUM: snprintf(t, sizeof t, "%.15g", j->n); bstr(b, t); break;
    case J_STR: bput(b, "\"", 1); for (const char *p = j->s; *p; p++) { if (*p == '"' || *p == '\\') { bput(b, "\\", 1); bput(b, p, 1); } else if ((unsigned char)*p < 32) { snprintf(t, sizeof t, "\\u%04x", *p); bstr(b, t); } else bput(b, p, 1); } bput(b, "\"", 1); break;
    case J_ARR: bput(b, "[", 1); for (int i = 0; i < j->len; i++) { if (i) bput(b, ",", 1); jdump(j->v[i], b); } bput(b, "]", 1); break;
    case J_OBJ: bput(b, "{", 1); for (int i = 0; i < j->len; i++) { if (i) bput(b, ",", 1); J k = { J_STR, 0, j->k[i] }; jdump(&k, b); bput(b, ":", 1); jdump(j->v[i], b); } bput(b, "}", 1); break;
  }
}

/* ---------------------------------------------------------------- HTTP to the service (HTTP/1.0: no chunking) */
static unsigned char *http(const char *method, const char *path, const char *body, int *outlen) {
  int s = socket(AF_INET, SOCK_STREAM, 0); if (s < 0) return NULL;
  struct timeval tv = { 8, 0 }; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  struct sockaddr_in a = { 0 }; a.sin_family = AF_INET; a.sin_port = htons(PORT); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) { close(s); return NULL; }
  char head[512]; int bl = body ? (int)strlen(body) : 0;
  int hl = snprintf(head, sizeof head, "%s %s HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n", method, path, bl);
  if (send(s, head, hl, 0) < 0 || (bl && send(s, body, bl, 0) < 0)) { close(s); return NULL; }
  size_t cap = 65536, n = 0; unsigned char *r = malloc(cap);
  for (;;) { if (n + 4096 > cap) { cap *= 2; r = realloc(r, cap); } ssize_t k = recv(s, r + n, cap - n, 0); if (k <= 0) break; n += k; }
  close(s);
  unsigned char *e = NULL; for (size_t i = 0; i + 3 < n; i++) if (!memcmp(r + i, "\r\n\r\n", 4)) { e = r + i + 4; break; }
  if (!e || n < 12 || memcmp(r, "HTTP/1.", 7) || r[9] != '2') { free(r); return NULL; }
  size_t off = e - r; memmove(r, e, n - off); n -= off; r[n] = 0; if (outlen) *outlen = (int)n; return r;
}

/* ---------------------------------------------------------------- language (follows the TV) */
static int lang = 0;   /* 0 en 1 nl 2 de 3 fr 4 es */
typedef struct { const char *en, *nl, *de, *fr, *es; } Tx;
static const Tx TXT[] = {
  { "Inputs", "Ingangen", "Eing\xc3\xa4nge", "Entr\xc3\xa9""es", "Entradas" },
  { "Settings", "Instellingen", "Einstellungen", "Param\xc3\xa8tres", "Ajustes" },
  { "Customise", "Aanpassen", "Anpassen", "Personnaliser", "Personalizar" },
  { "Open", "Openen", "\xc3\x96""ffnen", "Ouvrir", "Abrir" },
  { "Move left in dock", "Naar links in dock", "Im Dock nach links", "D\xc3\xa9""placer \xc3\xa0 gauche dans le dock", "Mover a la izquierda en el dock" },
  { "Move right in dock", "Naar rechts in dock", "Im Dock nach rechts", "D\xc3\xa9""placer \xc3\xa0 droite dans le dock", "Mover a la derecha en el dock" },
  { "Remove from dock", "Uit dock verwijderen", "Aus dem Dock entfernen", "Retirer du dock", "Quitar del dock" },
  { "Add to dock", "Aan dock toevoegen", "Zum Dock hinzuf\xc3\xbc""gen", "Ajouter au dock", "A\xc3\xb1""adir al dock" },
  { "Add to dock...", "Aan dock toevoegen...", "Zum Dock hinzuf\xc3\xbc""gen...", "Ajouter au dock...", "A\xc3\xb1""adir al dock..." },
  { "Replace which app?", "Welke app vervangen?", "Welche App ersetzen?", "Remplacer quelle app ?", "\xc2\xbfQu\xc3\xa9 app reemplazar?" },
  { "Hide app", "App verbergen", "App ausblenden", "Masquer l\xe2\x80\x99""app", "Ocultar app" },
  { "Remove this button", "Deze knop verwijderen", "Diese Schaltfl\xc3\xa4""che entfernen", "Supprimer ce bouton", "Quitar este bot\xc3\xb3n" },
  { "Hold OK on any app to change the dock", "Houd OK ingedrukt op een app om het dock te wijzigen", "Halte OK auf einer App gedr\xc3\xbc""ckt, um das Dock zu \xc3\xa4ndern", "Maintenez OK sur une app pour modifier le dock", "Mant\xc3\xa9n OK en una app para cambiar el dock" },
  { "hold OK on an app", "houd OK ingedrukt op een app", "OK auf einer App gedr\xc3\xbc""ckt halten", "maintenez OK sur une app", "mant\xc3\xa9n OK en una app" },
  { "Top buttons", "Knoppen bovenaan", "Obere Schaltfl\xc3\xa4""chen", "Boutons du haut", "Botones superiores" },
  { "Hidden apps", "Verborgen apps", "Ausgeblendete Apps", "Apps masqu\xc3\xa9""es", "Apps ocultas" },
  { "App order", "Volgorde van apps", "App-Reihenfolge", "Ordre des apps", "Orden de las apps" },
  { "Most used first", "Meest gebruikt eerst", "Meistgenutzte zuerst", "Les plus utilis\xc3\xa9""es d\xe2\x80\x99""abord", "M\xc3\xa1s usadas primero" },
  { "A to Z", "A tot Z", "A bis Z", "De A \xc3\xa0 Z", "De la A a la Z" },
  { "Reset customisation", "Aanpassingen herstellen", "Anpassungen zur\xc3\xbc""cksetzen", "Tout r\xc3\xa9initialiser", "Restablecer personalizaci\xc3\xb3n" },
  { "Add a button...", "Knop toevoegen...", "Schaltfl\xc3\xa4""che hinzuf\xc3\xbc""gen...", "Ajouter un bouton...", "A\xc3\xb1""adir un bot\xc3\xb3n..." },
  { "Add a button", "Knop toevoegen", "Schaltfl\xc3\xa4""che hinzuf\xc3\xbc""gen", "Ajouter un bouton", "A\xc3\xb1""adir un bot\xc3\xb3n" },
  { "An app...", "Een app...", "Eine App...", "Une app...", "Una app..." },
  { "Pick an app", "Kies een app", "App ausw\xc3\xa4hlen", "Choisir une app", "Elige una app" },
  { "OK to remove", "OK om te verwijderen", "OK zum Entfernen", "OK pour supprimer", "OK para quitar" },
  { "Hidden apps (OK to show again)", "Verborgen apps (OK om te tonen)", "Ausgeblendete Apps (OK zum Einblenden)", "Apps masqu\xc3\xa9""es (OK pour afficher)", "Apps ocultas (OK para mostrar)" },
  { "No hidden apps", "Geen verborgen apps", "Keine ausgeblendeten Apps", "Aucune app masqu\xc3\xa9""e", "No hay apps ocultas" },
  { "Could not open", "Kan niet openen", "Konnte nicht ge\xc3\xb6""ffnet werden", "Impossible d\xe2\x80\x99ouvrir", "No se pudo abrir" },
  { "Helper not reachable, retrying...", "Helper niet bereikbaar, opnieuw proberen...", "Helper nicht erreichbar, neuer Versuch...", "Assistant injoignable, nouvel essai...", "Helper no disponible, reintentando..." },
  { "Connected", "Verbonden", "Verbunden", "Connect\xc3\xa9", "Conectado" },
  { "Active", "Actief", "Aktiv", "Actif", "Activo" },
  { "Clock", "Klok", "Uhr", "Horloge", "Reloj" },
  { "Automatic", "Automatisch", "Automatisch", "Automatique", "Autom\xc3\xa1tico" },
  { "24-hour", "24-uurs", "24 Stunden", "24 heures", "24 horas" },
  { "12-hour", "12-uurs", "12 Stunden", "12 heures", "12 horas" },
  { "Live TV", "Live TV", "Live TV", "Live TV", "Live TV" },
  { "Switching to LG Home: the TV restarts...", "Overschakelen naar LG Home: de TV start opnieuw op...", "Wechsel zu LG Home: Der Fernseher startet neu...", "Passage \xc3\xa0 LG Home : le t\xc3\xa9l\xc3\xa9viseur red\xc3\xa9marre...", "Cambiando a LG Home: el televisor se reinicia..." },
};
static const char *t(const char *k) {
  if (!lang) return k;
  for (size_t i = 0; i < sizeof TXT / sizeof *TXT; i++) if (!strcmp(TXT[i].en, k)) { const char *r = (&TXT[i].en)[lang]; return r ? r : k; }
  return k;
}
static const char *DAYS[5][7] = {
  { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" },
  { "zondag", "maandag", "dinsdag", "woensdag", "donderdag", "vrijdag", "zaterdag" },
  { "Sonntag", "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag" },
  { "dimanche", "lundi", "mardi", "mercredi", "jeudi", "vendredi", "samedi" },
  { "domingo", "lunes", "martes", "mi\xc3\xa9rcoles", "jueves", "viernes", "s\xc3\xa1""bado" } };
static const char *MONTHS[5][12] = {
  { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" },
  { "januari", "februari", "maart", "april", "mei", "juni", "juli", "augustus", "september", "oktober", "november", "december" },
  { "Januar", "Februar", "M\xc3\xa4rz", "April", "Mai", "Juni", "Juli", "August", "September", "Oktober", "November", "Dezember" },
  { "janvier", "f\xc3\xa9vrier", "mars", "avril", "mai", "juin", "juillet", "ao\xc3\xbbt", "septembre", "octobre", "novembre", "d\xc3\xa9""cembre" },
  { "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto", "septiembre", "octubre", "noviembre", "diciembre" } };

/* ---------------------------------------------------------------- drawing helpers */
static SDL_Window *win; static SDL_Renderer *ren;
static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }

typedef struct { int w, h, r, ring, blur; SDL_Texture *tex; } RR;
static RR rrs[96]; static int nrr;
/* an anti-aliased rounded rectangle (white; tinted with colour mod), a ring of it, or a soft shadow of it */
static SDL_Texture *rr(int w, int h, int r, int ring, int blur) {
  for (int i = 0; i < nrr; i++) if (rrs[i].w == w && rrs[i].h == h && rrs[i].r == r && rrs[i].ring == ring && rrs[i].blur == blur) return rrs[i].tex;
  int pad = blur, tw = w + 2 * pad, th = h + 2 * pad;
  SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, tw, th, 32, SDL_PIXELFORMAT_ARGB8888);
  float hw = w / 2.0f, hh = h / 2.0f;
  for (int y = 0; y < th; y++) {
    Uint32 *row = (Uint32 *)((Uint8 *)s->pixels + y * s->pitch);
    for (int x = 0; x < tw; x++) {
      float px = x + 0.5f - pad - hw, py = y + 0.5f - pad - hh;
      float dx = fabsf(px) - (hw - r), dy = fabsf(py) - (hh - r);
      float d = sqrtf((dx > 0 ? dx : 0) * (dx > 0 ? dx : 0) + (dy > 0 ? dy : 0) * (dy > 0 ? dy : 0)) + (dx > dy ? (dx < 0 ? dx : 0) : (dy < 0 ? dy : 0)) - r;
      float a;
      if (blur) { a = clampf(1.0f - (d + blur * 0.35f) / (blur * 0.9f), 0, 1); a = a * a * (3 - 2 * a); }
      else if (ring) { float o = clampf(0.5f - d, 0, 1), in = clampf(0.5f - (d + ring), 0, 1); a = o - in; }
      else a = clampf(0.5f - d, 0, 1);
      row[x] = ((Uint32)(a * 255 + 0.5f) << 24) | 0xffffff;
    }
  }
  SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s); SDL_FreeSurface(s);
  SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); SDL_SetTextureScaleMode(t, SDL_ScaleModeBest);
  if (nrr < 96) rrs[nrr++] = (RR){ w, h, r, ring, blur, t };
  return t;
}
static void fillrr(float x, float y, float w, float h, int bw, int bh, int r, int cr, int cg, int cb, int alpha) {
  SDL_Texture *t = rr(bw, bh, r, 0, 0); SDL_SetTextureColorMod(t, cr, cg, cb); SDL_SetTextureAlphaMod(t, alpha);
  SDL_FRect d = { x, y, w, h }; SDL_RenderCopyF(ren, t, NULL, &d);
}

/* fonts and cached text */
#define FDIR "/usr/share/fonts/"
static TTF_Font *fonts[8][160]; static const char *FILES[4] = { "LG_Smart_UI-Regular.ttf", "LG_Smart_UI-SemiBold.ttf", "LG_Smart_UI-Bold.ttf", "LG_Smart_UI-Light.ttf" };
static TTF_Font *font(int wgt, int px) {
  if (px > 159) px = 159;
  if (!fonts[wgt][px]) { char p[160]; snprintf(p, sizeof p, FDIR "%s", FILES[wgt]); fonts[wgt][px] = TTF_OpenFont(p, px); if (!fonts[wgt][px]) fonts[wgt][px] = TTF_OpenFont(FDIR "DroidSans.ttf", px); if (fonts[wgt][px]) TTF_SetFontHinting(fonts[wgt][px], TTF_HINTING_NONE); }
  return fonts[wgt][px];
}
typedef struct { char *s; int wgt, px, maxw; SDL_Texture *tex; int w, h; unsigned age; } TC;
static TC tcs[400]; static int ntc; static unsigned tcAge;
static TC *text(const char *s, int wgt, int px, int maxw) {
  tcAge++;
  for (int i = 0; i < ntc; i++) if (tcs[i].wgt == wgt && tcs[i].px == px && tcs[i].maxw == maxw && !strcmp(tcs[i].s, s)) { tcs[i].age = tcAge; return &tcs[i]; }
  TC *c;
  if (ntc < 400) c = &tcs[ntc++];
  else { c = &tcs[0]; for (int i = 1; i < ntc; i++) if (tcs[i].age < c->age) c = &tcs[i]; SDL_DestroyTexture(c->tex); free(c->s); }
  TTF_Font *f = font(wgt, px); char buf[400]; strncpy(buf, s, 380); buf[380] = 0;
  int w = 0, h = 0;
  if (maxw > 0) {
    TTF_SizeUTF8(f, buf, &w, &h);
    while (w > maxw && strlen(buf) > 1) {   /* cut at a character boundary and add an ellipsis */
      size_t n = strlen(buf); if (n > 3 && !strcmp(buf + n - 3, "\xe2\x80\xa6")) n -= 3;
      do n--; while (n > 0 && (buf[n] & 0xC0) == 0x80);
      strcpy(buf + n, "\xe2\x80\xa6"); TTF_SizeUTF8(f, buf, &w, &h);
    }
  }
  SDL_Color white = { 255, 255, 255, 255 };
  SDL_Surface *su = buf[0] ? TTF_RenderUTF8_Blended(f, buf, white) : NULL;
  c->s = strdup(s); c->wgt = wgt; c->px = px; c->maxw = maxw; c->age = tcAge;
  if (su) { c->tex = SDL_CreateTextureFromSurface(ren, su); c->w = su->w; c->h = su->h; SDL_FreeSurface(su); SDL_SetTextureBlendMode(c->tex, SDL_BLENDMODE_BLEND); SDL_SetTextureScaleMode(c->tex, SDL_ScaleModeBest); }
  else { c->tex = NULL; c->w = 0; c->h = TTF_FontHeight(f); }
  return c;
}
/* draw text; align 0 left, 1 centre, 2 right of x; y is the top of the line box */
static int drawtext(const char *s, int wgt, int px, int maxw, float x, float y, int align, int r, int g, int b, int a) {
  TC *c = text(s, wgt, px, maxw); if (!c->tex) return 0;
  SDL_SetTextureColorMod(c->tex, r, g, b); SDL_SetTextureAlphaMod(c->tex, a);
  SDL_FRect d = { align == 1 ? x - c->w / 2.0f : align == 2 ? x - c->w : x, y, c->w, c->h }; SDL_RenderCopyF(ren, c->tex, NULL, &d);
  return c->w;
}
static int textw(const char *s, int wgt, int px, int maxw) { TC *c = text(s, wgt, px, maxw); return c->w; }

/* text with letter spacing (CSS letter-spacing in px), drawn glyph by glyph; returns the width */
static int drawtextLS(const char *s, int wgt, int px, float ls, float x, float y, int r, int g, int b, int a, int draw) {
  TTF_Font *f = font(wgt, px); float x0 = x;
  for (const unsigned char *p = (const unsigned char *)s; *p;) {
    int len = *p < 0x80 ? 1 : *p < 0xE0 ? 2 : *p < 0xF0 ? 3 : 4; char ch[5] = { 0 }; memcpy(ch, p, len); p += len;
    unsigned cp = len == 1 ? (unsigned char)ch[0] : len == 2 ? ((ch[0] & 0x1F) << 6) | (ch[1] & 0x3F) : len == 3 ? ((ch[0] & 0x0F) << 12) | ((ch[1] & 0x3F) << 6) | (ch[2] & 0x3F) : '?';
    int adv = 0; if (TTF_GlyphMetrics(f, (Uint16)cp, NULL, NULL, NULL, NULL, &adv) != 0) adv = px / 2;
    if (draw && ch[0] != ' ') drawtext(ch, wgt, px, 0, x, y, 0, r, g, b, a);
    x += adv + ls;
  }
  return (int)(x - x0);
}

/* vector icons (24 x 24 grid), stroked with round caps */
typedef struct { float x, y; } Pt;
static void stroke_seg(SDL_Surface *s, Pt a, Pt b, float wd, float scale) {
  int x0 = (int)fminf(a.x, b.x) - (int)(wd + 2), x1 = (int)fmaxf(a.x, b.x) + (int)(wd + 2), y0 = (int)fminf(a.y, b.y) - (int)(wd + 2), y1 = (int)fmaxf(a.y, b.y) + (int)(wd + 2);
  float vx = b.x - a.x, vy = b.y - a.y, l2 = vx * vx + vy * vy; (void)scale;
  for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < s->h; y++) for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < s->w; x++) {
    float px = x + 0.5f - a.x, py = y + 0.5f - a.y, u = l2 > 0 ? clampf((px * vx + py * vy) / l2, 0, 1) : 0;
    float dx = px - u * vx, dy = py - u * vy, d = sqrtf(dx * dx + dy * dy) - wd / 2;
    float cov = clampf(0.5f - d, 0, 1); Uint32 *p = (Uint32 *)((Uint8 *)s->pixels + y * s->pitch) + x;
    float old = (*p >> 24) / 255.0f, nw = cov + old * (1 - cov); *p = ((Uint32)(nw * 255 + 0.5f) << 24) | 0xffffff;
  }
}
static void poly(SDL_Surface *s, const float *p, int n, float wd, float sc, float off, int close) {
  for (int i = 0; i + 1 < n; i++) stroke_seg(s, (Pt){ p[i * 2] * sc + off, p[i * 2 + 1] * sc + off }, (Pt){ p[i * 2 + 2] * sc + off, p[i * 2 + 3] * sc + off }, wd, sc);
  if (close) stroke_seg(s, (Pt){ p[(n - 1) * 2] * sc + off, p[(n - 1) * 2 + 1] * sc + off }, (Pt){ p[0] * sc + off, p[1] * sc + off }, wd, sc);
}
static void arc(SDL_Surface *s, float cx, float cy, float r, float a0, float a1, float wd, float sc) {
  int n = 24; Pt prev = { cx + r * cosf(a0), cy + r * sinf(a0) };
  for (int i = 1; i <= n; i++) { float a = a0 + (a1 - a0) * i / n; Pt q = { cx + r * cosf(a), cy + r * sinf(a) };
    stroke_seg(s, (Pt){ prev.x * sc, prev.y * sc }, (Pt){ q.x * sc, q.y * sc }, wd, sc); prev = q; }
}
#define P(...) (const float[]){ __VA_ARGS__ }
static SDL_Surface *icon_surface(const char *name, int px) {
  SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, px, px, 32, SDL_PIXELFORMAT_ARGB8888); SDL_FillRect(s, NULL, 0x00ffffff);
  float sc = px / 24.0f, wd = 1.7f * sc;
  if (!strcmp(name, "inputs")) { poly(s, P(15, 4, 19, 4, 21, 6, 21, 18, 19, 20, 15, 20), 6, wd, sc, 0, 0); poly(s, P(10, 17, 15, 12, 10, 7), 3, wd, sc, 0, 0); poly(s, P(15, 12, 3, 12), 2, wd, sc, 0, 0); }
  else if (!strcmp(name, "settings")) {
    poly(s, P(4, 21, 4, 14), 2, wd, sc, 0, 0); poly(s, P(4, 10, 4, 3), 2, wd, sc, 0, 0); poly(s, P(12, 21, 12, 12), 2, wd, sc, 0, 0); poly(s, P(12, 8, 12, 3), 2, wd, sc, 0, 0);
    poly(s, P(20, 21, 20, 16), 2, wd, sc, 0, 0); poly(s, P(20, 12, 20, 3), 2, wd, sc, 0, 0);
    poly(s, P(1, 14, 7, 14), 2, wd, sc, 0, 0); poly(s, P(9, 8, 15, 8), 2, wd, sc, 0, 0); poly(s, P(17, 16, 23, 16), 2, wd, sc, 0, 0); }
  else if (!strcmp(name, "edit")) { poly(s, P(12, 20, 21, 20), 2, wd, sc, 0, 0); poly(s, P(16.5f, 3.5f, 20.5f, 7.5f, 7.5f, 19.5f, 3, 21, 4, 16.5f), 5, wd, sc, 0, 1); }
  else if (!strcmp(name, "tv")) { poly(s, P(4, 7, 20, 7, 22, 9, 22, 20, 20, 22, 4, 22, 2, 20, 2, 9), 8, wd, sc, 0, 1); poly(s, P(17, 2, 12, 7, 7, 2), 3, wd, sc, 0, 0); }
  else if (!strcmp(name, "hdmi")) { poly(s, P(4, 8, 20, 8, 20, 15, 17, 18, 7, 18, 4, 15), 6, wd, sc, 0, 1); poly(s, P(9, 11, 9, 14), 2, wd, sc, 0, 0); poly(s, P(12, 11, 12, 14), 2, wd, sc, 0, 0); poly(s, P(15, 11, 15, 14), 2, wd, sc, 0, 0); }
  else if (!strcmp(name, "restore")) { arc(s, 12, 12, 9, 3.5f, 11.0f, wd, sc); poly(s, P(3, 3, 3, 8, 8, 8), 3, wd, sc, 0, 0); }
  else { poly(s, P(3, 3, 10, 3, 10, 10, 3, 10), 4, wd, sc, 0, 1); poly(s, P(14, 3, 21, 3, 21, 10, 14, 10), 4, wd, sc, 0, 1); poly(s, P(14, 14, 21, 14, 21, 21, 14, 21), 4, wd, sc, 0, 1); poly(s, P(3, 14, 10, 14, 10, 21, 3, 21), 4, wd, sc, 0, 1); }
  return s;
}
typedef struct { char name[16]; int px; SDL_Texture *tex; } IC;
static IC ics[40]; static int nic;
static SDL_Texture *icon(const char *name, int px) {
  for (int i = 0; i < nic; i++) if (ics[i].px == px && !strcmp(ics[i].name, name)) return ics[i].tex;
  SDL_Surface *s = icon_surface(name, px); SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s); SDL_FreeSurface(s);
  SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); SDL_SetTextureScaleMode(t, SDL_ScaleModeBest);
  if (nic < 40) { snprintf(ics[nic].name, 16, "%s", name); ics[nic].px = px; ics[nic++].tex = t; }
  return t;
}
static void drawicon(const char *name, int px, float x, float y, int r, int g, int b, int a) {
  SDL_Texture *t = icon(name, px); SDL_SetTextureColorMod(t, r, g, b); SDL_SetTextureAlphaMod(t, a); SDL_FRect d = { x, y, px, px }; SDL_RenderCopyF(ren, t, NULL, &d);
}


/* LG's compositor takes Back (and Exit) for itself and asks "close the app?" unless the window asks for them: its
 * wl_webos_shell_surface has set_key_mask. SDL does not expose that object, so bind LG's shell and make one for the window. */
static FILE *lg;
struct wl_msg { const char *name, *signature; const void **types; };
struct wl_iface { const char *name; int version, method_count; const struct wl_msg *methods; int event_count; const struct wl_msg *events; };
static void *wlShell; static unsigned wlShellName, wlShellVer;
static void wlGlobal(void *d, void *reg, unsigned name, const char *iface, unsigned ver) { (void)d; (void)reg; if (!strcmp(iface, "wl_webos_shell")) { wlShellName = name; wlShellVer = ver; } }
static void wlGlobalRemove(void *d, void *reg, unsigned name) { (void)d; (void)reg; (void)name; }
static int methodIndex(const struct wl_iface *i, const char *name) { for (int k = 0; k < i->method_count; k++) { if (lg) fprintf(lg, "keymask:   %s request %d %s (%s)\n", i->name, k, i->methods[k].name, i->methods[k].signature); if (!strcmp(i->methods[k].name, name)) return k; } return -1; }
static void claimKeys(SDL_Window *w, unsigned mask) {
  SDL_SysWMinfo info; SDL_VERSION(&info.version);
  if (!SDL_GetWindowWMInfo(w, &info) || info.subsystem != SDL_SYSWM_WAYLAND) { if (lg) fprintf(lg, "keymask: no wayland window\n"); return; }
  void *hc = dlopen("libwayland-client.so.0", RTLD_NOW), *hw = dlopen("libwayland-webos-client.so.1", RTLD_NOW);
  if (!hc || !hw) { if (lg) fprintf(lg, "keymask: libraries missing\n"); return; }
  void *(*ctor)(void *, unsigned, const struct wl_iface *, ...) = (void *(*)(void *, unsigned, const struct wl_iface *, ...))dlsym(hc, "wl_proxy_marshal_constructor");
  void *(*ctorv)(void *, unsigned, const struct wl_iface *, unsigned, ...) = (void *(*)(void *, unsigned, const struct wl_iface *, unsigned, ...))dlsym(hc, "wl_proxy_marshal_constructor_versioned");
  void (*marshal)(void *, unsigned, ...) = (void (*)(void *, unsigned, ...))dlsym(hc, "wl_proxy_marshal");
  int (*addListener)(void *, void (**)(void), void *) = (int (*)(void *, void (**)(void), void *))dlsym(hc, "wl_proxy_add_listener");
  int (*roundtrip)(void *) = (int (*)(void *))dlsym(hc, "wl_display_roundtrip");
  const struct wl_iface *regI = (const struct wl_iface *)dlsym(hc, "wl_registry_interface"), *shI = (const struct wl_iface *)dlsym(hw, "wl_webos_shell_interface"), *ssI = (const struct wl_iface *)dlsym(hw, "wl_webos_shell_surface_interface");
  if (!ctor || !ctorv || !marshal || !addListener || !roundtrip || !regI || !shI || !ssI) { if (lg) fprintf(lg, "keymask: symbols missing\n"); return; }
  { const char *names[] = { "wl_webos_input_manager_interface", "wl_webos_seat_interface", "wl_webos_xinput_interface", "wl_webos_xinput_extension_interface", "wl_webos_foreign_interface" };
    for (int n = 0; n < 5; n++) { const struct wl_iface *x = (const struct wl_iface *)dlsym(hw, names[n]); if (!x) continue; for (int k = 0; k < x->method_count; k++) if (lg) fprintf(lg, "wl: %s request %d %s (%s)\n", x->name, k, x->methods[k].name, x->methods[k].signature); for (int k = 0; k < x->event_count; k++) if (lg) fprintf(lg, "wl: %s event %d %s (%s)\n", x->name, k, x->events[k].name, x->events[k].signature); } }
  void *display = info.info.wl.display, *surface = info.info.wl.surface;
  void *reg = ctor(display, 1, regI, NULL);   /* wl_display.get_registry */
  static void (*listener[2])(void) = { (void (*)(void))wlGlobal, (void (*)(void))wlGlobalRemove };
  addListener(reg, listener, NULL); roundtrip(display);
  if (!wlShellName) { if (lg) fprintf(lg, "keymask: no wl_webos_shell global\n"); return; }
  unsigned v = wlShellVer < (unsigned)shI->version ? wlShellVer : (unsigned)shI->version;
  wlShell = ctorv(reg, 0, shI, v, wlShellName, shI->name, v, NULL);   /* wl_registry.bind */
  int gi = methodIndex(shI, "get_shell_surface"); if (gi < 0 || !wlShell) { if (lg) fprintf(lg, "keymask: no get_shell_surface\n"); return; }
  void *ss = ctor(wlShell, gi, ssI, NULL, surface);
  int ki = methodIndex(ssI, "set_key_mask"); if (!ss || ki < 0) { if (lg) fprintf(lg, "keymask: no set_key_mask\n"); return; }
  marshal(ss, ki, mask); roundtrip(display);
  if (lg) fprintf(lg, "keymask: set 0x%x (shell %p, surface %p)\n", mask, ss, surface);
}

/* ---------------------------------------------------------------- model */
typedef struct { char *id, *title; SDL_Texture *tex; int tw, th, state; /* 0 none 1 loading 2 ok 3 failed */ int gc[3], bc[3]; int hasColor; } App;
typedef struct { int kind; /* 0 app 1 pill */ int idx; float x, y, w, h; float s; } Cell;
static App *apps; static int napps;
static J *cfg;          /* the service's config object */
static J *usage;
static char *lastSig;
static Cell *cells[64]; static int rowLen[64]; static int nrows;   /* rows[0] top buttons, rows[1] dock, rows[2..] grid */
static int fr = 1, fc = 0;
static const char *DEFAULT_DOCK[4] = { "netflix", "youtube.leanback.v4", "amazon", "com.disney.disneyplus-prod" };
static int ntop; static struct { char label[48]; char icon[20]; char action[16]; char app[96]; int fixed; } top[16];

static unsigned now_ms(void) { return SDL_GetTicks(); }
static int appIndex(const char *id) { for (int i = 0; i < napps; i++) if (!strcmp(apps[i].id, id)) return i; return -1; }
static int isHidden(const char *id) { J *h = jget(cfg, "hidden"); if (h && h->t == J_ARR) for (int i = 0; i < h->len; i++) if (jstr(h->v[i]) && !strcmp(h->v[i]->s, id)) return 1; return 0; }
static double use(const char *id) { J *u = jget(usage, id); return u && u->t == J_NUM ? u->n : 0; }
static int dockIds(int *out) {
  int n = 0; J *d = jget(cfg, "dock"); int custom = d && d->t == J_ARR && d->len;
  for (int i = 0; i < (custom ? d->len : 4) && n < 4; i++) { const char *id = custom ? jstr(d->v[i]) : DEFAULT_DOCK[i]; int a = id ? appIndex(id) : -1; if (a >= 0 && !isHidden(id)) out[n++] = a; }
  return n;
}
static int cmpApps(const void *a, const void *b) {
  const App *x = &apps[*(const int *)a], *y = &apps[*(const int *)b]; J *so = jget(cfg, "sort");
  if (!(so && jstr(so) && !strcmp(so->s, "alpha"))) { double d = use(y->id) - use(x->id); if (fabs(d) > 1e-9) return d > 0 ? 1 : -1; }
  return strcasecmp(x->title, y->title);
}
static void readTop(void) {
  ntop = 0; J *tb = jget(cfg, "topbar");
  if (tb && tb->t == J_ARR) {
    for (int i = 0; i < tb->len && ntop < 15; i++) { J *o = tb->v[i]; if (!o || o->t != J_OBJ) continue;
      const char *l = jstr(jget(o, "label")), *ic = jstr(jget(o, "icon")), *ac = jstr(jget(o, "action")), *ap = jstr(jget(o, "app"));
      snprintf(top[ntop].label, 48, "%s", l ? l : ""); snprintf(top[ntop].icon, 20, "%s", ic ? ic : "grid"); snprintf(top[ntop].action, 16, "%s", ac ? ac : ""); snprintf(top[ntop].app, 96, "%s", ap ? ap : ""); top[ntop++].fixed = 0; }
  } else {
    snprintf(top[0].label, 48, "Inputs"); snprintf(top[0].icon, 20, "inputs"); snprintf(top[0].action, 16, "inputs"); top[0].app[0] = 0; top[0].fixed = 0;
    snprintf(top[1].label, 48, "Live TV"); snprintf(top[1].icon, 20, "tv"); snprintf(top[1].action, 16, "live"); top[1].app[0] = 0; top[1].fixed = 0;
    snprintf(top[2].label, 48, "Settings"); snprintf(top[2].icon, 20, "settings"); top[2].action[0] = 0; snprintf(top[2].app, 96, "com.palm.app.settings"); top[2].fixed = 0; ntop = 3;
  }
  snprintf(top[ntop].label, 48, "Customise"); snprintf(top[ntop].icon, 20, "edit"); snprintf(top[ntop].action, 16, "customize"); top[ntop].app[0] = 0; top[ntop++].fixed = 1;
}
static const char *barLabel(int i) {
  if (!strcmp(top[i].action, "inputs")) return t("Inputs"); if (!strcmp(top[i].app, "com.palm.app.settings")) return t("Settings");
  if (!strcmp(top[i].action, "customize")) return t("Customise"); if (!strcmp(top[i].action, "live")) return t("Live TV"); return top[i].label;
}
/* positions of everything, as the web launcher lays it out */
static int gridCount;
static void layout(void) {
  for (int r = 0; r < nrows; r++) { free(cells[r]); cells[r] = NULL; } nrows = 0;
  /* row 0: top buttons, right-aligned */
  readTop();
  Cell *c0 = calloc(ntop, sizeof(Cell)); float x = W - 96;
  for (int i = ntop - 1; i >= 0; i--) { int w = 24 + 30 + 14 + textw(barLabel(i), 0, 26, 420) + 30; x -= w; c0[i] = (Cell){ 1, i, x, 60, w, 64, 1 }; x -= 16; }
  cells[0] = c0; rowLen[0] = ntop; nrows = 1;
  int dk[4], nd = dockIds(dk), *rest = malloc(napps * sizeof(int)), nr = 0;
  for (int i = 0; i < napps; i++) { int in = 0; for (int j = 0; j < nd; j++) if (dk[j] == i) in = 1; if (!in && !isHidden(apps[i].id)) rest[nr++] = i; }
  qsort(rest, nr, sizeof(int), cmpApps);
  { int u = appIndex("__undo"); if (u >= 0) { int k = 0; for (int i = 0; i < nr; i++) if (rest[i] != u) rest[k++] = rest[i]; nr = k; rest[nr++] = u; } }
  if (nd) { Cell *c = calloc(nd, sizeof(Cell)); for (int i = 0; i < nd; i++) c[i] = (Cell){ 0, dk[i], 96 + i * 440, 360, 408, 230, 1 }; cells[1] = c; rowLen[1] = nd; }
  else { cells[1] = calloc(ntop, sizeof(Cell)); memcpy(cells[1], c0, ntop * sizeof(Cell)); rowLen[1] = ntop; }
  nrows = 2;
  for (int i = 0; i < nr; i += 10) { int n = nr - i < 10 ? nr - i : 10; Cell *c = calloc(n, sizeof(Cell));
    for (int j = 0; j < n; j++) c[j] = (Cell){ 0, rest[i + j], 96 + j * 175, 650 + (i / 10) * 177, 151, 151, 1 };
    cells[nrows] = c; rowLen[nrows++] = n; }
  gridCount = nr; free(rest);
  if (fr >= nrows) fr = nrows - 1; if (fc >= rowLen[fr]) fc = rowLen[fr] - 1;
}

/* ---------------------------------------------------------------- worker thread: service calls and icon decoding */
typedef struct Job { int type; char *a, *b; struct Job *next; } Job;   /* 1 apps, 2 icon(a), 3 launch(a,params b), 4 save config(a), 5 inputs */
static Job *jobs, *jobsTail; static SDL_mutex *jm; static SDL_sem *js; static Uint32 EV;
static void post(int type, void *p1, void *p2) { SDL_Event e = { 0 }; e.type = EV; e.user.code = type; e.user.data1 = p1; e.user.data2 = p2; SDL_PushEvent(&e); }
static void enqueue(int type, const char *a, const char *b) {
  Job *j = calloc(1, sizeof *j); j->type = type; j->a = a ? strdup(a) : NULL; j->b = b ? strdup(b) : NULL;
  SDL_LockMutex(jm); if (jobsTail) jobsTail->next = j; else jobs = j; jobsTail = j; SDL_UnlockMutex(jm); SDL_SemPost(js);
}
typedef struct { char *id; SDL_Surface *s; int gc[3], bc[3]; int has; } IconRes;
static void rgb2hsl(float R, float G, float B, float *h, float *s, float *l) {
  R /= 255; G /= 255; B /= 255; float mx = fmaxf(R, fmaxf(G, B)), mn = fminf(R, fminf(G, B)); *l = (mx + mn) / 2; float d = mx - mn; *h = 0; *s = 0;
  if (d > 0) { *s = d / (1 - fabsf(2 * *l - 1)); *h = mx == R ? (G - B) / d + (G < B ? 6 : 0) : mx == G ? (B - R) / d + 2 : (R - G) / d + 4; *h *= 60; }
}
static void hsl2rgb(float h, float s, float l, int *o) {
  float a = s * fminf(l, 1 - l);
  for (int i = 0; i < 3; i++) { int n = i == 0 ? 0 : i == 1 ? 8 : 4; float k = fmodf(n + h / 30, 12); o[i] = (int)lroundf(255 * (l - a * fmaxf(-1, fminf(fminf(k - 3, 9 - k), 1)))); }
}
static int hashHue(const char *id) { int h = 0; for (; *id; id++) h = (h * 31 + (unsigned char)*id) % 360; return h; }
/* the glow colour (vibrant) and the tile background (the icon's own edge colour), like the web launcher */
static void iconColors(SDL_Surface *s, const char *id, int *gc, int *bc) {
  const int N = 16; float R = 0, G = 0, B = 0, Wt = 0, eR = 0, eG = 0, eB = 0; int eN = 0, ring = 0, haveGlow = 0, haveEdge = 0, glow[3], edge[3] = { 0 };
  if (s) for (int y = 0; y < N; y++) for (int x = 0; x < N; x++) {
    int sx = (x * s->w) / N + s->w / (2 * N), sy = (y * s->h) / N + s->h / (2 * N); Uint32 p = *(Uint32 *)((Uint8 *)s->pixels + sy * s->pitch + sx * 4);
    float a = ((p >> 24) & 255) / 255.0f, r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
    if (y == 0 || y == N - 1 || x == 0 || x == N - 1) { ring++; if (a > .8f) { eR += r; eG += g; eB += b; eN++; } }
    if (a < .4f) continue; float h, sa, l; rgb2hsl(r, g, b, &h, &sa, &l); float w = a * (.15f + sa * sa); R += r * w; G += g * w; B += b * w; Wt += w;
  }
  if (eN > ring * .5f) { edge[0] = (int)lroundf(eR / eN); edge[1] = (int)lroundf(eG / eN); edge[2] = (int)lroundf(eB / eN); haveEdge = 1; }
  if (Wt > 0.5f) { float h, sa, l; rgb2hsl(R / Wt, G / Wt, B / Wt, &h, &sa, &l); if (sa > .22f) { hsl2rgb(h, fmaxf(.5f, fminf(sa, .8f)), fmaxf(.36f, fminf(l, .52f)), glow); haveGlow = 1; } }
  int fb[3]; hsl2rgb(hashHue(id), .42f, .42f, fb);
  if (!haveGlow) { float h, sa, l; if (haveEdge) rgb2hsl(edge[0], edge[1], edge[2], &h, &sa, &l); if (haveEdge && sa > .3f) memcpy(glow, edge, sizeof glow); else memcpy(glow, fb, sizeof glow); }
  memcpy(gc, glow, sizeof glow);
  if (haveEdge) memcpy(bc, edge, sizeof edge); else { float h, sa, l; rgb2hsl(glow[0], glow[1], glow[2], &h, &sa, &l); hsl2rgb(h, .4f, .2f, bc); }
}
/* the last app list and the icons, kept in /tmp so the next start paints at once */
#define CACHE "/tmp/cytv-native-cache"
static unsigned char *readFile(const char *path, int *n, time_t *mtime) {
  FILE *f = fopen(path, "rb"); if (!f) return NULL; struct stat st; if (mtime && fstat(fileno(f), &st) == 0) *mtime = st.st_mtime;
  fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET); if (len <= 0 || len > 8 * 1024 * 1024) { fclose(f); return NULL; }
  unsigned char *b = malloc(len + 1); if (fread(b, 1, len, f) != (size_t)len) { free(b); fclose(f); return NULL; } fclose(f); b[len] = 0; if (n) *n = (int)len; return b;
}
static void writeFile(const char *path, const void *d, int n) { FILE *f = fopen(path, "wb"); if (f) { fwrite(d, 1, n, f); fclose(f); } }
static int worker(void *arg) {
  (void)arg;
  for (;;) {
    SDL_SemWait(js); SDL_LockMutex(jm); Job *j = jobs; if (j) { jobs = j->next; if (!jobs) jobsTail = NULL; } SDL_UnlockMutex(jm); if (!j) continue;
    if (j->type == 1) { int n = 0; unsigned char *r = http("GET", "/api/apps", NULL, &n); if (r) writeFile(CACHE "/apps.json", r, n); post(1, r, NULL); }
    else if (j->type == 2) {
      char path[400], cp[400]; int n = 0; snprintf(path, sizeof path, "/icon/%s", j->a); snprintf(cp, sizeof cp, CACHE "/%s.png", j->a);
      time_t mt = 0; unsigned char *r = readFile(cp, &n, &mt); if (r && time(NULL) - mt > 12 * 3600) { free(r); r = NULL; }
      if (!r) { r = http("GET", path, NULL, &n); if (r) writeFile(cp, r, n); }
      IconRes *ir = calloc(1, sizeof *ir); ir->id = strdup(j->a);
      if (r) { SDL_RWops *rw = SDL_RWFromConstMem(r, n); SDL_Surface *s = IMG_Load_RW(rw, 1);
        if (s) { SDL_Surface *c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0); SDL_FreeSurface(s); ir->s = c; iconColors(c, j->a, ir->gc, ir->bc); ir->has = 1; } free(r); }
      if (!ir->s) iconColors(NULL, j->a, ir->gc, ir->bc);
      post(2, ir, NULL);
    }
    else if (j->type == 3) {
      Buf b = { 0 }; J *o = jnew(J_OBJ); jset(o, "id", jstrn(j->a)); if (j->b) { J *p = jparse(j->b); jset(o, "params", p); } jdump(o, &b); jfree(o);
      unsigned char *r = http("POST", "/api/launch", b.b, NULL); int bad = 1;
      if (r) { J *x = jparse((char *)r); J *rv = jget(x, "returnValue"); bad = rv && rv->t == J_BOOL && !rv->n; jfree(x); free(r); }
      free(b.b); if (bad) post(3, strdup(j->a), NULL);
    }
    else if (j->type == 4) { unsigned char *r = http("POST", "/api/config", j->a, NULL); free(r); }
    else if (j->type == 6) { unsigned char *r = http("POST", "/api/undo", "{}", NULL); free(r); }
    else if (j->type == 5) { unsigned char *r = http("GET", "/api/inputs", NULL, NULL); post(5, r, NULL); }
    free(j->a); free(j->b); free(j);
  }
}

/* ---------------------------------------------------------------- state: glow, toast, menus */
static float gl[2][3]; static int glSide; static float glA[2]; static int glTarget[3] = { 74, 84, 112 }, glCur[3] = { -1 }; static unsigned glowAt;
static float scrollY, scrollT;
static char toastS[200]; static unsigned toastUntil;
static void toast(const char *s) { snprintf(toastS, sizeof toastS, "%s", s); toastUntil = now_ms() + 4000; }
typedef struct { char *label, *sub; int dim; int icon; int stay; int fn; char *arg; int n; } Item;
typedef struct { char *title; Item *items; int n, idx; } Menu;
static Menu stack[8]; static int nstack;
static SDL_Texture *glowTex;
static int dirty = 1, visible = 1, running = 1, animating = 0, resident = 0;   /* resident: stay in memory when another app takes over (when this is LG's Home) */
static unsigned lastKeyAt, lastMouseMove; static int mx = -1, my = -1, lastMX = -1, lastMY = -1;

enum { F_NONE, F_LAUNCH, F_OPENMENU, F_SETDOCK_ADD, F_MOVE, F_REMOVE_DOCK, F_REPLACE, F_HIDE, F_UNHIDE, F_RMBTN, F_ADDBTN_PRESET, F_ADDBTN_APP, F_SORT, F_CLOCK, F_RESET, F_SUBMENU, F_LAUNCH_LIVE, F_TOPMENU };
static void closeMenus(void) { for (int i = 0; i < nstack; i++) { for (int j = 0; j < stack[i].n; j++) { free(stack[i].items[j].label); free(stack[i].items[j].sub); free(stack[i].items[j].arg); } free(stack[i].items); free(stack[i].title); } nstack = 0; dirty = 1; }
static void backMenu(void) {
  if (!nstack) return; Menu *m = &stack[--nstack];
  for (int j = 0; j < m->n; j++) { free(m->items[j].label); free(m->items[j].sub); free(m->items[j].arg); } free(m->items); free(m->title); dirty = 1;
}
static Menu *pushMenu(const char *title) { Menu *m = &stack[nstack++]; memset(m, 0, sizeof *m); m->title = strdup(title); dirty = 1; return m; }
static Item *addItem(Menu *m, const char *label, const char *sub, int fn, const char *arg, int n, int stay) {
  m->items = realloc(m->items, (m->n + 1) * sizeof(Item)); Item *it = &m->items[m->n++]; memset(it, 0, sizeof *it);
  it->label = strdup(label); it->sub = sub ? strdup(sub) : NULL; it->fn = fn; it->arg = arg ? strdup(arg) : NULL; it->n = n; it->stay = stay; return it;
}

static void saveCfg(void) {
  Buf b = { 0 }; J *o = jnew(J_OBJ); const char *keys[] = { "dock", "hidden", "sort", "clock", "topbar" };
  for (int i = 0; i < 5; i++) { J *v = jget(cfg, keys[i]); if (v) { Buf t = { 0 }; jdump(v, &t); jset(o, keys[i], jparse(t.b)); free(t.b); } }
  jdump(o, &b); jfree(o); enqueue(4, b.b, NULL); free(b.b); layout(); dirty = 1;
}
static J *arrOf(const char *key) { J *a = jget(cfg, key); if (!a || a->t != J_ARR) { a = jnew(J_ARR); jset(cfg, key, a); } return a; }
static void setDock(const int *idx, int n) { J *a = jnew(J_ARR); for (int i = 0; i < n && i < 4; i++) jpush(a, jstrn(apps[idx[i]].id)); jset(cfg, "dock", a); saveCfg(); }
static void launch(const char *id, const char *params) {
  if (lg) fprintf(lg, "launch %s\n", id);
  if (!strcmp(id, "__live")) { enqueue(3, "com.cleanyourtv.live", "{\"live\":true}"); return; }   /* Live TV is the web launcher's, installed as its own app */
  enqueue(3, id, params);
}

/* ---------------------------------------------------------------- menus */
static void inputsMenu(void) { enqueue(5, NULL, NULL); }
static void appMenu(int a) {
  int dk[4], nd = dockIds(dk), pos = -1; for (int i = 0; i < nd; i++) if (dk[i] == a) pos = i;
  Menu *m = pushMenu(apps[a].title); char s[16];
  snprintf(s, sizeof s, "%d", a); addItem(m, t("Open"), NULL, F_LAUNCH, apps[a].id, a, 0);
  if (pos >= 0) {
    if (pos > 0) addItem(m, t("Move left in dock"), NULL, F_MOVE, NULL, a * 10 + 0, 0);
    if (pos < nd - 1) addItem(m, t("Move right in dock"), NULL, F_MOVE, NULL, a * 10 + 1, 0);
    addItem(m, t("Remove from dock"), NULL, F_REMOVE_DOCK, NULL, a, 0);
  } else if (nd < 4) addItem(m, t("Add to dock"), NULL, F_SETDOCK_ADD, NULL, a, 0);
  else addItem(m, t("Add to dock..."), NULL, F_REPLACE, NULL, a, 1);
  addItem(m, t("Hide app"), NULL, F_HIDE, NULL, a, 0);
}
static void sortApps(int *idx, int n) { int i, j; for (i = 1; i < n; i++) { int v = idx[i]; for (j = i - 1; j >= 0 && strcasecmp(apps[idx[j]].title, apps[v].title) > 0; j--) idx[j + 1] = idx[j]; idx[j + 1] = v; } }
static void customizeMenu(void) {
  Menu *m = pushMenu(t("Customise")); J *so = jget(cfg, "sort"), *ck = jget(cfg, "clock");
  addItem(m, "Dock", t("hold OK on an app"), F_NONE, NULL, 1, 1);
  addItem(m, t("Top buttons"), NULL, F_TOPMENU, NULL, 0, 1);
  addItem(m, t("Hidden apps"), NULL, F_SUBMENU, NULL, 0, 1);
  addItem(m, t("App order"), so && jstr(so) && !strcmp(so->s, "alpha") ? t("A to Z") : t("Most used first"), F_SORT, NULL, 0, 0);
  addItem(m, t("Clock"), ck && jstr(ck) && !strcmp(ck->s, "24") ? t("24-hour") : ck && jstr(ck) && !strcmp(ck->s, "12") ? t("12-hour") : t("Automatic"), F_CLOCK, NULL, 0, 0);
  addItem(m, t("Reset customisation"), NULL, F_RESET, NULL, 0, 0);
}
static void act(Cell *c) {
  if (c->kind == 0) { if (!strcmp(apps[c->idx].id, "__undo")) { toast(t("Switching to LG Home: the TV restarts...")); enqueue(6, NULL, NULL); return; } launch(apps[c->idx].id, NULL); return; }
  if (!strcmp(top[c->idx].action, "customize")) { customizeMenu(); return; }
  if (!strcmp(top[c->idx].action, "live")) { launch("__live", NULL); return; }
  if (!strcmp(top[c->idx].action, "inputs")) { inputsMenu(); return; }
  if (top[c->idx].app[0]) launch(top[c->idx].app, NULL);
}
static void options(Cell *c) {
  if (c->kind == 0) appMenu(c->idx);
  else if (!top[c->idx].fixed) { Menu *m = pushMenu(barLabel(c->idx)); addItem(m, t("Remove this button"), NULL, F_RMBTN, NULL, c->idx, 0); }
}
static void removeBtn(int i) { J *tb = jget(cfg, "topbar"); if (!tb || tb->t != J_ARR) { /* materialise the defaults first */
    tb = jnew(J_ARR); for (int k = 0; k < ntop; k++) if (!top[k].fixed) { J *o = jnew(J_OBJ); jset(o, "label", jstrn(top[k].label)); jset(o, "icon", jstrn(top[k].icon)); if (top[k].action[0]) jset(o, "action", jstrn(top[k].action)); if (top[k].app[0]) jset(o, "app", jstrn(top[k].app)); jpush(tb, o); } jset(cfg, "topbar", tb); }
  if (i >= 0 && i < tb->len) { jfree(tb->v[i]); memmove(tb->v + i, tb->v + i + 1, (tb->len - i - 1) * sizeof *tb->v); tb->len--; } saveCfg(); }
static void addBtn(const char *label, const char *icon, const char *action, const char *app) {
  removeBtn(-1); J *tb = jget(cfg, "topbar"); J *o = jnew(J_OBJ); jset(o, "label", jstrn(label)); jset(o, "icon", jstrn(icon)); if (action) jset(o, "action", jstrn(action)); if (app) jset(o, "app", jstrn(app)); jpush(tb, o); saveCfg(); }

static void pickItem(void) {
  if (lg) fprintf(lg, "pick %d\n", nstack ? stack[nstack - 1].idx : -1);
  Menu *m = &stack[nstack - 1]; if (m->idx >= m->n) return; Item it = m->items[m->idx]; int n0 = nstack, dk[4], nd = dockIds(dk);
  char *arg = it.arg ? strdup(it.arg) : NULL; int n = it.n, stay = it.stay;   /* the menu may be rebuilt below */
  switch (it.fn) {
    case F_LAUNCH: launch(arg, NULL); break;
    case F_MOVE: { int a = n / 10, dir = n % 10, pos = -1; for (int i = 0; i < nd; i++) if (dk[i] == a) pos = i;
      if (pos >= 0) { int to = pos + (dir ? 1 : -1); if (to >= 0 && to < nd) { int tmp = dk[to]; dk[to] = dk[pos]; dk[pos] = tmp; setDock(dk, nd); } } break; }
    case F_REMOVE_DOCK: { int o[4], k = 0; for (int i = 0; i < nd; i++) if (dk[i] != n) o[k++] = dk[i]; setDock(o, k); break; }
    case F_SETDOCK_ADD: { dk[nd] = n; setDock(dk, nd + 1); break; }
    case F_REPLACE: { Menu *s = pushMenu(t("Replace which app?")); for (int i = 0; i < nd; i++) addItem(s, apps[dk[i]].title, NULL, F_REPLACE + 100, NULL, i * 1000 + n, 0); break; }
    case F_REPLACE + 100: { dk[n / 1000] = n % 1000; setDock(dk, nd); break; }
    case F_HIDE: { J *h = arrOf("hidden"); jpush(h, jstrn(apps[n].id)); int o[4], k = 0; for (int i = 0; i < nd; i++) if (dk[i] != n) o[k++] = dk[i]; if (k != nd) setDock(o, k); else saveCfg(); break; }
    case F_UNHIDE: { J *h = arrOf("hidden"); for (int i = 0; i < h->len; i++) if (jstr(h->v[i]) && !strcmp(h->v[i]->s, arg)) { jfree(h->v[i]); memmove(h->v + i, h->v + i + 1, (h->len - i - 1) * sizeof *h->v); h->len--; break; } saveCfg(); break; }
    case F_SUBMENU: { J *h = jget(cfg, "hidden"); int any = 0; Menu *s = NULL;
      if (h && h->t == J_ARR) for (int i = 0; i < h->len; i++) { const char *id = jstr(h->v[i]); int a = id ? appIndex(id) : -1; if (a < 0) continue; if (!s) s = pushMenu(t("Hidden apps (OK to show again)")); addItem(s, apps[a].title, NULL, F_UNHIDE, id, 0, 0); any = 1; }
      if (!any) toast(t("No hidden apps")); break; }
    case F_TOPMENU: { Menu *s = pushMenu(t("Top buttons")); for (int i = 0; i < ntop; i++) if (!top[i].fixed) addItem(s, barLabel(i), t("OK to remove"), F_RMBTN, NULL, i, 0);
      addItem(s, t("Add a button..."), NULL, F_ADDBTN_PRESET, NULL, 0, 1); break; }
    case F_RMBTN: removeBtn(n); break;
    case F_ADDBTN_PRESET: { Menu *s = pushMenu(t("Add a button")); addItem(s, t("Inputs"), NULL, F_ADDBTN_PRESET + 100, NULL, 0, 0); addItem(s, t("Settings"), NULL, F_ADDBTN_PRESET + 100, NULL, 1, 0); addItem(s, t("Live TV"), NULL, F_ADDBTN_PRESET + 100, NULL, 2, 0); addItem(s, t("An app..."), NULL, F_ADDBTN_APP, NULL, 0, 1); break; }
    case F_ADDBTN_PRESET + 100: if (n == 0) addBtn("Inputs", "inputs", "inputs", NULL); else if (n == 1) addBtn("Settings", "settings", NULL, "com.palm.app.settings"); else addBtn("Live TV", "tv", "live", NULL); break;
    case F_ADDBTN_APP: { Menu *s = pushMenu(t("Pick an app")); int *ix = malloc(napps * sizeof(int)); for (int i = 0; i < napps; i++) ix[i] = i; sortApps(ix, napps);
      for (int i = 0; i < napps; i++) addItem(s, apps[ix[i]].title, NULL, F_ADDBTN_APP + 100, apps[ix[i]].id, 0, 0); free(ix); break; }
    case F_ADDBTN_APP + 100: { int a = appIndex(arg); if (a >= 0) addBtn(apps[a].title, "grid", NULL, arg); break; }
    case F_SORT: { J *so = jget(cfg, "sort"); jset(cfg, "sort", jstrn(so && jstr(so) && !strcmp(so->s, "alpha") ? "usage" : "alpha")); saveCfg(); break; }
    case F_CLOCK: { J *ck = jget(cfg, "clock"); const char *c = ck && jstr(ck) ? ck->s : "auto"; jset(cfg, "clock", jstrn(!strcmp(c, "24") ? "12" : !strcmp(c, "12") ? "auto" : "24")); saveCfg(); break; }
    case F_RESET: { jfree(cfg); cfg = jnew(J_OBJ); saveCfg(); break; }
    case F_LAUNCH_LIVE: launch("__live", NULL); break;
    default: break;
  }
  free(arg);
  if (nstack == n0 && !stay) closeMenus();
}

/* ---------------------------------------------------------------- data in */
static void applyApps(unsigned char *raw) {
  if (!raw) { toast(t("Helper not reachable, retrying...")); dirty = 1; return; }
  J *x = jparse((char *)raw); J *al = jget(x, "apps"); if (!al || al->t != J_ARR) { jfree(x); free(raw); return; }
  Buf sig = { 0 }; for (int i = 0; i < al->len; i++) { bstr(&sig, jstr(jget(al->v[i], "id")) ? jget(al->v[i], "id")->s : ""); bstr(&sig, ","); }
  J *c = jget(x, "config"); if (c) jdump(c, &sig); J *u = jget(x, "usage"); if (u) { /* rank only */ for (int i = 0; i < u->len; i++) { bstr(&sig, u->k[i]); bstr(&sig, ";"); } }
  if (lastSig && !strcmp(lastSig, sig.b)) { jfree(x); free(raw); free(sig.b); return; }
  free(lastSig); lastSig = sig.b;
  /* keep icons we already have */
  App *old = apps; int nold = napps; apps = calloc(al->len + 2, sizeof(App)); napps = 0;
  for (int i = 0; i < al->len; i++) {
    const char *id = jstr(jget(al->v[i], "id")), *ti = jstr(jget(al->v[i], "title")); if (!id) continue;
    App *a = &apps[napps]; a->id = strdup(id); a->title = strdup(ti ? ti : id);
    for (int k = 0; k < nold; k++) if (old[k].id && !strcmp(old[k].id, id)) { a->tex = old[k].tex; a->tw = old[k].tw; a->th = old[k].th; a->state = old[k].state; memcpy(a->gc, old[k].gc, sizeof a->gc); memcpy(a->bc, old[k].bc, sizeof a->bc); a->hasColor = old[k].hasColor; old[k].tex = NULL; }
    napps++;
  }
  { App *a = &apps[napps]; a->id = strdup("__live"); a->title = strdup(t("Live TV")); a->state = 2;
    int hh[3]; hsl2rgb(355, .8f, .62f, hh); memcpy(a->gc, (int[]){ 239, 79, 95 }, sizeof a->gc); memcpy(a->bc, (int[]){ 239, 79, 95 }, sizeof a->bc); a->hasColor = 1;
    for (int k = 0; k < nold; k++) if (old[k].id && !strcmp(old[k].id, "__live")) { a->tex = old[k].tex; a->tw = old[k].tw; a->th = old[k].th; old[k].tex = NULL; }
    napps++; }
  { App *a = &apps[napps]; a->id = strdup("__undo"); a->title = strdup("LG Home"); a->state = 5; napps++; }
  for (int k = 0; k < nold; k++) { free(old[k].id); free(old[k].title); if (old[k].tex) SDL_DestroyTexture(old[k].tex); } free(old);
  jfree(cfg); cfg = jnew(J_OBJ);
  if (c) { Buf t2 = { 0 }; jdump(c, &t2); jfree(cfg); cfg = jparse(t2.b); free(t2.b); }
  jfree(usage); { Buf t2 = { 0 }; if (u) jdump(u, &t2); else bstr(&t2, "{}"); usage = jparse(t2.b); free(t2.b); }
  jfree(x); free(raw);
  /* the Live TV tile's icon: the same red tile with a play button as the web launcher */
  for (int i = 0; i < napps; i++) if (!strcmp(apps[i].id, "__live") && !apps[i].tex) {
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, 130, 130, 32, SDL_PIXELFORMAT_ARGB8888); SDL_FillRect(s, NULL, 0xffef4f5f);
    for (int y = 0; y < 130; y++) for (int xx = 0; xx < 130; xx++) *((Uint32 *)((Uint8 *)s->pixels + y * s->pitch) + xx) = 0xffef4f5f;
    SDL_Surface *m = SDL_CreateRGBSurfaceWithFormat(0, 130, 130, 32, SDL_PIXELFORMAT_ARGB8888); SDL_FillRect(m, NULL, 0x00ffffff);
    { float a[] = { 38.5f, 42.5f, 91.5f, 42.5f, 91.5f, 83.5f, 38.5f, 83.5f }; poly(m, a, 4, 8, 1, 0, 1); }
    { float a[] = { 58, 52, 58, 74, 77, 63 }; poly(m, a, 3, 6, 1, 0, 1); for (int y = 53; y < 74; y++) for (int xx = 59; xx < 76; xx++) if (xx - 58 < (y < 63 ? (y - 52) * 19 / 11 : (74 - y) * 19 / 11)) stroke_seg(m, (Pt){ xx, y }, (Pt){ xx, y }, 1.2f, 1); }
    { float a[] = { 48, 104, 82, 104 }; poly(m, a, 2, 8, 1, 0, 0); }
    SDL_BlitSurface(m, NULL, s, NULL); SDL_FreeSurface(m);
    apps[i].tex = SDL_CreateTextureFromSurface(ren, s); apps[i].tw = apps[i].th = 130; SDL_FreeSurface(s); SDL_SetTextureScaleMode(apps[i].tex, SDL_ScaleModeBest); }
  for (int i = 0; i < napps; i++) if (strcmp(apps[i].id, "__live") && apps[i].state == 0) { apps[i].state = 1; enqueue(2, apps[i].id, NULL); }
  layout(); dirty = 1;
}
static void applyIcon(IconRes *ir) {
  int i = appIndex(ir->id);
  if (i >= 0) {
    App *a = &apps[i]; memcpy(a->gc, ir->gc, sizeof a->gc); memcpy(a->bc, ir->bc, sizeof a->bc); a->hasColor = 1;
    if (ir->s) { a->tex = SDL_CreateTextureFromSurface(ren, ir->s); SDL_SetTextureScaleMode(a->tex, SDL_ScaleModeBest); a->tw = ir->s->w; a->th = ir->s->h; a->state = 2; } else a->state = 3;
    if (nrows && cells[fr][fc].kind == 0 && cells[fr][fc].idx == i) { memcpy(glTarget, a->gc, sizeof glTarget); glowAt = now_ms() + 30; }
    dirty = 1;
  }
  if (ir->s) SDL_FreeSurface(ir->s); free(ir->id); free(ir);
}
static void applyInputs(unsigned char *raw) {
  if (!raw) { toast(t("Could not open")); return; }
  J *x = jparse((char *)raw); J *dv = jget(x, "devices"); Menu *m = pushMenu(t("Inputs"));
  if (dv && dv->t == J_ARR) for (int i = 0; i < dv->len; i++) {
    J *d = dv->v[i]; const char *app = jstr(jget(d, "appId")), *label = jstr(jget(d, "label")); if (!app) continue;
    char port[24], sub[120]; J *pn = jget(d, "port"); snprintf(port, sizeof port, "HDMI %d", pn ? (int)pn->n : 0); sub[0] = 0;
    if (label && strcmp(label, port)) snprintf(sub, sizeof sub, "%s", port);
    J *pl = jget(d, "hdmiPlugIn"), *ac = jget(d, "activate"); int plugged = pl && pl->n;
    if (plugged) { if (sub[0]) strcat(sub, " \xc2\xb7 "); strcat(sub, t("Connected")); }
    if (ac && ac->n) { if (sub[0]) strcat(sub, " \xc2\xb7 "); strcat(sub, t("Active")); }
    Item *it = addItem(m, label ? label : port, sub[0] ? sub : NULL, F_LAUNCH, app, 0, 0); it->dim = !plugged; it->icon = 1;
  }
  jfree(x); free(raw);
}

/* ---------------------------------------------------------------- rendering */
static void fmtDate(char *o, size_t n, struct tm *tm) {
  static const char *F[5] = { "%s, %d %s", "%s %d %s", "%s, %d. %s", "%s %d %s", "%s, %d de %s" };
  snprintf(o, n, F[lang], DAYS[lang][tm->tm_wday], tm->tm_mday, MONTHS[lang][tm->tm_mon]);
}
static void fmtTime(char *o, size_t n, struct tm *tm) {
  J *ck = jget(cfg, "clock"); const char *c = ck && jstr(ck) ? ck->s : "auto"; int h12 = !strcmp(c, "12") || (!strcmp(c, "auto") && lang == 0 && 0);
  if (h12) snprintf(o, n, "%d:%02d %s", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12, tm->tm_min, tm->tm_hour < 12 ? "AM" : "PM"); else snprintf(o, n, "%02d:%02d", tm->tm_hour, tm->tm_min);
}
static void drawTile(Cell *c, int focused, float y, int dock) {
  App *a = &apps[c->idx]; float sc = c->s, w = c->w * sc, h = c->h * sc, x = c->x - (w - c->w) / 2, yy = y - (h - c->h) / 2;
  int rad = dock ? 36 : 34;
  int bcol[3] = { a->hasColor ? a->bc[0] : 20, a->hasColor ? a->bc[1] : 22, a->hasColor ? a->bc[2] : 27 };
  if (focused) {   /* shadow, then the tile, then a white ring */
    float k = clampf((sc - 1) / (dock ? 0.05f : 0.12f), 0, 1);
    SDL_Texture *sh = rr((int)c->w, (int)c->h, rad, 0, 60); SDL_SetTextureColorMod(sh, 0, 0, 0); SDL_SetTextureAlphaMod(sh, (int)(150 * k));
    SDL_FRect d = { x - 60 * sc + 0, yy - 60 * sc + 24 * k, w + 120 * sc, h + 120 * sc }; SDL_RenderCopyF(ren, sh, NULL, &d);
  }
  if (a->state == 5) {   /* the "LG Home" tile: glass, icon and label */
    fillrr(x, yy, w, h, (int)c->w, (int)c->h, rad, 255, 255, 255, 13);
    SDL_Texture *bd = rr((int)c->w, (int)c->h, rad, 1, 0); SDL_SetTextureColorMod(bd, 255, 255, 255); SDL_SetTextureAlphaMod(bd, 31); SDL_FRect bdd = { x, yy, w, h }; SDL_RenderCopyF(ren, bd, NULL, &bdd);
    drawicon("restore", 64, x + (w - 64) / 2, yy + h / 2 - 64 + 14, 245, 246, 248, 230);
    drawtext("LG Home", 1, 22, (int)(w - 20), x + w / 2, yy + h / 2 + 22, 1, 245, 246, 248, 158);
  } else
  fillrr(x, yy, w, h, (int)c->w, (int)c->h, rad, bcol[0], bcol[1], bcol[2], 255);
  if (a->tex) {
    float fw = dock ? fminf(c->h * .56f * a->tw / a->th, c->w * .46f) : c->w * .64f, fh = dock ? fw * a->th / a->tw : c->h * .64f;
    if (!dock) { fw = c->w * .64f; fh = fw * a->th / a->tw; if (fh > c->h * .64f) { fh = c->h * .64f; fw = fh * a->tw / a->th; } }
    else { fh = fminf(c->h * .56f, c->w * .46f * a->th / a->tw); fw = fh * a->tw / a->th; }
    fw *= sc; fh *= sc; SDL_FRect d = { x + (w - fw) / 2, yy + (h - fh) / 2, fw, fh }; SDL_SetTextureColorMod(a->tex, 255, 255, 255); SDL_SetTextureAlphaMod(a->tex, 255); SDL_RenderCopyF(ren, a->tex, NULL, &d);
  } else if (a->state == 3) drawtext(a->title, 1, (int)(26 * sc), (int)(w - 24), x + w / 2, yy + h / 2 - 16 * sc, 1, 245, 246, 248, 230);
  if (focused) {
    SDL_Texture *rg = rr((int)c->w + 16, (int)c->h + 16, rad + 8, 4, 0); SDL_SetTextureColorMod(rg, 255, 255, 255); SDL_SetTextureAlphaMod(rg, 242);
    float gw = w + 16 * sc, gh = h + 16 * sc; SDL_FRect d = { x - 8 * sc, yy - 8 * sc, gw, gh }; SDL_RenderCopyF(ren, rg, NULL, &d);
  }
}
static void drawPill(Cell *c, int focused) {
  float sc = c->s, w = c->w * sc, h = c->h * sc, x = c->x - (w - c->w) / 2, y = c->y - (h - c->h) / 2;
  if (focused) fillrr(x, y, w, h, (int)c->w, (int)c->h, 32, 245, 246, 248, 255);
  else { fillrr(x, y, w, h, (int)c->w, (int)c->h, 32, 255, 255, 255, 23); SDL_Texture *rg = rr((int)c->w, (int)c->h, 32, 1, 0); SDL_SetTextureColorMod(rg, 255, 255, 255); SDL_SetTextureAlphaMod(rg, 26); SDL_FRect d = { x, y, w, h }; SDL_RenderCopyF(ren, rg, NULL, &d); }
  int fg = focused ? 11 : 245, fg2 = focused ? 12 : 246, fg3 = focused ? 16 : 248;
  drawicon(top[c->idx].icon, 30, x + 24 * sc, y + (h - 30) / 2, fg, fg2, fg3, 255);
  drawtext(barLabel(c->idx), 0, 26, 420, x + (24 + 30 + 14) * sc, y + (h - TTF_FontHeight(font(0, 26))) / 2, 0, fg, fg2, fg3, 255);
}
static void drawMenu(void) {
  Menu *m = &stack[nstack - 1]; SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND); SDL_SetRenderDrawColor(ren, 5, 6, 9, 248); SDL_Rect all = { 0, 0, W, H }; SDL_RenderFillRect(ren, &all);
  int N = 8, start = m->idx - 3 < m->n - N ? m->idx - 3 : m->n - N; if (start < 0) start = 0;
  int vis = m->n - start < N ? m->n - start : N, ph = 46 + 14 + 22 + vis * 96 + 22; float y0 = (H - ph) / 2.0f, x0 = (W - 760) / 2.0f;
  drawtext(m->title, 2, 46, 760, x0, y0, 0, 245, 246, 248, 255);
  float y = y0 + 46 + 14 + 22 + 6; if (start > 0) drawtext("\xe2\x96\xb2", 0, 20, 0, W / 2.0f, y - 28, 1, 245, 246, 248, 150);
  for (int i = start; i < start + vis; i++) {
    Item *it = &m->items[i]; int f = i == m->idx;
    if (f) fillrr(x0, y, 760, 84, 760, 84, 30, 245, 246, 248, 255); else { fillrr(x0, y, 760, 84, 760, 84, 30, 26, 28, 34, 255); SDL_Texture *rg = rr(760, 84, 30, 1, 0); SDL_SetTextureColorMod(rg, 255, 255, 255); SDL_SetTextureAlphaMod(rg, 20); SDL_FRect d = { x0, y, 760, 84 }; SDL_RenderCopyF(ren, rg, NULL, &d); }
    int cr = f ? 11 : 245, cg = f ? 12 : 246, cb = f ? 16 : 248, al = it->dim && !f ? 153 : 255; float tx = x0 + 34;
    if (it->icon) { drawicon("hdmi", 34, tx, y + 25, cr, cg, cb, al); tx += 34 + 22; }
    int subw = it->sub ? textw(it->sub, 0, 22, 330) : 0;
    drawtext(it->label, 1, 30, (int)(760 - 68 - (tx - x0 - 34) - (subw ? subw + 22 : 0)), tx, y + (84 - TTF_FontHeight(font(1, 30))) / 2, 0, cr, cg, cb, al);
    if (it->sub) drawtext(it->sub, 0, 22, 330, x0 + 760 - 34, y + (84 - TTF_FontHeight(font(0, 22))) / 2, 2, cr, cg, cb, f ? 165 : 165);
    y += 96;
  }
  if (start + vis < m->n) drawtext("\xe2\x96\xbc", 0, 20, 0, W / 2.0f, y - 4, 1, 245, 246, 248, 150);
}
static char shotPath[200];
static void logUptime(const char *what) { FILE *u = fopen("/proc/uptime", "r"); double up = 0; if (u) { if (fscanf(u, "%lf", &up) != 1) up = 0; fclose(u); } if (lg) fprintf(lg, "%s at uptime %.2f\n", what, up); }
static void shot(void) {   /* debug: the frame as a PPM, read before it is presented */
  SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888); SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
  FILE *o = fopen(shotPath, "wb"); if (o) { fprintf(o, "P6\n%d %d\n255\n", W, H); for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) { Uint32 p = *((Uint32 *)((Uint8 *)s->pixels + y * s->pitch) + x); fputc((p >> 16) & 255, o); fputc((p >> 8) & 255, o); fputc(p & 255, o); } fclose(o); }
  SDL_FreeSurface(s); shotPath[0] = 0;
}
static void render(void) {
  SDL_SetRenderDrawColor(ren, 7, 8, 11, 255); SDL_RenderClear(ren);
  /* ambient glow */
  for (int s = 0; s < 2; s++) if (glA[s] > 0.003f) {
    SDL_SetTextureColorMod(glowTex, gl[s][0], gl[s][1], gl[s][2]); SDL_SetTextureAlphaMod(glowTex, (int)(glA[s] * 255)); SDL_Rect d = { 0, 0, W, 720 }; SDL_RenderCopy(ren, glowTex, NULL, &d); }
  time_t tt = time(NULL); struct tm tm; localtime_r(&tt, &tm); char tb[40], db[80]; fmtTime(tb, sizeof tb, &tm); fmtDate(db, sizeof db, &tm);
  int tw = drawtextLS(tb, 1, 56, -1.1f, 96, 54 + (76 - TTF_FontHeight(font(1, 56))) / 2, 245, 246, 248, 255, 1);
  drawtext(db, 0, 28, 0, 96 + tw + 22, 54 + (76 - TTF_FontHeight(font(0, 28))) / 2 + 6, 0, 245, 246, 248, 224);
  /* the focused name */
  Cell *fcell = nrows ? &cells[fr][fc] : NULL;
  if (fcell) { const char *nm = fcell->kind == 0 ? apps[fcell->idx].title : barLabel(fcell->idx); drawtextLS(nm, 2, 92, -2.6f, 96, 150 + (120 - TTF_FontHeight(font(2, 92))) / 2 - 14, 245, 246, 248, 255, 1); }
  for (int i = 0; i < rowLen[0]; i++) if (!(fr == 0 && fc == i)) drawPill(&cells[0][i], 0);
  if (fr == 0) drawPill(&cells[0][fc], 1);
  /* dock and grid clip under the shelf */
  SDL_Rect clip = { 0, 300, W, H - 300 }; SDL_RenderSetClipRect(ren, &clip);
  for (int r = 1; r < nrows; r++) {
    int dock = r == 1 && cells[1][0].kind == 0;
    for (int i = 0; i < rowLen[r]; i++) { Cell *c = &cells[r][i]; if (c->kind != 0) continue; if (r == fr && i == fc) continue; float y = c->y + (r >= 1 ? scrollY : 0); if (y > H || y + c->h < 300) continue; drawTile(c, 0, y, dock); }
  }
  if (fr >= 1 && cells[1][0].kind == 0 && cells[fr][fc].kind == 0) { Cell *c = &cells[fr][fc]; drawTile(c, 1, c->y + (fr >= 1 ? scrollY : 0) + (fr == 1 ? -10 * clampf((c->s - 1) / 0.05f, 0, 1) : 0), fr == 1); }
  SDL_RenderSetClipRect(ren, NULL);
  if (fr == 1 && cells[1][0].kind == 1) { /* no dock: the top buttons are repeated in row 1 */ }
  if (nstack) drawMenu();
  if (toastUntil > now_ms()) {
    int w = textw(toastS, 0, 28, 1500) + 76; float x = (W - w) / 2.0f, y = H - 64 - 72;
    fillrr(x, y, w, 72, w, 72, 34, 20, 22, 27, 255); SDL_Texture *rg = rr(w, 72, 34, 1, 0); SDL_SetTextureColorMod(rg, 255, 255, 255); SDL_SetTextureAlphaMod(rg, 30); SDL_FRect d = { x, y, w, 72 }; SDL_RenderCopyF(ren, rg, NULL, &d);
    drawtext(toastS, 0, 28, 1500, x + 38, y + (72 - TTF_FontHeight(font(0, 28))) / 2, 0, 245, 246, 248, 255);
  }
  if (shotPath[0]) shot();
  SDL_RenderPresent(ren);
}

/* ---------------------------------------------------------------- focus, animation */
static void setFocus(int nr, int nc, int first) {
  fr = nr; fc = nc; if (fc >= rowLen[fr]) fc = rowLen[fr] - 1; if (fc < 0) fc = 0;
  Cell *e = &cells[fr][fc]; float sh = 0;
  if (fr >= 2) { float off = (fr - 2) * 177.0f, gh = (nrows - 2) * 151.0f + (nrows - 3) * 26.0f; sh = fminf(0, fmaxf(50 - (350 + off), 726 - (350 + gh))); }
  scrollT = sh; if (first) scrollY = sh;
  if (e->kind == 0 && apps[e->idx].hasColor) { memcpy(glTarget, apps[e->idx].gc, sizeof glTarget); glowAt = first ? 0 : now_ms() + 110; } else { glTarget[0] = 74; glTarget[1] = 84; glTarget[2] = 112; glowAt = now_ms() + 110; }
  if (first) glowAt = 1;
  dirty = 1;
}
static int mapCol(int from, int to, int col) { return from > 1 ? (int)lroundf(col * (to - 1.0f) / (from - 1)) : 0; }
static void stepAnim(float dt) {
  float k = 1 - expf(-dt / 0.055f); animating = 0;
  for (int r = 0; r < nrows; r++) for (int i = 0; i < rowLen[r]; i++) { Cell *c = &cells[r][i]; float tg = (r == fr && i == fc) ? (c->kind == 1 ? 1.07f : r == 1 && cells[1][0].kind == 0 ? 1.05f : 1.12f) : 1.0f;
    if (fabsf(c->s - tg) > 0.002f) { c->s += (tg - c->s) * k; animating = 1; } else c->s = tg; }
  if (fabsf(scrollY - scrollT) > 0.4f) { scrollY += (scrollT - scrollY) * (1 - expf(-dt / 0.1f)); animating = 1; } else scrollY = scrollT;
  /* glow: switch layers once focus settles, cross-fade over 350 ms */
  if (glowAt && now_ms() >= glowAt) { glowAt = 0; if (glTarget[0] != glCur[0] || glTarget[1] != glCur[1] || glTarget[2] != glCur[2]) { memcpy(glCur, glTarget, sizeof glCur); glSide ^= 1; for (int i = 0; i < 3; i++) gl[glSide][i] = glCur[i]; } }
  else if (glowAt) animating = 1;
  for (int s = 0; s < 2; s++) { float tg = s == glSide ? 1 : 0, step = dt / 0.35f; if (glA[s] < tg) { glA[s] = fminf(tg, glA[s] + step); animating = 1; } else if (glA[s] > tg) { glA[s] = fmaxf(tg, glA[s] - step); animating = 1; } }
}

/* ---------------------------------------------------------------- input */
static unsigned okDownAt; static int okDown, okHeld, okInMenu; static unsigned okHoldAt;
static int keyOf(SDL_Keysym k) {   /* 1 up 2 down 3 left 4 right 5 ok 6 back 0 other */
  switch (k.sym) { case SDLK_UP: return 1; case SDLK_DOWN: return 2; case SDLK_LEFT: return 3; case SDLK_RIGHT: return 4; case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: return 5;
    case SDLK_ESCAPE: case SDLK_BACKSPACE: case SDLK_AC_BACK: return 6; }
  /* LG's SDL reports the arrow keys with the SDL scancode as the key symbol (79 right, 80 left, 81 down, 82 up) and no scancode */
  switch (k.sym) { case 82: return 1; case 81: return 2; case 80: return 3; case 79: return 4; case 40: case 13: case 88: return 5; case 41: case 42: case 270: case 461: return 6; }
  if (k.scancode == 461 || k.scancode == SDL_SCANCODE_AC_BACK || k.scancode == 158) return 6;
  return 0;
}
static Cell *cellAt(int x, int y) {
  for (int r = nrows - 1; r >= 0; r--) for (int i = 0; i < rowLen[r]; i++) { Cell *c = &cells[r][i]; float cy = c->y + (r >= 1 ? scrollY : 0); if (r >= 1 && cy + c->h < 300) continue; if (r >= 1 && y < 300) continue;
    if (x >= c->x && x < c->x + c->w && y >= cy && y < cy + c->h) return c; }
  return NULL;
}
static int posOf(Cell *c, int *rr_, int *cc_) { for (int r = 0; r < nrows; r++) for (int i = 0; i < rowLen[r]; i++) if (&cells[r][i] == c) { *rr_ = r; *cc_ = i; return 1; } return 0; }
static int menuRowAt(int x, int y) {   /* the row under a pointer position, or -1 */
  Menu *m = &stack[nstack - 1]; int N = 8, start = m->idx - 3 < m->n - N ? m->idx - 3 : m->n - N; if (start < 0) start = 0; int vis = m->n - start < N ? m->n - start : N;
  float ph = 46 + 14 + 22 + vis * 96 + 22, y0 = (H - ph) / 2 + 46 + 14 + 22 + 6, x0 = (W - 760) / 2.0f;
  for (int i = 0; i < vis; i++) if (x >= x0 && x < x0 + 760 && y >= y0 + i * 96 && y < y0 + i * 96 + 84) return start + i;
  return -1;
}
static void keyDown(int k) {
  lastKeyAt = now_ms(); dirty = 1;
  if (nstack) {
    Menu *m = &stack[nstack - 1];
    if (k == 1 && m->idx > 0) m->idx--; else if (k == 2 && m->idx < m->n - 1) m->idx++; else if (k == 6) backMenu(); else if (k == 5) okInMenu = 1;
    return;
  }
  int len = rowLen[fr];
  if (k == 3 && fc > 0) setFocus(fr, fc - 1, 0); else if (k == 4 && fc < len - 1) setFocus(fr, fc + 1, 0);
  else if (k == 1 && fr > 0) setFocus(fr - 1, mapCol(len, rowLen[fr - 1], fc), 0);
  else if (k == 2 && fr < nrows - 1) setFocus(fr + 1, mapCol(len, rowLen[fr + 1], fc), 0);
  else if (k == 5) { if (!okDown) { okDown = 1; okHeld = 0; okHoldAt = now_ms() + 650; } }
}
static void keyUp(int k) {
  if (k != 5) return;
  if (nstack) { if (okInMenu) { okInMenu = 0; pickItem(); dirty = 1; } okDown = 0; return; }
  if (okDown && !okHeld) act(&cells[fr][fc]);
  okDown = 0; okHeld = 0; dirty = 1;
}
static void debugCmd(void) {
  static long off; FILE *f = fopen("/tmp/cytv-native.cmd", "r"); if (!f) return; char line[300];
  fseek(f, 0, SEEK_END); long size = ftell(f); if (size < off) off = 0; if (size == off) { fclose(f); return; } fseek(f, off, SEEK_SET);   /* only what was added since the last look */
  while (fgets(line, sizeof line, f)) {
    line[strcspn(line, "\r\n")] = 0;
    if (lg) fprintf(lg, "cmd %s\n", line);
    if (!strncmp(line, "key ", 4)) { const char *n = line + 4; int k = !strcmp(n, "up") ? 1 : !strcmp(n, "down") ? 2 : !strcmp(n, "left") ? 3 : !strcmp(n, "right") ? 4 : !strcmp(n, "ok") ? 5 : !strcmp(n, "back") ? 6 : 0;
      if (k) { keyDown(k); keyUp(k); } }
    else if (!strcmp(line, "okhold")) options(&cells[fr][fc]);
    else if (!strncmp(line, "shot ", 5)) { snprintf(shotPath, sizeof shotPath, "%s", line + 5); render(); }
    else if (!strcmp(line, "reload")) enqueue(1, NULL, NULL);
    else if (!strcmp(line, "quit")) running = 0;
    else if (!strncmp(line, "stat ", 5)) { FILE *o = fopen(line + 5, "w"); if (o) { fprintf(o, "frames %u apps %d animating %d visible %d\n", 0u, napps, animating, visible); fclose(o); } }
  }
  off = ftell(f); fclose(f); dirty = 1;
}

static void makeGlow(void) {   /* the web launcher's radial glow: an ellipse 1400 x 640 at 26% / -6%, fading out by 70% */
  int gw = 480, gh = 180; SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, gw, gh, 32, SDL_PIXELFORMAT_ARGB8888);
  for (int y = 0; y < gh; y++) for (int x = 0; x < gw; x++) {
    float px = (x + .5f) / gw * W - 0.26f * W, py = (y + .5f) / gh * 720 + 0.06f * 720, d = sqrtf((px * px) / (1400.0f * 1400.0f) + (py * py) / (640.0f * 640.0f));
    float a = clampf(1 - d / 0.7f, 0, 1) * 0.45f;
    *((Uint32 *)((Uint8 *)s->pixels + y * s->pitch) + x) = ((Uint32)(a * 255 + .5f) << 24) | 0xffffff; }
  glowTex = SDL_CreateTextureFromSurface(ren, s); SDL_FreeSurface(s); SDL_SetTextureBlendMode(glowTex, SDL_BLENDMODE_BLEND); SDL_SetTextureScaleMode(glowTex, SDL_ScaleModeBest);
}
static void readLocale(void) {
  FILE *f = fopen("/var/luna/preferences/localeInfo", "r"); char b[1024]; size_t n = 0; if (f) { n = fread(b, 1, sizeof b - 1, f); fclose(f); } b[n] = 0;
  const char *p = strstr(b, "\"UI\""); const char *l = p ? strchr(p + 4, '"') : NULL; if (!l) { const char *e = getenv("LANG"); l = e ? e - 1 : NULL; }
  if (l) { const char *c = l + 1; lang = !strncmp(c, "nl", 2) ? 1 : !strncmp(c, "de", 2) ? 2 : !strncmp(c, "fr", 2) ? 3 : !strncmp(c, "es", 2) ? 4 : 0; }
}

int main(int argc, char **argv) {
  lg = fopen(argc > 1 && strstr(argv[1], "com.webos.app.home") ? "/tmp/cytv-home.log" : "/tmp/cytv-native.log", "w"); if (lg) { setvbuf(lg, NULL, _IOLBF, 0); }
  logUptime("start"); readLocale();
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1"); SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0) { if (lg) fprintf(lg, "SDL_Init: %s\n", SDL_GetError()); return 1; }
  if (TTF_Init() != 0 || IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG) == 0) { if (lg) fprintf(lg, "ttf/img: %s\n", SDL_GetError()); }
  win = SDL_CreateWindow("CleanYourTV", 0, 0, W, H, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_SHOWN);
  ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : NULL;
  if (!ren) { if (lg) fprintf(lg, "window/renderer: %s\n", SDL_GetError()); return 2; }
 
  { unsigned mask = 0x6; FILE *mf = fopen("/tmp/cytv-keymask", "r"); if (mf) { if (fscanf(mf, "%x", &mask) != 1) mask = 0x6; fclose(mf); } claimKeys(win, mask); }
  EV = SDL_RegisterEvents(1); jm = SDL_CreateMutex(); js = SDL_CreateSemaphore(0); SDL_CreateThread(worker, "net", NULL);
  resident = argc > 1 && strstr(argv[1], "\"nid\":\"com.webos.app.home\"") != NULL;
  cfg = jnew(J_OBJ); usage = jnew(J_OBJ); makeGlow();
  int dbg = access("/tmp/cytv-native.debug", F_OK) == 0;
  if (lg) fprintf(lg, "up, version %s, lang %d, debug %d, argc %d%s%s\n", VERSION, lang, dbg, argc, argc > 1 ? " argv1 " : "", argc > 1 ? argv[1] : "");
  /* first frame at once, then the apps */
  { SDL_SetRenderDrawColor(ren, 7, 8, 11, 255); SDL_RenderClear(ren); SDL_RenderPresent(ren); }
  mkdir(CACHE, 0777);
  readTop(); layout(); setFocus(1, 0, 1);
  { int n = 0; unsigned char *c = readFile(CACHE "/apps.json", &n, NULL); if (c) applyApps(c); }
  enqueue(1, NULL, NULL);
  if (argc > 1 && strstr(argv[1], "\"inputs\":true")) inputsMenu();
  if (argc > 1 && strstr(argv[1], "\"live\":true")) launch("__live", NULL);
  unsigned prev = now_ms(), lastTick = 0, retry = 0, frames = 0, lastLog = now_ms(); int loaded = 0, firstDone = 0;
  while (running) {
    SDL_Event e; unsigned timeout = 15000;
    if (animating || dirty) timeout = 0; else if (okDown && !okHeld) timeout = 100; else if (toastUntil > now_ms()) timeout = 250; if (dbg && timeout > 120) timeout = 120;
    int have = SDL_WaitEventTimeout(&e, visible ? timeout : 1000);
    while (have) {
      if (e.type == SDL_QUIT) { if (lg) fprintf(lg, "SDL_QUIT\n"); running = 0; }
      else if (e.type == SDL_KEYDOWN && !e.key.repeat) { if (lg) fprintf(lg, "key sym %d scan %d -> %d\n", e.key.keysym.sym, e.key.keysym.scancode, keyOf(e.key.keysym)); keyDown(keyOf(e.key.keysym)); }
      else if (e.type == SDL_KEYDOWN) { if (keyOf(e.key.keysym) >= 1 && keyOf(e.key.keysym) <= 4 && !nstack) keyDown(keyOf(e.key.keysym)); else if (nstack && keyOf(e.key.keysym) >= 1 && keyOf(e.key.keysym) <= 2) keyDown(keyOf(e.key.keysym)); }
      else if (e.type == SDL_KEYUP) keyUp(keyOf(e.key.keysym));
      else if (e.type == SDL_MOUSEMOTION) { static int nm; if (lg && nm++ < 20) fprintf(lg, "mouse move %d,%d\n", e.motion.x, e.motion.y); mx = e.motion.x; my = e.motion.y; if (nstack) { if (abs(mx - lastMX) + abs(my - lastMY) >= 8) { lastMX = mx; lastMY = my; int r = menuRowAt(mx, my); if (r >= 0 && r != stack[nstack - 1].idx) { stack[nstack - 1].idx = r; dirty = 1; } } } else if (now_ms() - lastKeyAt > 1500) {
          if (lastMX < 0) { lastMX = mx; lastMY = my; } else if (abs(mx - lastMX) + abs(my - lastMY) >= 24) { lastMX = mx; lastMY = my; Cell *c = cellAt(mx, my); int r2, c2; if (c && posOf(c, &r2, &c2) && !(r2 == fr && c2 == fc)) setFocus(r2, c2, 0); } } }
      else if (e.type == SDL_MOUSEBUTTONDOWN) { if (lg) fprintf(lg, "mouse down %d,%d button %d\n", e.button.x, e.button.y, e.button.button);
        if (nstack) { int r = e.button.button == SDL_BUTTON_LEFT ? menuRowAt(e.button.x, e.button.y) : -1;   /* a row picks it; the right button or a click beside the list goes back */
          if (r >= 0) { stack[nstack - 1].idx = r; pickItem(); } else backMenu(); }
          else { Cell *c = cellAt(e.button.x, e.button.y); int r2, c2; if (c && posOf(c, &r2, &c2)) { setFocus(r2, c2, 0); okDown = 1; okHeld = 0; okHoldAt = now_ms() + 650; } } dirty = 1; }
      else if (e.type != EV && e.type != SDL_WINDOWEVENT && e.type != SDL_KEYDOWN && e.type != SDL_KEYUP && e.type != SDL_MOUSEBUTTONUP && lg) fprintf(lg, "event type %u\n", e.type);
      if (e.type == SDL_MOUSEBUTTONUP) { if (!nstack && okDown && !okHeld) act(&cells[fr][fc]); okDown = 0; okHeld = 0; dirty = 1; }
      else if (e.type == SDL_WINDOWEVENT) {
        if (e.window.event == SDL_WINDOWEVENT_HIDDEN || e.window.event == SDL_WINDOWEVENT_MINIMIZED) { if (!resident) running = 0; else visible = 0; }
        else if (e.window.event == SDL_WINDOWEVENT_SHOWN || e.window.event == SDL_WINDOWEVENT_EXPOSED || e.window.event == SDL_WINDOWEVENT_RESTORED || e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) { if (!visible || e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) enqueue(1, NULL, NULL); if (!visible && resident) { SDL_ShowWindow(win); SDL_RaiseWindow(win); SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN); } visible = 1; dirty = 1; }
        if (lg) fprintf(lg, "window event %d\n", e.window.event);
      }
      else if (e.type == EV) {
        switch (e.user.code) {
          case 1: loaded = e.user.data1 != NULL; applyApps(e.user.data1); if (!loaded) retry = now_ms() + 3000; break;
          case 2: applyIcon(e.user.data1); break;
          case 3: { char m[200]; snprintf(m, sizeof m, "%s %s", t("Could not open"), (char *)e.user.data1); toast(m); free(e.user.data1); break; }
          case 5: applyInputs(e.user.data1); break;
        }
        dirty = 1;
      }
      have = SDL_PollEvent(&e);
    }
    unsigned now = now_ms();
    if (retry && now >= retry) { retry = 0; enqueue(1, NULL, NULL); }
    if (okDown && !okHeld && now >= okHoldAt && !nstack) { okHeld = 1; options(&cells[fr][fc]); dirty = 1; }
    if (dbg) debugCmd();
    if (now - lastTick > 15000) { lastTick = now; dirty = 1; }
    float dt = (now - prev) / 1000.0f; if (dt > 0.1f) dt = 0.1f; prev = now;
    stepAnim(dt);
    if (visible && (dirty || animating)) { dirty = 0; render(); if (!frames && !firstDone) { firstDone = 1; logUptime("first frame with content"); } frames++; }
    if (lg && now - lastLog >= 5000) { fprintf(lg, "frames %u in %u ms animating %d dirty %d\n", frames, now - lastLog, animating, dirty); frames = 0; lastLog = now; }
  }
  SDL_Quit(); return 0;
}
