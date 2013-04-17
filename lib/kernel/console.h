#ifndef __LIB_KERNEL_CONSOLE_H
#define __LIB_KERNEL_CONSOLE_H
#include "../stddef.h"

void console_init (void);
void console_panic (void);
void console_print_stats (void);
void putbuf (const char *, size_t);

#endif /* lib/kernel/console.h */
