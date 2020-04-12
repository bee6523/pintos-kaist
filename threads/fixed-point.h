#define FIXED_F (1<<14)
#define HALF_F (1<<13)
#define itof(n) (n*(FIXED_F))
//#define ftoi(f) ((f+(FIXED_F)/2)/FIXED_F)
#define ftoi(f) ((f>0)?((f+HALF_F)/FIXED_F):((f-HALF_F)/FIXED_F))
#define add_fi(f,n) (f+n*FIXED_F)
#define sub_fi(f,n) (f-n*FIXED_F)
#define mult_ff(f1,f2) (((int64_t) f1)*f2/FIXED_F)
#define mult_fi(f,n) (f*n)
#define div_ff(f1,f2) (((int64_t) f1)*FIXED_F/f2)
#define div_fi(f,n) (f/n)