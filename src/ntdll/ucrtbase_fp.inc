/* ucrtbase_fp.inc - floating-point classification and control (included by ucrtbase.c).
 *
 * The control word the runtime reports is the Microsoft one (_MCW_EM, _MCW_RC, _MCW_DN) mapped onto MXCSR, which is the
 * state the SSE arithmetic of x64 code uses. The x87 precision and infinity controls (_MCW_PC, _MCW_IC) do not exist on
 * x64 and are refused by _controlfp_s, as they are by the Microsoft runtime. The pure mapping functions come first so that
 * the host tests can run them; only the MXCSR access is machine-specific.
 */

/* Microsoft control word bits */
#define UCT_EM_INEXACT 0x00000001u
#define UCT_EM_UNDERFLOW 0x00000002u
#define UCT_EM_OVERFLOW 0x00000004u
#define UCT_EM_ZERODIVIDE 0x00000008u
#define UCT_EM_INVALID 0x00000010u
#define UCT_EM_DENORMAL 0x00080000u
#define UCT_MCW_EM 0x0008001Fu
#define UCT_MCW_RC 0x00000300u
#define UCT_MCW_DN 0x03000000u
#define UCT_MCW_PC 0x00030000u
#define UCT_MCW_IC 0x00040000u

/* MXCSR bits */
#define UCT_MX_IE 0x0001u /* flags: invalid, denormal, divide by zero, overflow, underflow, precision */
#define UCT_MX_DE 0x0002u
#define UCT_MX_ZE 0x0004u
#define UCT_MX_OE 0x0008u
#define UCT_MX_UE 0x0010u
#define UCT_MX_PE 0x0020u
#define UCT_MX_DAZ 0x0040u
#define UCT_MX_IM 0x0080u /* masks, in the same order, one bit up from the flags' positions */
#define UCT_MX_DM 0x0100u
#define UCT_MX_ZM 0x0200u
#define UCT_MX_OM 0x0400u
#define UCT_MX_UM 0x0800u
#define UCT_MX_PM 0x1000u
#define UCT_MX_FZ 0x8000u

/* The status flags of an MXCSR value as the _SW_* bits (the same values as the _EM_* ones). */
static unsigned uct_mx_to_status(unsigned mx)
{
    unsigned s = 0;
    if (mx & UCT_MX_IE)
        s |= UCT_EM_INVALID;
    if (mx & UCT_MX_DE)
        s |= UCT_EM_DENORMAL;
    if (mx & UCT_MX_ZE)
        s |= UCT_EM_ZERODIVIDE;
    if (mx & UCT_MX_OE)
        s |= UCT_EM_OVERFLOW;
    if (mx & UCT_MX_UE)
        s |= UCT_EM_UNDERFLOW;
    if (mx & UCT_MX_PE)
        s |= UCT_EM_INEXACT;
    return s;
}

static unsigned uct_status_to_mx(unsigned s)
{
    unsigned mx = 0;
    if (s & UCT_EM_INVALID)
        mx |= UCT_MX_IE;
    if (s & UCT_EM_DENORMAL)
        mx |= UCT_MX_DE;
    if (s & UCT_EM_ZERODIVIDE)
        mx |= UCT_MX_ZE;
    if (s & UCT_EM_OVERFLOW)
        mx |= UCT_MX_OE;
    if (s & UCT_EM_UNDERFLOW)
        mx |= UCT_MX_UE;
    if (s & UCT_EM_INEXACT)
        mx |= UCT_MX_PE;
    return mx;
}

/* The control word (exception masks, rounding, denormal handling) of an MXCSR value. */
static unsigned uct_mx_to_control(unsigned mx)
{
    unsigned c = 0;
    if (mx & UCT_MX_IM)
        c |= UCT_EM_INVALID;
    if (mx & UCT_MX_DM)
        c |= UCT_EM_DENORMAL;
    if (mx & UCT_MX_ZM)
        c |= UCT_EM_ZERODIVIDE;
    if (mx & UCT_MX_OM)
        c |= UCT_EM_OVERFLOW;
    if (mx & UCT_MX_UM)
        c |= UCT_EM_UNDERFLOW;
    if (mx & UCT_MX_PM)
        c |= UCT_EM_INEXACT;
    c |= ((mx >> 13) & 3u) << 8; /* rounding: 0 nearest, 1 down, 2 up, 3 toward zero */
    if ((mx & UCT_MX_FZ) && (mx & UCT_MX_DAZ))
        c |= 0x01000000u; /* _DN_FLUSH */
    else if (mx & UCT_MX_DAZ)
        c |= 0x02000000u; /* _DN_FLUSH_OPERANDS_SAVE_RESULTS */
    else if (mx & UCT_MX_FZ)
        c |= 0x03000000u; /* _DN_SAVE_OPERANDS_FLUSH_RESULT */
    return c;
}

/* Replaces the control bits of mx selected by `mask` (a Microsoft mask) with those of `value`. */
static unsigned uct_control_to_mx(unsigned mx, unsigned value, unsigned mask)
{
    static const struct {
        unsigned em, mx;
    } em[] = {{UCT_EM_INVALID, UCT_MX_IM},  {UCT_EM_DENORMAL, UCT_MX_DM}, {UCT_EM_ZERODIVIDE, UCT_MX_ZM},
              {UCT_EM_OVERFLOW, UCT_MX_OM}, {UCT_EM_UNDERFLOW, UCT_MX_UM}, {UCT_EM_INEXACT, UCT_MX_PM}};
    for (size_t i = 0; i < sizeof em / sizeof em[0]; ++i) {
        if (mask & em[i].em) {
            if (value & em[i].em)
                mx |= em[i].mx;
            else
                mx &= ~em[i].mx;
        }
    }
    if ((mask & UCT_MCW_RC) == UCT_MCW_RC)
        mx = (mx & ~(3u << 13)) | (((value >> 8) & 3u) << 13);
    if ((mask & UCT_MCW_DN) == UCT_MCW_DN) {
        mx &= ~(UCT_MX_FZ | UCT_MX_DAZ);
        switch ((value & UCT_MCW_DN) >> 24) {
        case 1: mx |= UCT_MX_FZ | UCT_MX_DAZ; break;
        case 2: mx |= UCT_MX_DAZ; break;
        case 3: mx |= UCT_MX_FZ; break;
        }
    }
    return mx;
}

static inline unsigned uct_read_mxcsr(void) { return __builtin_ia32_stmxcsr(); }
static inline void uct_write_mxcsr(unsigned v) { __builtin_ia32_ldmxcsr(v); }

unsigned w__control87(unsigned value, unsigned mask)
{
    unsigned mx = uct_read_mxcsr();
    mx = uct_control_to_mx(mx, value, mask & (UCT_MCW_EM | UCT_MCW_RC | UCT_MCW_DN));
    uct_write_mxcsr(mx);
    return uct_mx_to_control(mx);
}

unsigned w__controlfp(unsigned value, unsigned mask) { return w__control87(value, mask); }

int w__controlfp_s(unsigned *current, unsigned value, unsigned mask)
{
    unsigned r;
    if ((mask & (UCT_MCW_PC | UCT_MCW_IC)) ||
        (value & mask & ~(UCT_MCW_EM | UCT_MCW_RC | UCT_MCW_DN))) {
        if (current)
            *current = uct_mx_to_control(uct_read_mxcsr());
        return ucrt_invalid(UCRT_EINVAL);
    }
    r = w__control87(value, mask);
    if (current)
        *current = r;
    return 0;
}

unsigned w__statusfp(void) { return uct_mx_to_status(uct_read_mxcsr()); }

unsigned w__clearfp(void)
{
    unsigned mx = uct_read_mxcsr();
    unsigned s = uct_mx_to_status(mx);
    uct_write_mxcsr(mx & ~0x3Fu);
    return s;
}

void w__fpreset(void)
{
    uct_write_mxcsr(0x1F80u);
    __asm__ volatile("fninit");
}

/* C99 <fenv.h> on the same state. */
int w_fegetround(void) { return (int)(uct_mx_to_control(uct_read_mxcsr()) & UCT_MCW_RC); }

int w_fesetround(int mode)
{
    if ((unsigned)mode & ~UCT_MCW_RC)
        return 1;
    uct_write_mxcsr(uct_control_to_mx(uct_read_mxcsr(), (unsigned)mode, UCT_MCW_RC));
    return 0;
}

int w_fetestexcept(int mask) { return (int)(uct_mx_to_status(uct_read_mxcsr()) & (unsigned)mask & 0x1Fu); }

int w_feclearexcept(int mask)
{
    uct_write_mxcsr(uct_read_mxcsr() & ~uct_status_to_mx((unsigned)mask & 0x1Fu));
    return 0;
}

int w_feraiseexcept(int mask)
{
    uct_write_mxcsr(uct_read_mxcsr() | uct_status_to_mx((unsigned)mask & 0x1Fu));
    return 0;
}

typedef struct uct_fenv {
    unsigned long ctl, stat;
} uct_fenv;

int w_fegetenv(uct_fenv *e)
{
    unsigned mx = uct_read_mxcsr();
    e->ctl = uct_mx_to_control(mx);
    e->stat = uct_mx_to_status(mx);
    return 0;
}

int w_fesetenv(const uct_fenv *e)
{
    unsigned mx = uct_read_mxcsr() & ~0x3Fu;
    mx = uct_control_to_mx(mx, (unsigned)e->ctl, UCT_MCW_EM | UCT_MCW_RC | UCT_MCW_DN);
    mx |= uct_status_to_mx((unsigned)e->stat);
    uct_write_mxcsr(mx);
    return 0;
}

int w_feholdexcept(uct_fenv *e)
{
    w_fegetenv(e);
    uct_write_mxcsr((uct_read_mxcsr() & ~0x3Fu) | UCT_MX_IM | UCT_MX_DM | UCT_MX_ZM | UCT_MX_OM | UCT_MX_UM | UCT_MX_PM);
    return 0;
}

int w_feupdateenv(const uct_fenv *e)
{
    unsigned raised = uct_mx_to_status(uct_read_mxcsr());
    w_fesetenv(e);
    uct_write_mxcsr(uct_read_mxcsr() | uct_status_to_mx(raised));
    return 0;
}

/* ---- Classification ---------------------------------------------------------------------------------------------------------------- */

#define UCT_FPCLASS_SNAN 0x0001
#define UCT_FPCLASS_QNAN 0x0002
#define UCT_FPCLASS_NINF 0x0004
#define UCT_FPCLASS_NN 0x0008
#define UCT_FPCLASS_ND 0x0010
#define UCT_FPCLASS_NZ 0x0020
#define UCT_FPCLASS_PZ 0x0040
#define UCT_FPCLASS_PD 0x0080
#define UCT_FPCLASS_PN 0x0100
#define UCT_FPCLASS_PINF 0x0200

static int uct_fpclass_bits(uint64_t bits, int exp_bits, int frac_bits)
{
    uint64_t sign = bits >> (exp_bits + frac_bits);
    uint64_t exp = (bits >> frac_bits) & (((uint64_t)1 << exp_bits) - 1);
    uint64_t frac = bits & (((uint64_t)1 << frac_bits) - 1);
    if (exp == ((uint64_t)1 << exp_bits) - 1) {
        if (!frac)
            return sign ? UCT_FPCLASS_NINF : UCT_FPCLASS_PINF;
        return (frac >> (frac_bits - 1)) ? UCT_FPCLASS_QNAN : UCT_FPCLASS_SNAN;
    }
    if (exp == 0)
        return frac ? (sign ? UCT_FPCLASS_ND : UCT_FPCLASS_PD) : (sign ? UCT_FPCLASS_NZ : UCT_FPCLASS_PZ);
    return sign ? UCT_FPCLASS_NN : UCT_FPCLASS_PN;
}

static uint64_t uct_double_bits(double d)
{
    uint64_t b;
    ucrt_memcpy(&b, &d, 8);
    return b;
}

static uint32_t uct_float_bits(float f)
{
    uint32_t b;
    ucrt_memcpy(&b, &f, 4);
    return b;
}

int w__fpclass(double d) { return uct_fpclass_bits(uct_double_bits(d), 11, 52); }

/* fpclassify's values in the Microsoft runtime: FP_NAN 2, FP_INFINITE 1, FP_NORMAL -1, FP_SUBNORMAL -2, FP_ZERO 0. */
static short uct_dclass_of(int cls)
{
    switch (cls) {
    case UCT_FPCLASS_SNAN:
    case UCT_FPCLASS_QNAN: return 2;
    case UCT_FPCLASS_NINF:
    case UCT_FPCLASS_PINF: return 1;
    case UCT_FPCLASS_ND:
    case UCT_FPCLASS_PD: return -2;
    case UCT_FPCLASS_NZ:
    case UCT_FPCLASS_PZ: return 0;
    default: return -1;
    }
}

short w__dclass(double d) { return uct_dclass_of(uct_fpclass_bits(uct_double_bits(d), 11, 52)); }
short w__fdclass(float f) { return uct_dclass_of(uct_fpclass_bits(uct_float_bits(f), 8, 23)); }
int w__dsign(double d) { return (uct_double_bits(d) >> 63) ? 0x8000 : 0; }
int w__fdsign(float f) { return (uct_float_bits(f) >> 31) ? 0x8000 : 0; }

/* The user handler for math errors is stored and never called: the math functions here do not raise _exception. */
static int (*g_uct_matherr)(void *);
void w___setusermatherr(int (*handler)(void *)) { g_uct_matherr = handler; }

int w__set_FMA3_enable(int enable)
{
    (void)enable;
    return 1;
}

int w__get_FMA3_enable(void) { return 1; }
