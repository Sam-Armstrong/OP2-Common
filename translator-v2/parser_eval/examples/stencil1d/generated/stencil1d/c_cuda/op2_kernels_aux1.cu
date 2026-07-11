
#define INCBIN_STYLE INCBIN_STYLE_SNAKE
#define INCBIN_PREFIX
#include <extern/incbin.h>

#include <cstdint>

extern double op2_const_alpha;

static __constant__ double op2_const_alpha_d;

static uint64_t  op2_const_alpha_hash = 0;

#define OP_F2C_PRELUDE OP_F2C_PRELUDE_5286389598
#define OP_F2C_PRELUDE_DATA OP_F2C_PRELUDE_5286389598_data
INCTXT(OP_F2C_PRELUDE, "op_f2c_prelude.h");

#include <op_f2c_prelude.h>
#include <op_f2c_helpers.h>

#include <op_lib_cpp.h>
#include <op_profile.h>

#include <cstdint>
#include <cmath>
#include <cstdio>

namespace f2c = op::f2c;

extern "C" {

void prepareDeviceGbls(op_arg *args, int nargs, int max_threads);
bool processDeviceGbls(op_arg *args, int nargs, int nthreads, int max_threads);
int getBlockLimit(op_arg *args, int nargs, int block_size, const char *name);
void setGblIncAtomic(bool enable);

}

#include "stencil1d_1_diffuse_kernel_aux1.cuh"
#include "stencil1d_2_update_kernel_aux1.cuh"
