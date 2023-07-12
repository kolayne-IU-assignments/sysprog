#pragma once

// <asm-generic/int-ll64.h>
typedef unsigned long long __u64;

// <linux/types.h>
#ifdef __CHECKER__
#define __bitwise   __attribute__((bitwise))
#else
#define __bitwise
#endif
typedef __u64 __bitwise __le64;
#define __aligned_u64 __u64 __attribute__((aligned(8)))

// <linux/sched.h>
#define CLONE_PARENT    0x00008000  /* set if we want to have the same parent as the cloner */
// This is the first version of `struct clone_args` taken from 7f192e3cd316ba58c88dfa26796cf77789dd9872
struct clone_args {
	__aligned_u64 flags;
	__aligned_u64 pidfd;
	__aligned_u64 child_tid;
	__aligned_u64 parent_tid;
	__aligned_u64 exit_signal;
	__aligned_u64 stack;
	__aligned_u64 stack_size;
	__aligned_u64 tls;
};

// <asm/unistd.h> => <bits.syscall.h>
#define SYS_clone3 435
