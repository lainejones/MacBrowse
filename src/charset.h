#ifndef CHARSET_H
#define CHARSET_H
#include <stdint.h>
uint8_t mac2lat(uint8_t c);
uint8_t lat2mac(uint8_t c);
void mac2lat_str(char *dst, const uint8_t *src, int n);
void lat2mac_str(uint8_t *dst, const char *src);
void amiga_name(char *dst, const uint8_t *pname, int raw);
int lat_strcasecmp(const char *a, const char *b);
void type_from_ext(const char *name, uint8_t *finfo);
#endif
