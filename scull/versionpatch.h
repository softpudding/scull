#include <linux/version.h>
/*
 * asm/system.h is removed in kernel 3.4
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(3,4,0) 
#include <asm/system.h>         /* cli(), *_flags */
#endif

/* 
 * <linux/uaccess.h> was added in kernel version 2.6.18, and should be
 * included in preference to <asm/uaccess.h>.  In particular, copy_to_user()
 * and copy_from_user() were moved to <linux/uaccess.h> in kernel version
 * 4.12.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,18)
#include <linux/uaccess.h>
#else
#include <asm/uaccess.h>
#endif

/*
 * Kernel 5.0 removed VERIFY_READ and VERIFY_WRITE and removed the first
 * parameter of access_ok() which was set to VERIFY_READ or VERIFY_WRITE.
 * That has been redundant since kernel 2.5.70, and even then it was only
 * checked for kernels that support old 386 processors.
 *
 * Get rid of the first parameter and always pass VERIFY_WRITE for kernels
 * prior to 5.0.  This will fail for old 386 processors on pre-2.5.70
 * kernels if the memory region is not in fact writeable.
 */
#ifdef VERIFY_WRITE
/* Pre 5.0 kernel. */
static inline int _kcompat_access_ok(unsigned long addr, size_t size)
{
    /* Always use VERIFY_WRITE.  Most architectures ignore it. */
    return access_ok(VERIFY_WRITE, addr, size);
}
/* Redefine access_ok() to remove first parameter. */
#undef access_ok
#define access_ok(addr, size) _kcompat_access_ok((unsigned long)(addr), (size))
#endif
