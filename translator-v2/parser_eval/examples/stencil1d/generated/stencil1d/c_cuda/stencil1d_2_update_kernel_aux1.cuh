namespace op2_m_stencil1d_2_update_m {




static __device__ void update(
    double& u,
    double& du,
    double& residual,
    double& u_max
);


static __device__ void update(
    double& u,
    double& du,
    double& residual,
    double& u_max
) {

    u = u + du;
    residual = residual + f2c::abs(du);
    du = 0.0;
    u_max = f2c::max(u_max, u);
}

}


extern "C" __global__ 
void op2_k_stencil1d_2_update_m_wrapper(
    double *__restrict dat0,
    double *__restrict dat1,
    double *__restrict gbl2,
    double *__restrict gbl3,
    const int stride_gbl,
    const int start,
    const int end,
    const int stride
) {
    using namespace op2_m_stencil1d_2_update_m;
    int thread_id = threadIdx.x + blockIdx.x * blockDim.x;

    int zero_int = 0;
    bool zero_bool = 0;
    float zero_float = 0;
    double zero_double = 0;

    for (int i = thread_id + start; i < end; i += blockDim.x * gridDim.x) {
        int n = i;



        update(
            f2c::Ptr{dat0 + n * 1}.data[0],
            f2c::Ptr{dat1 + n * 1}.data[0],
            f2c::Ptr{gbl2 + thread_id, stride_gbl}.data[0],
            f2c::Ptr{gbl3 + thread_id, stride_gbl}.data[0]
        );
    }
}


const char op2_k_stencil1d_2_update_m_src[] = R"_op2_k(
namespace op2_m_stencil1d_2_update_m {

using int64_t = long long int;

static __device__ void update(
    double& u,
    double& du,
    double& residual,
    double& u_max
);


static __device__ void update(
    double& u,
    double& du,
    double& residual,
    double& u_max
) {

    u = u + du;
    residual = residual + f2c::abs(du);
    du = 0.0;
    u_max = f2c::max(u_max, u);
}

}

extern "C" __global__ 
void op2_k_stencil1d_2_update_m_wrapper(
    double *__restrict dat0,
    double *__restrict dat1,
    double *__restrict gbl2,
    double *__restrict gbl3,
    const int stride_gbl,
    const int start,
    const int end,
    const int stride
) {
    using namespace op2_m_stencil1d_2_update_m;
    int thread_id = threadIdx.x + blockIdx.x * blockDim.x;

    int zero_int = 0;
    bool zero_bool = 0;
    float zero_float = 0;
    double zero_double = 0;

    for (int i = thread_id + start; i < end; i += blockDim.x * gridDim.x) {
        int n = i;



        update(
            f2c::Ptr{dat0 + n * 1}.data[0],
            f2c::Ptr{dat1 + n * 1}.data[0],
            f2c::Ptr{gbl2 + thread_id, stride_gbl}.data[0],
            f2c::Ptr{gbl3 + thread_id, stride_gbl}.data[0]
        );
    }
}

)_op2_k";

__global__
static void op2_k_stencil1d_2_update_m_init_gbls(
    double *gbl2,
    double *gbl3,
    double *gbl3_ref,
    int stride
) {
    namespace kernel = op2_m_stencil1d_2_update_m;

    int thread_id = threadIdx.x + blockIdx.x * blockDim.x;

    for (int d = 0; d < 1; ++d) {
        gbl2[thread_id + d * stride] = 0;
    }
    for (int d = 0; d < 1; ++d) {
        gbl3[thread_id + d * stride] = gbl3_ref[d];
    }
}

extern "C" void op2_k_stencil1d_2_update_m_c(
    op_set set,
    op_arg arg0,
    op_arg arg1,
    op_arg arg2,
    op_arg arg3
) {
    namespace kernel = op2_m_stencil1d_2_update_m;

    int n_args = 4;
    op_arg args[4];

    op_profile_enter_kernel("stencil1d_2_update", "c_CUDA", "Direct");
    op_profile_enter("Init");

    op_profile_enter("Kernel Info Setup");

    static bool first_invocation = true;
    static op::f2c::KernelInfo info("op2_k_stencil1d_2_update_m_wrapper",
                                    (void *)op2_k_stencil1d_2_update_m_wrapper,
                                    op2_k_stencil1d_2_update_m_src);

    if (first_invocation) {

        first_invocation = false;
    }

    args[0] = arg0;
    args[1] = arg1;
    args[2] = arg2;
    args[3] = arg3;

    op_profile_next("MPI Exchanges");
    int n_exec = op_mpi_halo_exchanges_grouped(set, n_args, args, 2);

    if (n_exec == 0) {
        op_profile_exit();
        op_profile_exit();

        op_mpi_wait_all_grouped(n_args, args, 2);

        op_mpi_reduce(&arg2, (double *)arg2.data);
        op_mpi_reduce(&arg3, (double *)arg3.data);

        op_mpi_set_dirtybit_cuda(n_args, args);
        op_profile_exit();
        return;
    }

    setGblIncAtomic(false);



    static double* gbl3_ref_d = nullptr;

    op_profile_next("Get Kernel");
    auto *kernel_inst = info.get_kernel();
    op_profile_exit();

    auto [block_limit, block_size] = info.get_launch_config(kernel_inst, set->size);
    block_limit = std::min(block_limit, getBlockLimit(args, n_args, block_size, "stencil1d_2_update"));

    int num_blocks = (set->size + (block_size - 1)) / block_size;
    num_blocks = std::min(num_blocks, block_limit);
    int max_blocks = num_blocks;


    op_profile_enter("Prepare GBLs");
    prepareDeviceGbls(args, n_args, block_size * max_blocks);
    bool exit_sync = false;

    arg0 = args[0];
    arg1 = args[1];
    arg2 = args[2];
    arg3 = args[3];

    op_profile_next("Update GBL Refs");
    if (gbl3_ref_d == nullptr) {
        CUDA_SAFE_CALL(cudaMalloc(&gbl3_ref_d, 1 * sizeof(double)));
    }

    CUDA_SAFE_CALL(cudaMemcpyAsync(gbl3_ref_d, arg3.data, 1 * sizeof(double), cudaMemcpyHostToDevice, 0));

    op_profile_next("Init GBLs");

    int stride_gbl = block_size * max_blocks;
    op2_k_stencil1d_2_update_m_init_gbls<<<max_blocks, block_size>>>(
        (double *)arg2.data_d,
        (double *)arg3.data_d,
        gbl3_ref_d,
        stride_gbl
    );

    CUDA_SAFE_CALL(cudaPeekAtLastError());

    op_profile_exit();
    op_profile_next("Computation");

    int start = 0;
    int end = set->size;

    op_profile_enter("Kernel");

    int size = f2c::round32(set->size);
    void *kernel_args[] = {
        &arg0.data_d,
        &arg1.data_d,
        &arg2.data_d,
        &arg3.data_d,
        &stride_gbl,
        &start,
        &end,
        &size
    };

    void *kernel_args_jit[] = {
        &arg0.data_d,
        &arg1.data_d,
        &arg2.data_d,
        &arg3.data_d,
        &stride_gbl,
        &start,
        &end,
        &size
    };

    info.invoke(kernel_inst, num_blocks, block_size, kernel_args, kernel_args_jit);

    op_profile_next("Process GBLs");
    exit_sync = processDeviceGbls(args, n_args, block_size * max_blocks, block_size * max_blocks);

    op_profile_exit();

    op_profile_exit();

    op_profile_enter("Finalise");
    op_mpi_reduce(&arg2, (double *)arg2.data);
    op_mpi_reduce(&arg3, (double *)arg3.data);

    op_mpi_set_dirtybit_cuda(n_args, args);
    if (exit_sync) CUDA_SAFE_CALL(cudaStreamSynchronize(0));

    op_profile_exit();
    op_profile_exit();
}