#ifndef UTILS_H
#define UTILS_H

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "hci.h"

#define le16toh(x) __builtin_bswap16(x)
#define htole16(x) __builtin_bswap16(x)

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#define MIN2(x, y)	(((x) < (y)) ? (x) : (y))
#define MAX2(x, y)	(((x) > (y)) ? (x) : (y))
#define ROUNDUP32(x)	(((u32)(x) + 0x1f) & ~0x1f)
#define ROUNDDOWN32(x)	(((u32)(x) - 0x1f) & ~0x1f)

#define UNUSED(x) (void)(x)
#define MEMBER_SIZE(type, member) sizeof(((type *)0)->member)

#define STRINGIFY(x)	#x
#define TOSTRING(x)	STRINGIFY(x)

#ifdef assert
#undef assert
#endif
#define assert(exp) ( (exp) ? (void)0 : my_assert_func(__FILE__, __LINE__, __FUNCTION__, #exp))

//#define DEBUG(...) printf(__VA_ARGS__)
#define DEBUG(...) (void)0

extern void my_assert_func(const char *file, int line, const char *func, const char *failedexpr);

static inline int memmismatch(const void *restrict a, const void *restrict b, int size)
{
	int i = 0;
	while (size) {
		if (((u8 *)a)[i] != ((u8 *)b)[i])
			return i;
		i++;
		size--;
	}
	return i;
}

/* Message injection helpers */
int inject_msg_to_usb_intr_ready_queue(void *msg);
int inject_msg_to_usb_bulk_in_ready_queue(void *msg);

#endif
