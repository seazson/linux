#ifndef _UAPI_LINUX_MMAN_H
#define _UAPI_LINUX_MMAN_H

#include <asm/mman.h>

#define MREMAP_MAYMOVE	1
#define MREMAP_FIXED	2

#define OVERCOMMIT_GUESS		0      /*启发式方式，正常内存分配*/
#define OVERCOMMIT_ALWAYS		1      /*允许内存过度提交*/
#define OVERCOMMIT_NEVER		2      /*预留性方式，防止过度分配*/

#endif /* _UAPI_LINUX_MMAN_H */
