/***************************************************************************
 *   Copyright (C) 2008 by Calin A. Culianu   *
 *   cculianu@yahoo.com   *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
#include "extra_mathfuncs.h"
#include <ieee754.h>
#define IBMPC 1
#define DENORMAL 1
extern double pow(double, double); /**< provided by rtmath */
extern double gamma ( double ); /**< provided by rtmath */
extern double log ( double ); /**< provided by rtmath */
extern double exp ( double ); /**< provided by rtmath */
extern double fabs ( double ); /**< provided by rtmath */
extern double frexp ( double, int * );
extern double ldexp ( double, int );
extern int isnan(double);
extern int signbit ( double );
#define mtherr(a,b) do {  } while(0)
static const unsigned short MACHEP_[4] = {0x0000,0x0000,0x0000,0x3ca0};
#define MACHEP (*(const double *)MACHEP_)
static const unsigned short MAXLOG_[4] = {0x39ef,0xfefa,0x2e42,0x4086};
#define MAXLOG (*(const double *)MAXLOG_)
static const unsigned short MINLOG_[4] = {0x3052,0xd52d,0x4910,0xc087};
#define MINLOG (*(const double *)MINLOG_)
static const unsigned short MAXNUM_[4] = {0xffff,0xffff,0xffff,0x7fef};
#define MAXNUM (*(const double *)MAXNUM_)
static const unsigned short INFINITY_[4] = {0x0000,0x0000,0x0000,0x7ff0};
#define INFINITY (*(const double *)INFINITY_)
static const unsigned short NAN_[4] = {0x0000,0x0000,0x0000,0x7ffc};
#define NAN (*(const double *)NAN_)
static const unsigned short NEGZERO_[4] = {0x0000,0x0000,0x0000,0x8000};
#define NEGZERO (*(const double *)NEGZERO_)
static const unsigned short LOGE2_[4]  = {0x39ef,0xfefa,0x2e42,0x3fe6};
#define LOGE2 (*(const double *)LOGE2_)

int isnan(double d)
{
    union ieee754_double *u = (union ieee754_double *)&d; 
    return u->ieee_nan.exponent == 2047 && (u->ieee.mantissa0 || u->ieee.mantissa1);
}

double fac(int i) 
{ 
    double ret = i; 
    while (--i > 0) ret *= i;  
    return ret; 
}

double exp2(double x) 
{ 
    return pow(2., x); 
}

double exp10(double x) 
{
   return pow(10., x); 
}

static double polevl( double x, double coef[], int N )
{
    double ans;
    int i;
    double *p;
    
    p = coef;
    ans = *p++;
    i = N;
    
    do
        ans = ans * x  +  *p++;
    while( --i );
    
    return( ans );
}

/*							p1evl()	*/
/*                                          N
 * Evaluate polynomial when coefficient of x  is 1.0.
 * Otherwise same as polevl.
 */

static double p1evl( double x, double coef[], int N )
{
    double ans;
    double *p;
    int i;
    
    p = coef;
    ans = x + *p++;
    i = N-1;
    
    do
        ans = ans * x  + *p++;
    while( --i );
    
    return( ans );
}

double expn( int n, double x )
{
#   define EUL 0.57721566490153286060
#   define BIG  1.44115188075855872E+17
    
    double ans, r, t, yk, xk;
    double pk, pkm1, pkm2, qk, qkm1, qkm2;
    double psi, z;
    int i, k;
    static double big = BIG;
    
    if( n < 0 )
        goto domerr;
    
    if( x < 0 )
        {
    domerr:	/*mtherr( "expn", DOMAIN );*/
        return( MAXNUM );
        }
    
    if( x > MAXLOG )
        return( 0.0 );
    
    if( x == 0.0 )
        {
        if( n < 2 )
            {
            /*mtherr( "expn", SING );*/
            return( MAXNUM );
            }
        else
            return( 1.0/(n-1.0) );
        }
    
    if( n == 0 )
        return( exp(-x)/x );
    
    /*							expn.c	*/
    /*		Expansion for large n		*/
    
    if( n > 5000 )
        {
        xk = x + n;
        yk = 1.0 / (xk * xk);
        t = n;
        ans = yk * t * (6.0 * x * x  -  8.0 * t * x  +  t * t);
        ans = yk * (ans + t * (t  -  2.0 * x));
        ans = yk * (ans + t);
        ans = (ans + 1.0) * exp( -x ) / xk;
        goto done;
        }
    
    if( x > 1.0 )
        goto cfrac;
    
    /*							expn.c	*/
    
    /*		Power series expansion		*/
    
    psi = -EUL - log(x);
    for( i=1; i<n; i++ )
        psi = psi + 1.0/i;
    
    z = -x;
    xk = 0.0;
    yk = 1.0;
    pk = 1.0 - n;
    if( n == 1 )
        ans = 0.0;
    else
        ans = 1.0/pk;
    do
        {
        xk += 1.0;
        yk *= z/xk;
        pk += 1.0;
        if( pk != 0.0 )
            {
            ans += yk/pk;
            }
        if( ans != 0.0 )
            t = fabs(yk/ans);
        else
            t = 1.0;
        }
    while( t > MACHEP );
    k = xk;
    t = n;
    r = n - 1;
    ans = (pow(z, r) * psi / gamma(t)) - ans;
    goto done;
    
    /*							expn.c	*/
    /*		continued fraction		*/
    cfrac:
    k = 1;
    pkm2 = 1.0;
    qkm2 = x;
    pkm1 = 1.0;
    qkm1 = x + n;
    ans = pkm1/qkm1;
    
    do
        {
        k += 1;
        if( k & 1 )
            {
            yk = 1.0;
            xk = n + (k-1)/2;
            }
        else
            {
            yk = x;
            xk = k/2;
            }
        pk = pkm1 * yk  +  pkm2 * xk;
        qk = qkm1 * yk  +  qkm2 * xk;
        if( qk != 0 )
            {
            r = pk/qk;
            t = fabs( (ans - r)/r );
            ans = r;
            }
        else
            t = 1.0;
        pkm2 = pkm1;
        pkm1 = pk;
        qkm2 = qkm1;
        qkm1 = qk;
    if( fabs(pk) > big )
            {
            pkm2 /= big;
            pkm1 /= big;
            qkm2 /= big;
            qkm1 /= big;
            }
        }
    while( t > MACHEP );
    
    ans *= exp( -x );
    
    done:
    return( ans );
}

double log2(double x) 
{ 
  /* Stuff for log2 */
  static unsigned short P[] = {
    0x1bb0,0x93c3,0xb4c2,0x3f1a,
    0x52f2,0x3f56,0xd6f5,0x3fdf,
    0x6911,0xed92,0xd2ba,0x4012,
    0xeb2e,0xc63e,0xff72,0x402c,
    0xc84d,0x924b,0xefd6,0x4031,
    0xdcf8,0x7d7e,0xd563,0x401e,
  };
  static unsigned short Q[] = {
    /*0x0000,0x0000,0x0000,0x3ff0,*/
    0xef8e,0xae97,0x9320,0x4026,
    0xc033,0x4e19,0x9d2c,0x4046,
    0xbdbd,0xa326,0xbf33,0x4054,
    0xae21,0xeb5e,0xc9e2,0x4051,
    0x25b2,0x9e1f,0x200a,0x4037,
  };
  static unsigned short L[5] = {0x0bf8,0x94ae,0x551d,0x3fdc};
#  define LOG2EA (*(double *)(&L[0]))
  static unsigned short R[12] = {
    0x0e84,0xdc6c,0x443d,0xbfe9,
    0x7b6b,0x7302,0x62fc,0x4030,
    0x2a20,0x1122,0x0906,0xc050,
  };
  static unsigned short S[12] = {
    /*0x0000,0x0000,0x0000,0x3ff0,*/
    0x6d0a,0x43ec,0xd60d,0xc041,
    0xe40e,0x112a,0x8180,0x4073,
    0x3f3b,0x19b3,0x0d89,0xc088,
  };
#  define SQRTH 0.70710678118654752440
    
    int e;
    double y;
    double z;
    #ifdef DEC
    short *q;
    #endif
    
    #ifdef NANS
    if( isnan(x) )
        return(x);
    #endif
    #ifdef INFINITIES
    if( x == INFINITY )
        return(x);
    #endif
    /* Test for domain */
    if( x <= 0.0 )
        {
        if( x == 0.0 )
                {
            mtherr( fname, SING );
            return( -INFINITY );
                }
        else
                {
            mtherr( fname, DOMAIN );
            return( NAN );
                }
        }
    
    /* separate mantissa from exponent */
    
    #ifdef DEC
    q = (short *)&x;
    e = *q;			/* short containing exponent */
    e = ((e >> 7) & 0377) - 0200;	/* the exponent */
    *q &= 0177;	/* strip exponent from x */
    *q |= 040000;	/* x now between 0.5 and 1 */
    #endif
    
    /* Note, frexp is used so that denormal numbers
    * will be handled properly.
    */
    #ifdef IBMPC
    x = frexp( x, &e );
    /*
    q = (short *)&x;
    q += 3;
    e = *q;
    e = ((e >> 4) & 0x0fff) - 0x3fe;
    *q &= 0x0f;
    *q |= 0x3fe0;
    */
    #endif
    
    /* Equivalent C language standard library function: */
    #ifdef UNK
    x = frexp( x, &e );
    #endif
    
    #ifdef MIEEE
    x = frexp( x, &e );
    #endif
    
    
    /* logarithm using log(x) = z + z**3 P(z)/Q(z),
    * where z = 2(x-1)/x+1)
    */
    
    if( (e > 2) || (e < -2) )
    {
    if( x < SQRTH )
        { /* 2( 2x-1 )/( 2x+1 ) */
        e -= 1;
        z = x - 0.5;
        y = 0.5 * z + 0.5;
        }
    else
        { /*  2 (x-1)/(x+1)   */
        z = x - 0.5;
        z -= 0.5;
        y = 0.5 * x  + 0.5;
        }
    
    x = z / y;
    z = x*x;
    y = x * ( z * polevl( z, (double *)R, 2 ) / p1evl( z, (double *)S, 3 ) );
    goto ldone;
    }
    
    
    
    /* logarithm using log(1+x) = x - .5x**2 + x**3 P(x)/Q(x) */
    
    if( x < SQRTH )
        {
        e -= 1;
        x = ldexp( x, 1 ) - 1.0; /*  2x - 1  */
        }
    else
        {
        x = x - 1.0;
        }
    
    z = x*x;
    #ifdef DEC
    y = x * ( z * polevl( x, P, 5 ) / p1evl( x, Q, 6 ) ) - ldexp( z, -1 );
    #else
    y = x * ( z * polevl( x, (double *)P, 5 ) / p1evl( x, (double *)Q, 5 ) ) - ldexp( z, -1 );
    #endif
    
    ldone:
    
    /* Multiply log of fraction by log2(e)
    * and base 2 exponent by 1
    *
    * ***CAUTION***
    *
    * This sequence of operations is critical and it may
    * be horribly defeated by some compiler optimizers.
    */
    z = y * LOG2EA;
    z += x * LOG2EA;
    z += y;
    z += x;
    z += e;
    return( z );
}

double powi(double x, int nn) 
{ 
    int n, e, sign, asign, lx;
    double w, y, s;
    
    /* See pow.c for these tests.  */
    if( x == 0.0 )
        {
        if( nn == 0 )
            return( 1.0 );
        else if( nn < 0 )
            return( INFINITY );
        else
          {
            if( nn & 1 )
              return( x );
            else
              return( 0.0 );
          }
        }
    
    if( nn == 0 )
        return( 1.0 );
    
    if( nn == -1 )
        return( 1.0/x );
    
    if( x < 0.0 )
        {
        asign = -1;
        x = -x;
        }
    else
        asign = 0;
    
    
    if( nn < 0 )
        {
        sign = -1;
        n = -nn;
        }
    else
        {
        sign = 1;
        n = nn;
        }
    
    /* Even power will be positive. */
    if( (n & 1) == 0 )
        asign = 0;
    
    /* Overflow detection */
    
    /* Calculate approximate logarithm of answer */
    s = frexp( x, &lx );
    e = (lx - 1)*n;
    if( (e == 0) || (e > 64) || (e < -64) )
        {
        s = (s - 7.0710678118654752e-1) / (s +  7.0710678118654752e-1);
        s = (2.9142135623730950 * s - 0.5 + lx) * nn * LOGE2;
        }
    else
        {
        s = LOGE2 * e;
        }
    
    if( s > MAXLOG )
        {
        mtherr( "powi", OVERFLOW );
        y = INFINITY;
        goto done;
        }
    
    #if DENORMAL
    if( s < MINLOG )
        {
        y = 0.0;
        goto done;
        }
    
    /* Handle tiny denormal answer, but with less accuracy
    * since roundoff error in 1.0/x will be amplified.
    * The precise demarcation should be the gradual underflow threshold.
    */
    if( (s < (-MAXLOG+2.0)) && (sign < 0) )
        {
        x = 1.0/x;
        sign = -sign;
        }
    #else
    /* do not produce denormal answer */
    if( s < -MAXLOG )
        return(0.0);
    #endif
    
    
    /* First bit of the power */
    if( n & 1 )
        y = x;
    
    else
        y = 1.0;
    
    w = x;
    n >>= 1;
    while( n )
        {
        w = w * w;	/* arg to the 2-to-the-kth power */
        if( n & 1 )	/* if that bit is set, then include in product */
            y *= w;
        n >>= 1;
        }
    
    if( sign < 0 )
        y = 1.0/y;
    
    done:
    
    if( asign )
        {
        /* odd power of negative number */
        if( y == 0.0 )
            y = NEGZERO;
        else
            y = -y;
        }
    return(y);
}

double round(double d)
{
    double ret = d - (long)d;
    if (ret >= 0.5) return (long)(d+0.5);
    if (ret <= -0.5) return (long)(d-0.5);
    return (long)d;
}
