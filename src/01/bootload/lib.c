#include "defines.h"
#include "serial.h"
#include "lib.h"

// メモリを特定のバイトデータで埋める
void *memset(void *b, int c, long len) {
  char *p;
  for (p = b; len > 0; len--) {
    *(p++) = c;
  }
  return b;
}

// メモリのコピー
void *memcpy(void *dst, const void *src, long len) {
  char *d = dst;
  const char *s = src;
  for (; len > 0; len--) {
    *(d++) = *(s++);
  }
  return dst;
}

// メモリの比較
int memcmp(const void *b1, const void *b2, long len) {
  const char *p1 = b1, *p2 = b2;
  for (; len > 0; len--) {
    if (*p1 != *p2) {
      return (*p1 > *p2) ? 1 : -1;
    }
    p1++;
    p2++;
  }
  return 0;
}

// 文字列の長さ
int strlen(const char *s) {
  int len;
  for (len =0; *s; s++, len++);
  return len;
}

// 文字列のコピー
char *strcpy(char *dst, const char *src) {
  char *d = dst;
  for (;; dst++, src++) {
    *dst = *src;
    if (!*src) break;
  }
  return d;
}

// 文字列の比較
int strcmp(const char *s1, const char *s2) {
  while (*s1 || *s2) {
    if (*s1 != *s2) {
      return (*s1 > *s2) ? 1 : -1;
    }
    s1++;
    s2++;
  }
  return 0;
}

// 長さ指定での文字列の比較
int strncmp(const char *s1, const char *s2, int len) {
  while ((*s1 || *s2) && (len >0)) {
    if (*s1 != *s2) {
      return (*s1 > *s2) ? 1 : -1;
    }
    s1++;
    s2++;
    len--;
  }
  return 0;
}

/* 1文字送信 */
int putc(unsigned char c) {
  if (c == '\n') {
    serial_send_byte(SERIAL_DEFAULT_DEVICE, '\r');
  }
  return serial_send_byte(SERIAL_DEFAULT_DEVICE, c);
}

/* 1文字受信 */
unsigned char getc(void) {
  unsigned char c = serial_recv_byte(SERIAL_DEFAULT_DEVICE);
  c = (c == '\r') ? '\n' : c; // 改行コードの変換
  putc(c); // エコーバック
  return c;
}

/* 文字列送信 */
int puts(unsigned char *str) {
  while (*str) {
    putc(*(str++));
  }
  return 0;
}

/* 文字列受信 */
int gets(unsigned char *buf) {
  int i = 0;
  unsigned char c;
  do {
    c = getc();
    if (c == '\n') { // 改行コードは削る
      c = '\0';
    }
    buf[i++] = c;
  } while (c);
  return i - 1;
}

/* 数値の 16 進表示 */
int putxval(unsigned long value, int column) {
  char buf[9];
  char *p;

  p = buf + sizeof(buf) - 1;
  *(p--) = '\0';

  if (!value && !column) {
    column++;
  }

  while (value || column) {
    // 16 進文字に変換してバッファに格納する
    *(p--) = "0123456789abcdef"[value & 0xf];
    value >>= 4;
    if (column) column--;
  }

  puts(p + 1);

  return 0;
}
