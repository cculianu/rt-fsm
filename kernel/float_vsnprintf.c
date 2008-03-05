/*
 * Copyright Patrick Powell 1995
 * This code is based on code written by Patrick Powell (papowell@astart.com)
 * It may be used for any purpose as long as this notice remains intact
 * on all source code distributions
 *
 * Adapted for use in linux kernel, and made to print *only* floats/doubles by 
 * Calin Culianu <calin@ajvar.org> for his custom EmbC FSM code! March, 2008
 */
#ifdef __KERNEL__
#  include <linux/string.h>
#  include <linux/ctype.h>
#  include <linux/types.h>
#else
#  include <string.h>
#  include <ctype.h>
#  include <sys/types.h>
#endif

#include <stdarg.h>

#ifndef NULL
#define NULL 0
#endif

#define LDOUBLE long double
#define LLONG long long
#define VA_COPY va_copy

/*
 * dopr(): poor man's version of doprintf
 */

/* format read states */
#define DP_S_DEFAULT 0
#define DP_S_FLAGS   1
#define DP_S_MIN     2
#define DP_S_DOT     3
#define DP_S_MAX     4
#define DP_S_MOD     5
#define DP_S_CONV    6
#define DP_S_DONE    7

/* format flags - Bits */
#define DP_F_MINUS      (1 << 0)
#define DP_F_PLUS       (1 << 1)
#define DP_F_SPACE      (1 << 2)
#define DP_F_NUM        (1 << 3)
#define DP_F_ZERO       (1 << 4)
#define DP_F_UP         (1 << 5)
#define DP_F_UNSIGNED   (1 << 6)

/* Conversion Flags */
#define DP_C_SHORT   1
#define DP_C_LONG    2
#define DP_C_LDOUBLE 3
#define DP_C_LLONG   4

#define char_to_int(p) ((p)- '0')
#ifndef MAX
#define MAX(p,q) (((p) >= (q)) ? (p) : (q))
#endif


static size_t dopr(char *buffer, size_t maxlen, const char *format, 
                   va_list args_in);
static void fmtfp(char *buffer, size_t *currlen, size_t maxlen,
                   LDOUBLE fvalue, int min, int max, int flags);
static void dopr_outch(char *buffer, size_t *currlen, size_t maxlen, char c);

static size_t dopr(char *buffer, size_t maxlen, const char *format, va_list args_in)
{
        char ch;
        LDOUBLE fvalue;
        int min;
        int max;
        int state;
        int flags;
        int cflags;
        size_t currlen;
        const char *pctpos = format;
        va_list args;

        VA_COPY(args, args_in);
        
        state = DP_S_DEFAULT;
        currlen = flags = cflags = min = 0;
        max = -1;
        ch = *format++;
        
        while (state != DP_S_DONE) {
                if (ch == '\0') 
                        state = DP_S_DONE;

                switch(state) {
                case DP_S_DEFAULT:
                        if (ch == '%') {
                                state = DP_S_FLAGS;
                                pctpos = format-1;
                        } else 
                                dopr_outch (buffer, &currlen, maxlen, ch);
                        ch = *format++;
                        break;
                case DP_S_FLAGS:
                        switch (ch) {
                        case '-':
                                flags |= DP_F_MINUS;
                                ch = *format++;
                                break;
                        case '+':
                                flags |= DP_F_PLUS;
                                ch = *format++;
                                break;
                        case ' ':
                                flags |= DP_F_SPACE;
                                ch = *format++;
                                break;
                        case '#':
                                flags |= DP_F_NUM;
                                ch = *format++;
                                break;
                        case '0':
                                flags |= DP_F_ZERO;
                                ch = *format++;
                                break;
                        default:
                                state = DP_S_MIN;
                                break;
                        }
                        break;
                case DP_S_MIN:
                        if (isdigit((unsigned char)ch)) {
                                min = 10*min + char_to_int (ch);
                                ch = *format++;
                        } /* else if (ch == '*') { */
/*                                 min = va_arg (args, int); */
/*                                 ch = *format++; */
/*                                 state = DP_S_DOT; */
/*                         } */ else {
                                state = DP_S_DOT;
                        }
                        break;
                case DP_S_DOT:
                        if (ch == '.') {
                                state = DP_S_MAX;
                                ch = *format++;
                        } else { 
                                state = DP_S_MOD;
                        }
                        break;
                case DP_S_MAX:
                        if (isdigit((unsigned char)ch)) {
                                if (max < 0)
                                        max = 0;
                                max = 10*max + char_to_int (ch);
                                ch = *format++;
                        }/*  else if (ch == '*') { */
/*                                 max = va_arg (args, int); */
/*                                 ch = *format++; */
/*                                 state = DP_S_MOD; */
/*                         } */ else {
                                state = DP_S_MOD;
                        }
                        break;
                case DP_S_MOD:
                        switch (ch) {
                        case 'L':
                                cflags = DP_C_LDOUBLE;
                                ch = *format++;
                                break;
                        default:
                                break;
                        }
                        state = DP_S_CONV;
                        break;
                case DP_S_CONV:
                        switch (ch) {
                        case 'f':
                                if (cflags == DP_C_LDOUBLE)
                                        fvalue = va_arg (args, LDOUBLE);
                                else
                                        fvalue = va_arg (args, double);
                                /* um, floating point? */
                                fmtfp (buffer, &currlen, maxlen, fvalue, min, max, flags);
                                break;
                        case 'E':
                                flags |= DP_F_UP;
                        case 'e':
                                if (cflags == DP_C_LDOUBLE)
                                        fvalue = va_arg (args, LDOUBLE);
                                else
                                        fvalue = va_arg (args, double);
                                fmtfp (buffer, &currlen, maxlen, fvalue, min, max, flags);
                                break;
                        case 'G':
                                flags |= DP_F_UP;
                        case 'g':
                                if (cflags == DP_C_LDOUBLE)
                                        fvalue = va_arg (args, LDOUBLE);
                                else
                                        fvalue = va_arg (args, double);
                                fmtfp (buffer, &currlen, maxlen, fvalue, min, max, flags);
                                break;
                        default:
                            while (pctpos < format)
                                /* unknown, so just spit out the whole format string again.. */
                                dopr_outch(buffer, &currlen, maxlen, *pctpos++);
                                break;
                        }
                        ch = *format++;
                        state = DP_S_DEFAULT;
                        flags = cflags = min = 0;
                        max = -1;
                        break;
                case DP_S_DONE:
                        break;
                default:
                        /* hmm? */
                        break; /* some picky compilers need this */
                }
        }
        if (maxlen != 0) {
                if (currlen < maxlen - 1) 
                        buffer[currlen] = '\0';
                else if (maxlen > 0) 
                        buffer[maxlen - 1] = '\0';
        }
        
        return currlen;
}

static LDOUBLE abs_val(LDOUBLE value)
{
        LDOUBLE result = value;

        if (value < 0)
                result = -value;
        
        return result;
}

static LDOUBLE POW10(int exp)
{
        LDOUBLE result = 1;
        
        while (exp) {
                result *= 10;
                exp--;
        }
  
        return result;
}

static LLONG ROUND(LDOUBLE value)
{
        LLONG intpart;

        intpart = (LLONG)value;
        value = value - intpart;
        if (value >= 0.5) ++intpart;
        if (value <= -0.5) --intpart;
        
        return intpart;
}

/* a replacement for modf that doesn't need the math library. Should
   be portable, but slow */
static double my_modf(double x0, double *iptr)
{
        int i;
        long l;
        double x = x0;
        double f = 1.0;

        for (i=0;i<100;i++) {
                l = (long)x;
                if (l <= (x+1) && l >= (x-1)) break;
                x *= 0.1;
                f *= 10.0;
        }

        if (i == 100) {
                /* yikes! the number is beyond what we can handle. What do we do? */
                (*iptr) = 0;
                return 0;
        }

        if (i != 0) {
                double i2;
                double ret;

                ret = my_modf(x0-l*f, &i2);
                (*iptr) = l*f + i2;
                return ret;
        } 

        (*iptr) = l;
        return x - (*iptr);
}


static void fmtfp (char *buffer, size_t *currlen, size_t maxlen,
                   LDOUBLE fvalue, int min, int max, int flags)
{
        int signvalue = 0;
        double ufvalue;
        char iconvert[311];
        char fconvert[311];
        int iplace = 0;
        int fplace = 0;
        int padlen = 0; /* amount to pad */
        int zpadlen = 0; 
        int caps = 0;
        int idx;
        double intpart;
        double fracpart;
        double temp;
  
        /* 
         * AIX manpage says the default is 0, but Solaris says the default
         * is 6, and sprintf on AIX defaults to 6
         */
        if (max < 0)
                max = 6;

        ufvalue = abs_val (fvalue);

        if (fvalue < 0) {
                signvalue = '-';
        } else {
                if (flags & DP_F_PLUS) { /* Do a sign (+/i) */
                        signvalue = '+';
                } else {
                        if (flags & DP_F_SPACE)
                                signvalue = ' ';
                }
        }

#if 0
        if (flags & DP_F_UP) caps = 1; /* Should characters be upper case? */
#endif

#if 0
         if (max == 0) ufvalue += 0.5; /* if max = 0 we must round */
#endif

        /* 
         * Sorry, we only support 16 digits past the decimal because of our 
         * conversion method
         */
        if (max > 16)
                max = 16;

        /* We "cheat" by converting the fractional part to integer by
         * multiplying by a factor of 10
         */

        temp = ufvalue;
        my_modf(temp, &intpart);

        fracpart = ROUND((POW10(max)) * (ufvalue - intpart));
        
        if (fracpart >= POW10(max)) {
                intpart++;
                fracpart -= POW10(max);
        }


        /* Convert integer part */
        do {
                temp = intpart*0.1;
                my_modf(temp, &intpart);
                idx = (int) ((temp -intpart +0.05)* 10.0);
                /* idx = (int) (((double)(temp*0.1) -intpart +0.05) *10.0); */
                /* printf ("%llf, %f, %x\n", temp, intpart, idx); */
                iconvert[iplace++] =
                        (caps? "0123456789ABCDEF":"0123456789abcdef")[idx];
        } while (intpart && (iplace < 311));
        if (iplace == 311) iplace--;
        iconvert[iplace] = 0;

        /* Convert fractional part */
        if (fracpart)
        {
                do {
                        temp = fracpart*0.1;
                        my_modf(temp, &fracpart);
                        idx = (int) ((temp -fracpart +0.05)* 10.0);
                        /* idx = (int) ((((temp/10) -fracpart) +0.05) *10); */
                        /* printf ("%lf, %lf, %ld\n", temp, fracpart, idx ); */
                        fconvert[fplace++] =
                        (caps? "0123456789ABCDEF":"0123456789abcdef")[idx];
                } while(fracpart && (fplace < 311));
                if (fplace == 311) fplace--;
        }
        fconvert[fplace] = 0;
  
        /* -1 for decimal point, another -1 if we are printing a sign */
        padlen = min - iplace - max - 1 - ((signvalue) ? 1 : 0); 
        zpadlen = max - fplace;
        if (zpadlen < 0) zpadlen = 0;
        if (padlen < 0) 
                padlen = 0;
        if (flags & DP_F_MINUS) 
                padlen = -padlen; /* Left Justifty */
        
        if ((flags & DP_F_ZERO) && (padlen > 0)) {
                if (signvalue) {
                        dopr_outch (buffer, currlen, maxlen, signvalue);
                        --padlen;
                        signvalue = 0;
                }
                while (padlen > 0) {
                        dopr_outch (buffer, currlen, maxlen, '0');
                        --padlen;
                }
        }
        while (padlen > 0) {
                dopr_outch (buffer, currlen, maxlen, ' ');
                --padlen;
        }
        if (signvalue) 
                dopr_outch (buffer, currlen, maxlen, signvalue);
        
        while (iplace > 0) 
                dopr_outch (buffer, currlen, maxlen, iconvert[--iplace]);

        /*
         * Decimal point.  This should probably use locale to find the correct
         * char to print out.
         */
        if (max > 0) {
                dopr_outch (buffer, currlen, maxlen, '.');
                
                while (zpadlen > 0) {
                        dopr_outch (buffer, currlen, maxlen, '0');
                        --zpadlen;
                }

                while (fplace > 0) 
                        dopr_outch (buffer, currlen, maxlen, fconvert[--fplace]);
        }

        while (padlen < 0) {
                dopr_outch (buffer, currlen, maxlen, ' ');
                ++padlen;
        }
}

static void dopr_outch(char *buffer, size_t *currlen, size_t maxlen, char c)
{
        if (*currlen < maxlen) {
                buffer[(*currlen)] = c;
        }
        (*currlen)++;
}

 
int float_vsnprintf(char *str,size_t count,const char *fmt,va_list ap)
{
        return dopr(str,count,fmt,ap);
}
