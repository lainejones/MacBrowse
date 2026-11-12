/* charset.c - Mac Roman <-> ISO-8859-1 (Amiga) conversion. */
#include "charset.h"
#include <string.h>

/* Mac Roman 0x80..0xFF to Latin-1.  Characters with no Latin-1 equivalent are given an
   ASCII stand-in (<0x80), which is not converted back. */
static const uint8_t m2l[128] = {
    0xC4,0xC5,0xC7,0xC9,0xD1,0xD6,0xDC,0xE1,0xE0,0xE2,0xE4,0xE3,0xE5,0xE7,0xE9,0xE8, /* 80 */
    0xEA,0xEB,0xED,0xEC,0xEE,0xEF,0xF1,0xF3,0xF2,0xF4,0xF6,0xF5,0xFA,0xF9,0xFB,0xFC, /* 90 */
    '+', 0xB0,0xA2,0xA3,0xA7,0xB7,0xB6,0xDF,0xAE,0xA9,'T', 0xB4,0xA8,'#', 0xC6,0xD8, /* A0 */
    '8', 0xB1,'<', '>', 0xA5,0xB5,'d', 'S', 'P', 'p', 'S', 0xAA,0xBA,'O', 0xE6,0xF8, /* B0 */
    0xBF,0xA1,0xAC,'v', 'f', '~', 'D', 0xAB,0xBB,'.', 0xA0,0xC0,0xC3,0xD5,'O', 'o',  /* C0 */
    '-', '-', '"', '"', '\'','\'',0xF7,'o', 0xFF,'Y', '/', 0xA4,'<', '>', 'f', 'f',  /* D0 */
    '+', '.', ',', '"', '%', 0xC2,0xCA,0xC1,0xCB,0xC8,0xCD,0xCE,0xCF,0xCC,0xD3,0xD4, /* E0 */
    '@', 0xD2,0xDA,0xDB,0xD9,'i', '^', '~', 0xAF,'u', '.', 0xB0,0xB8,'"', ',', '^'   /* F0 */
};

static uint8_t l2m[128];
static int l2m_init;

static void init_l2m(void)
{
    int i;
    memset(l2m, 0, sizeof(l2m));
    for (i = 0; i < 128; i++)
        if (m2l[i] >= 0x80 && !l2m[m2l[i] - 0x80]) l2m[m2l[i] - 0x80] = (uint8_t)(0x80 + i);
    l2m_init = 1;
}

uint8_t mac2lat(uint8_t c) { return c < 0x80 ? c : m2l[c - 0x80]; }

uint8_t lat2mac(uint8_t c)
{
    if (c < 0x80) return c;
    if (!l2m_init) init_l2m();
    return l2m[c - 0x80] ? l2m[c - 0x80] : '_';
}

void mac2lat_str(char *dst, const uint8_t *src, int n)
{
    int i;
    for (i = 0; i < n; i++) dst[i] = (char)mac2lat(src[i]);
    dst[n] = 0;
}

void lat2mac_str(uint8_t *dst, const char *src)
{
    for (; *src; src++) *dst++ = lat2mac((uint8_t)*src);
    *dst = 0;
}

/* Name usable as an AmigaDOS file name: Latin-1, no ':' or '/', no control chars. */
void amiga_name(char *dst, const uint8_t *pname, int raw)
{
    int i, n = pname[0];
    for (i = 0; i < n; i++) {
        uint8_t c = raw ? pname[1 + i] : mac2lat(pname[1 + i]);
        if (c == '/' || c == ':' || c < 0x20 || c == 0x7F) c = '_';
        dst[i] = (char)c;
    }
    dst[n] = 0;
    if (!strcmp(dst, ".") || !strcmp(dst, "..")) dst[0] = '_';
}

static int lat_lower(int c)
{
    c &= 255;
    if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) c += 32;
    return c;
}

/* Case-insensitive compare of Latin-1 strings (AmigaDOS name rules). */
int lat_strcasecmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int d = lat_lower(*a) - lat_lower(*b);
        if (d || !*a) return d;
    }
}

/* Type/creator from a file name extension ("????????" if unknown). */
void type_from_ext(const char *name, uint8_t *finfo)
{
    static const char *map[][3] = {
        {".txt", "TEXT", "ttxt"}, {".text", "TEXT", "ttxt"}, {".readme", "TEXT", "ttxt"},
        {".doc", "TEXT", "ttxt"}, {".hqx", "TEXT", "BnHq"}, {".sit", "SIT!", "SIT!"},
        {".sea", "APPL", "aust"}, {".cpt", "PACT", "CPCT"}, {".pict", "PICT", "ttxt"},
        {".pct", "PICT", "ttxt"}, {".gif", "GIFf", "ogle"}, {".jpg", "JPEG", "ogle"},
        {".jpeg", "JPEG", "ogle"}, {".png", "PNGf", "ogle"}, {".mod", "STrk", "SNPL"},
        {".aiff", "AIFF", "SNPL"}, {".zip", "ZIP ", "SITx"}, {".img", "rohd", "ddsk"},
        {".image", "rohd", "ddsk"}, {".dsk", "rohd", "ddsk"}, {".pdf", "PDF ", "CARO"},
        {".html", "TEXT", "MOSS"}, {".htm", "TEXT", "MOSS"}, {".c", "TEXT", "CWIE"},
        {".h", "TEXT", "CWIE"}, {NULL, NULL, NULL}
    };
    const char *dot = strrchr(name, '.');
    int i;
    memcpy(finfo, "????????", 8);
    if (!dot) return;
    for (i = 0; map[i][0]; i++)
        if (!lat_strcasecmp(dot, map[i][0])) { memcpy(finfo, map[i][1], 4); memcpy(finfo + 4, map[i][2], 4); return; }
}
