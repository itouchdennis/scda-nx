// The CRT's x87 acos and asin check the argument in one call and branch on the ZF it returns in the next.
// Recompiled code does not carry flags across call and ret, so they read stale flags and returned NaN at
// random, which pinned look-at heads to their limit and twisted skinned bones. Computed on the host instead.
#include <cmath>

#include "xdk/hle.h"

X86_FUNC(GUEST(_CIacos)) {
    (void)base;
    X87_ST(0) = std::acos(X87_ST(0));
    ctx.esp += 4;
}

X86_FUNC(GUEST(_CIasin)) {
    (void)base;
    X87_ST(0) = std::asin(X87_ST(0));
    ctx.esp += 4;
}

// pow's body, reached from pow and _CIpow: x and y above the return address, y in st0. Its inf or NaN test on x is
// the same kind of branch and was never taken.
X86_FUNC(GUEST(crt_pow)) {
    X87_ST(0) = std::pow(double(X86_MEMF64(ctx.esp + 4)), double(X86_MEMF64(ctx.esp + 12)));
    ctx.esp += 4;
}
