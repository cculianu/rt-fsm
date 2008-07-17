# define JB_BX  0
# define JB_SI  1
# define JB_DI  2
# define JB_BP  3
# define JB_SP  4
# define JB_PC  5
# define JB_SIZE 24


.global setjmp
.type   setjmp,%function
.align 4
setjmp:
        xorl %eax, %eax
        movl 4   (%esp), %edx

        /* Save registers.  */
        movl %ebx, (JB_BX*4)(%edx)
        movl %esi, (JB_SI*4)(%edx)
        movl %edi, (JB_DI*4)(%edx)
        leal 4 (%esp), %ecx     /* Save SP as it will be after we return.  */
        movl %ecx, (JB_SP*4)(%edx)
        movl 0 (%esp), %ecx     /* Save PC we are returning to now.  */
        movl %ecx, (JB_PC*4)(%edx)
        movl %ebp, (JB_BP*4)(%edx) /* Save caller's frame pointer.  */

        movl %eax, JB_SIZE(%edx) /* No signal mask set.  */
        ret
.size setjmp,.-setjmp


.global longjmp
.type   longjmp,%function
.align 4
longjmp:
        movl 4(%esp), %ecx      /* User's jmp_buf in %ecx.  */
        movl 8(%esp), %eax      /* Second argument is return value.  */
        /* Save the return address now.  */
        movl (JB_PC*4)(%ecx), %edx
        /* Restore registers.  */
        movl (JB_BX*4)(%ecx), %ebx
        movl (JB_SI*4)(%ecx), %esi
        movl (JB_DI*4)(%ecx), %edi
        movl (JB_BP*4)(%ecx), %ebp
        movl (JB_SP*4)(%ecx), %esp
        /* Jump to saved PC.  */
        jmp *%edx
.size longjmp,.-longjmp
