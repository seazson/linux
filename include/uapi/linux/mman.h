#ifndef _UAPI_LINUX_MMAN_H
#define _UAPI_LINUX_MMAN_H

#include <asm/mman.h>

#define MREMAP_MAYMOVE	1
#define MREMAP_FIXED	2

#define OVERCOMMIT_GUESS		0      /*����ʽ��ʽ�������ڴ����*/
#define OVERCOMMIT_ALWAYS		1      /*�����ڴ�����ύ*/
#define OVERCOMMIT_NEVER		2      /*Ԥ���Է�ʽ����ֹ���ȷ���*/

#endif /* _UAPI_LINUX_MMAN_H */
