/** Math functions not provided by RTAI's rtai_math library, but that we would still like to have. */
#ifndef EXTRA_MATHFUNCS_H
#define EXTRA_MATHFUNCS_H
extern double pow(double, double); /**< provided by rtmath */

extern double exp2(double x);
extern double exp10(double x);
extern double round(double); /**< this one is provided by rt_math, but somehow doesn't show up in the header */
extern double expn(int i, double d);
extern double log2(double d);
extern double powi(double d, int i);
extern double fac(int i);
extern int isnan(double x);
#endif
