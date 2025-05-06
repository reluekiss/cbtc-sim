#include "libc.h"
#include "esp32_hw.h"

void* memset(void* s, int c, size_t n) {
    unsigned char* p = s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

void* memcpy(void* dest, const void* src, size_t n) {
    unsigned char* d = dest;
    const unsigned char* s = src;
    while (n--) *d++ = *s++;
    return dest;
}

int memcmp(const void* s1, const void* s2, size_t n) {
    const unsigned char* p1 = s1;
    const unsigned char* p2 = s2;
    while (n--) {
        if (*p1 != *p2) {
            return *p1 - *p2;
        }
        p1++;
        p2++;
    }
    return 0;
}

size_t strlen(const char* s) {
    const char* p = s;
    while (*p) p++;
    return p - s;
}

char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++));
    return dest;
}

int strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

int snprintf(char* str, size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    int result = vsnprintf(str, size, format, args);
    va_end(args);
    return result;
}

int vsnprintf(char* str, size_t size, const char* format, va_list args) {
    char* s = str;
    char* end = str + size - 1;
    
    while (*format && s < end) {
        if (*format != '%') {
            *s++ = *format++;
            continue;
        }
        
        format++; // Skip '%'
        
        switch (*format) {
            case 'd': {
                int val = va_arg(args, int);
                char num[12]; // Enough for 32-bit integer
                char* p = num + sizeof(num) - 1;
                *p = '\0';
                
                if (val == 0) {
                    *--p = '0';
                } else {
                    int is_neg = (val < 0);
                    if (is_neg) val = -val;
                    
                    while (val > 0) {
                        *--p = '0' + (val % 10);
                        val /= 10;
                    }
                    
                    if (is_neg) *--p = '-';
                }
                
                while (*p && s < end) {
                    *s++ = *p++;
                }
                break;
            }
            case 's': {
                char* val = va_arg(args, char*);
                while (*val && s < end) {
                    *s++ = *val++;
                }
                break;
            }
            case 'c': {
                char val = (char)va_arg(args, int);
                *s++ = val;
                break;
            }
        }
        
        format++;
    }
    
    *s = '\0';
    return s - str;
}

int printf(const char* format, ...) {
    char buffer[256];
    va_list args;
    va_start(args, format);
    int result = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    for (char* p = buffer; *p; p++) {
        esp_uart_putc(0, *p);
    }
    
    return result;
}
