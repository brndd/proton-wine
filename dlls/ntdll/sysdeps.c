/*
 * System-dependent scheduler support
 *
 * Copyright 1998 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"
#include "wine/port.h"

#include <signal.h>
#include <stdio.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif
#ifdef HAVE_SYS_SYSCALL_H
# include <sys/syscall.h>
#endif
#ifdef HAVE_SYS_LWP_H
# include <sys/lwp.h>
#endif
#ifdef HAVE_UCONTEXT_H
# include <ucontext.h>
#endif
#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif
#ifdef HAVE_SCHED_H
#include <sched.h>
#endif
#ifdef HAVE_VALGRIND_MEMCHECK_H
#include <valgrind/memcheck.h>
#endif

#ifdef HAVE_NPTL
#include <pthread.h>
#endif

#include "thread.h"
#include "wine/server.h"
#include "winbase.h"
#include "wine/library.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(thread);

struct thread_cleanup_info
{
    void *stack_base;
    int   stack_size;
    int   status;
};

/* temporary stacks used on thread exit */
#define TEMP_STACK_SIZE 1024
#define NB_TEMP_STACKS  8
static char temp_stacks[NB_TEMP_STACKS][TEMP_STACK_SIZE];
static LONG next_temp_stack;  /* next temp stack to use */

/***********************************************************************
 *           SYSDEPS_SetCurThread
 *
 * Make 'thread' the current thread.
 */
void SYSDEPS_SetCurThread( TEB *teb )
{
#if defined(__i386__)
    /* On the i386, the current thread is in the %fs register */
    LDT_ENTRY fs_entry;

    wine_ldt_set_base( &fs_entry, teb );
    wine_ldt_set_limit( &fs_entry, 0xfff );
    wine_ldt_set_flags( &fs_entry, WINE_LDT_FLAGS_DATA|WINE_LDT_FLAGS_32BIT );
    wine_ldt_init_fs( teb->teb_sel, &fs_entry );
#elif defined(__powerpc__)
    /* On PowerPC, the current TEB is in the gpr13 register */
# ifdef __APPLE__
    __asm__ __volatile__("mr r13, %0" : : "r" (teb));
# else
    __asm__ __volatile__("mr 2, %0" : : "r" (teb));
# endif
#elif defined(HAVE__LWP_CREATE)
    /* On non-i386 Solaris, we use the LWP private pointer */
    _lwp_setprivate( teb );
#endif

#ifdef HAVE_NPTL
    teb->pthread_data = (void *)pthread_self();
#endif
}


/***********************************************************************
 *           get_temp_stack
 *
 * Get a temporary stack address to run the thread exit code on.
 */
inline static char *get_temp_stack(void)
{
    unsigned int next = interlocked_xchg_add( &next_temp_stack, 1 );
    return temp_stacks[next % NB_TEMP_STACKS];
}


/***********************************************************************
 *           cleanup_thread
 *
 * Cleanup the remains of a thread. Runs on a temporary stack.
 */
static void cleanup_thread( void *ptr )
{
    /* copy the info structure since it is on the stack we will free */
    struct thread_cleanup_info info = *(struct thread_cleanup_info *)ptr;
    munmap( info.stack_base, info.stack_size );
    wine_ldt_free_fs( wine_get_fs() );
#ifdef HAVE__LWP_CREATE
    _lwp_exit();
#endif
    _exit( info.status );
}


/***********************************************************************
 *           SYSDEPS_StartThread
 *
 * Startup routine for a new thread.
 */
static void SYSDEPS_StartThread( TEB *teb )
{
    SYSDEPS_SetCurThread( teb );
    SIGNAL_Init();
    CLIENT_InitThread();
    teb->startup();
    SYSDEPS_ExitThread(0);  /* should never get here */
}


/***********************************************************************
 *           SYSDEPS_SpawnThread
 *
 * Start running a new thread.
 * Return -1 on error, 0 if OK.
 */
int SYSDEPS_SpawnThread( TEB *teb )
{
#ifdef HAVE_NPTL
    pthread_t id;
    pthread_attr_t attr;

    pthread_attr_init( &attr );
    pthread_attr_setstack( &attr, teb->stack_base, (char *)teb->stack_top - (char *)teb->stack_base );
    if (pthread_create( &id, &attr, (void * (*)(void *))SYSDEPS_StartThread, teb )) return -1;
    return 0;
#elif defined(HAVE_CLONE)
    if (clone( (int (*)(void *))SYSDEPS_StartThread, teb->stack_top,
               CLONE_VM | CLONE_FS | CLONE_FILES | SIGCHLD, teb ) < 0)
        return -1;
    return 0;
#elif defined(HAVE_RFORK)
    void **sp = (void **)teb->stack_top;
    *--sp = teb;
    *--sp = 0;
    *--sp = SYSDEPS_StartThread;
    __asm__ __volatile__(
    "pushl %2;\n\t"		/* flags */
    "pushl $0;\n\t"		/* 0 ? */
    "movl %1,%%eax;\n\t"	/* SYS_rfork */
    ".byte 0x9a; .long 0; .word 7;\n\t"	/* lcall 7:0... FreeBSD syscall */
    "cmpl $0, %%edx;\n\t"
    "je 1f;\n\t"
    "movl %0,%%esp;\n\t"	/* child -> new thread */
    "ret;\n"
    "1:\n\t"		/* parent -> caller thread */
    "addl $8,%%esp" :
    : "r" (sp), "g" (SYS_rfork), "g" (RFPROC | RFMEM)
    : "eax", "edx");
    return 0;
#elif defined(HAVE__LWP_CREATE)
    ucontext_t context;
    _lwp_makecontext( &context, (void(*)(void *))SYSDEPS_StartThread, teb,
                      NULL, teb->stack_base, (char *)teb->stack_top - (char *)teb->stack_base );
    if ( _lwp_create( &context, 0, NULL ) )
        return -1;
    return 0;
#endif

    FIXME("CreateThread: stub\n" );
    return -1;
}


/***********************************************************************
 *           SYSDEPS_SwitchToThreadStack
 *
 * Switch to the stack specified in the current thread TEB
 * and call the specified function.
 */
void DECLSPEC_NORETURN SYSDEPS_SwitchToThreadStack( void (*func)(void *), void *arg );
#ifdef __i386__
#  ifdef __GNUC__
__ASM_GLOBAL_FUNC( SYSDEPS_SwitchToThreadStack,
                   "movl 4(%esp),%ecx\n\t"  /* func */
                   "movl 8(%esp),%edx\n\t"  /* arg */
                   ".byte 0x64\n\tmovl 0x04,%esp\n\t"  /* teb->stack_top */
                   "pushl %edx\n\t"
                   "xorl %ebp,%ebp\n\t"
                   "call *%ecx\n\t"
                   "int $3" /* we never return here */ );
#  elif defined(_MSC_VER)
__declspec(naked) void SYSDEPS_SwitchToThreadStack( void (*func)(void *), void *arg )
{
  __asm mov ecx, 4[esp];
  __asm mov edx, 8[esp];
  __asm mov fs:[0x04], esp;
  __asm push edx;
  __asm xor ebp, ebp;
  __asm call [ecx];
  __asm int 3;
}
#  endif /* defined(__GNUC__) || defined(_MSC_VER) */
#elif defined(__sparc__) && defined(__GNUC__)
__ASM_GLOBAL_FUNC( SYSDEPS_SwitchToThreadStack,
                   "mov %o0, %l0\n\t" /* store first argument */
                   "call " __ASM_NAME("NtCurrentTeb") ", 0\n\t"
                   "mov %o1, %l1\n\t" /* delay slot: store second argument */
                   "ld [%o0+4], %sp\n\t" /* teb->stack_top */
                   "call %l0, 0\n\t" /* call func */
                   "mov %l1, %o0\n\t" /* delay slot:  arg for func */
                   "ta 0x01\n\t"); /* breakpoint - we never get here */
#elif defined(__powerpc__) && defined(__APPLE__)
/* Darwin SYSDEPS_SwitchToThreadStack
 Function Pointer to call is on r3, Args to pass on r4 and stack on r1 */
__ASM_GLOBAL_FUNC( SYSDEPS_SwitchToThreadStack,
                   "stw r1, 0x4(r13)\n\t" /* teb->stack_top */
                   "mr r12,r3\n\t"
                   "mtctr r12\n\t" /* func->ctr */
                   "mr r3,r4\n\t" /* args->function param 1 (r3) */
                   "bctr\n\t" /* call ctr */
                   "b _SYSDEPS_SwitchToThreadStack+24\n\t"); /* loop */
#elif defined(__powerpc__) && defined(__GNUC__)
/* Linux SYSDEPS_SwitchToThreadStack
 Function Pointer to call is on r3, Args to pass on r4 and stack on r1 */
__ASM_GLOBAL_FUNC( SYSDEPS_SwitchToThreadStack,
                   "stw 1, 0x4(13)\n\t" /* teb->stack_top */
                   "mr 12,3\n\t"
                   "mtctr 12\n\t" /* func->ctr */
                   "mr 3,4\n\t" /* args->function param 1 (r3) */
                   "bctr\n\t" /* call ctr */
                   "b _SYSDEPS_SwitchToThreadStack+24\n\t"); /* loop */
#else  /* !powerpc, !sparc, !i386 */
void SYSDEPS_SwitchToThreadStack( void (*func)(void *), void *arg )
{
    func( arg );
    while(1); /* avoid warning */
}
#endif /* !defined(__i386__) && !defined(__sparc__) */


/***********************************************************************
 *           SYSDEPS_ExitThread
 *
 * Exit a running thread; must not return.
 */
void SYSDEPS_ExitThread( int status )
{
    TEB *teb = NtCurrentTeb();
    DWORD size = 0;

#ifdef HAVE_NPTL
    static TEB *teb_to_free;
    TEB *free_teb;

    if ((free_teb = interlocked_xchg_ptr( (void **)&teb_to_free, teb )) != NULL)
    {
        void *ptr;

        TRACE("freeing prev teb %p stack %p fs %04x\n",
              free_teb, free_teb->stack_base, free_teb->teb_sel );

        pthread_join( (pthread_t)free_teb->pthread_data, &ptr );
        wine_ldt_free_fs( free_teb->teb_sel );
        ptr = free_teb->stack_base;
        NtFreeVirtualMemory( GetCurrentProcess(), &ptr, &size, MEM_RELEASE );
    }
    SIGNAL_Block();
    SYSDEPS_AbortThread( status );
#else
    struct thread_cleanup_info info;
    MEMORY_BASIC_INFORMATION meminfo;

    NtQueryVirtualMemory( GetCurrentProcess(), teb->stack_top, MemoryBasicInformation,
                          &meminfo, sizeof(meminfo), NULL );
    info.stack_base = meminfo.AllocationBase;
    info.stack_size = meminfo.RegionSize + ((char *)teb->stack_top - (char *)meminfo.AllocationBase);
    info.status     = status;

    SIGNAL_Block();
    size = 0;
    NtFreeVirtualMemory( GetCurrentProcess(), &teb->stack_base, &size, MEM_RELEASE | MEM_SYSTEM );
    close( teb->wait_fd[0] );
    close( teb->wait_fd[1] );
    close( teb->reply_fd );
    close( teb->request_fd );
    SIGNAL_Reset();
    teb->stack_low = get_temp_stack();
    teb->stack_top = (char *) teb->stack_low + TEMP_STACK_SIZE;
    SYSDEPS_SwitchToThreadStack( cleanup_thread, &info );
#endif
}


/***********************************************************************
 *           SYSDEPS_AbortThread
 *
 * Same as SYSDEPS_ExitThread, but must not do anything that requires a server call.
 */
void SYSDEPS_AbortThread( int status )
{
    SIGNAL_Block();
    close( NtCurrentTeb()->wait_fd[0] );
    close( NtCurrentTeb()->wait_fd[1] );
    close( NtCurrentTeb()->reply_fd );
    close( NtCurrentTeb()->request_fd );
#ifdef HAVE_NPTL
    pthread_exit( (void *)status );
#endif
    SIGNAL_Reset();
#ifdef HAVE__LWP_CREATE
    _lwp_exit();
#endif
    for (;;)  /* avoid warning */
        _exit( status );
}

/***********************************************************************
 *           SYSDEPS_GetUnixTid
 *
 * Get the Unix tid of the current thread.
 */
int SYSDEPS_GetUnixTid(void)
{
#ifdef HAVE__LWP_SELF
    return _lwp_self();
#elif defined(__linux__) && defined(__i386__)
    int ret;
    __asm__("int $0x80" : "=a" (ret) : "0" (224) /* SYS_gettid */);
    if (ret < 0) ret = -1;
    return ret;
#else
    return -1;
#endif
}


#ifndef HAVE_NPTL

/* default errno before threading is initialized */
static int *default_errno_location(void)
{
    static int static_errno;
    return &static_errno;
}

/* default h_errno before threading is initialized */
static int *default_h_errno_location(void)
{
    static int static_h_errno;
    return &static_h_errno;
}

/* errno once threading is working */
static int *thread_errno_location(void)
{
    return &NtCurrentTeb()->thread_errno;
}

/* h_errno once threading is working */
static int *thread_h_errno_location(void)
{
    return &NtCurrentTeb()->thread_h_errno;
}

static int* (*errno_location_ptr)(void) = default_errno_location;
static int* (*h_errno_location_ptr)(void) = default_h_errno_location;

/***********************************************************************
 *           __errno_location/__error/__errno/___errno/__thr_errno
 *
 * Get the per-thread errno location.
 */
int *__errno_location(void) { return errno_location_ptr(); }  /* Linux */
int *__error(void)          { return errno_location_ptr(); }  /* FreeBSD */
int *__errno(void)          { return errno_location_ptr(); }  /* NetBSD */
int *___errno(void)         { return errno_location_ptr(); }  /* Solaris */
int *__thr_errno(void)      { return errno_location_ptr(); }  /* UnixWare */

/***********************************************************************
 *           __h_errno_location
 *
 * Get the per-thread h_errno location.
 */
int *__h_errno_location(void)
{
    return h_errno_location_ptr();
}

#endif  /* HAVE_NPTL */


#if defined(__linux__) && defined(__i386__)
static inline void writejump( const char *symbol, void *dest )
{
    unsigned char *addr = wine_dlsym( RTLD_NEXT, symbol, NULL, 0 );

    if (!addr) return;

    /* write a relative jump at the function address */
    mprotect((void*)((unsigned int)addr & ~(getpagesize()-1)), 5, PROT_READ|PROT_EXEC|PROT_WRITE);
    addr[0] = 0xe9;
    *(int *)(addr+1) = (unsigned char *)dest - (addr + 5);
    mprotect((void*)((unsigned int)addr & ~(getpagesize()-1)), 5, PROT_READ|PROT_EXEC);

#ifdef HAVE_VALGRIND_MEMCHECK_H
    VALGRIND_DISCARD_TRANSLATIONS( addr, 5 );
#endif
}
#endif

/***********************************************************************
 *           SYSDEPS_InitErrno
 *
 * Initialize errno handling.
 */
void SYSDEPS_InitErrno(void)
{
#ifndef HAVE_NPTL
    errno_location_ptr = thread_errno_location;
    h_errno_location_ptr = thread_h_errno_location;

# if defined(__linux__) && defined(__i386__)
    writejump( "__errno_location", thread_errno_location );
    writejump( "__h_errno_location", thread_h_errno_location );
# endif
#endif  /* HAVE_NPTL */
}


/**********************************************************************
 *           NtCurrentTeb   (NTDLL.@)
 *
 * This will crash and burn if called before threading is initialized
 */
#if defined(__i386__) && defined(__GNUC__)
__ASM_GLOBAL_FUNC( NtCurrentTeb, ".byte 0x64\n\tmovl 0x18,%eax\n\tret" );
#elif defined(__i386__) && defined(_MSC_VER)
/* Nothing needs to be done. MS C "magically" exports the inline version from winnt.h */
#elif defined(HAVE__LWP_CREATE)
/***********************************************************************
 *		NtCurrentTeb (NTDLL.@)
 */
struct _TEB * WINAPI NtCurrentTeb(void)
{
    extern void *_lwp_getprivate(void);
    return (struct _TEB *)_lwp_getprivate();
}
#elif defined(__powerpc__)
# ifdef __APPLE__
__ASM_GLOBAL_FUNC( NtCurrentTeb, "\n\tmr r3,r13\n\tblr" );
# else
__ASM_GLOBAL_FUNC( NtCurrentTeb, "\n\tmr 3,2\n\tblr" );
# endif
#else
# error NtCurrentTeb not defined for this architecture
#endif  /* __i386__ */
