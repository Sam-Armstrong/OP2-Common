namespace op2_m_stencil1d_1_diffuse_m {




static __device__ void diffuse(
    const double u_left,
    const double u_right,
    double& du
);

static __device__ void scale_diff(
    const double left,
    const double right,
    double& out
);


static __device__ void diffuse(
    const double u_left,
    const double u_right,
    double& du
) {
    double delta;

    scale_diff(u_left, u_right, delta);
    atomicAdd(&(du), 0.0e0 + delta);
}

static __device__ void scale_diff(
    const double left,
    const double right,
    double& out
) {

    out = op2_const_alpha_d * (right - left);
}

}


extern "C" __global__ 
void op2_k_stencil1d_1_diffuse_m_wrapper(
    const double *__restrict dat0,
    double *__restrict dat1,
    const int *__restrict map0,
    const int start,
    const int end,
    const int stride
) {
    using namespace op2_m_stencil1d_1_diffuse_m;
    int thread_id = threadIdx.x + blockIdx.x * blockDim.x;

    int zero_int = 0;
    bool zero_bool = 0;
    float zero_float = 0;
    double zero_double = 0;

    for (int i = thread_id + start; i < end; i += blockDim.x * gridDim.x) {
        int n = i;



        diffuse(
            f2c::Ptr{dat0 + map0[0 * stride + n] * 1}.data[0],
            f2c::Ptr{dat0 + map0[1 * stride + n] * 1}.data[0],
            f2c::Ptr{dat1 + map0[0 * stride + n] * 1}.data[0]
        );
    }
}


const char op2_k_stencil1d_1_diffuse_m_src[] = R"_op2_k(
namespace op2_m_stencil1d_1_diffuse_m {

using int64_t = long long int;

static __device__ void diffuse(
    const double u_left,
    const double u_right,
    double& du
);

static __device__ void scale_diff(
    const double left,
    const double right,
    double& out
);


static __device__ void diffuse(
    const double u_left,
    const double u_right,
    double& du
) {
    double delta;

    scale_diff(u_left, u_right, delta);
    atomicAdd(&(du), 0.0e0 + delta);
}

static __device__ void scale_diff(
    const double left,
    const double right,
    double& out
) {

    out = op2_const_alpha_d * (right - left);
}

}

extern "C" __global__ 
void op2_k_stencil1d_1_diffuse_m_wrapper(
    const double *__restrict dat0,
    double *__restrict dat1,
    const int *__restrict map0,
    const int start,
    const int end,
    const int stride
) {
    using namespace op2_m_stencil1d_1_diffuse_m;
    int thread_id = threadIdx.x + blockIdx.x * blockDim.x;

    int zero_int = 0;
    bool zero_bool = 0;
    float zero_float = 0;
    double zero_double = 0;

    for (int i = thread_id + start; i < end; i += blockDim.x * gridDim.x) {
        int n = i;



        diffuse(
            f2c::Ptr{dat0 + map0[0 * stride + n] * 1}.data[0],
            f2c::Ptr{dat0 + map0[1 * stride + n] * 1}.data[0],
            f2c::Ptr{dat1 + map0[0 * stride + n] * 1}.data[0]
        );
    }
}

)_op2_k";


extern "C" void op2_k_stencil1d_1_diffuse_m_c(
    op_set set,
    op_arg arg0,
    op_arg arg1,
    op_arg arg2
) {
    namespace kernel = op2_m_stencil1d_1_diffuse_m;

    int n_args = 3;
    op_arg args[3];

    op_profile_enter_kernel("stencil1d_1_diffuse", "c_CUDA", "Indirect (atomics)");
    op_profile_enter("Init");

    op_profile_enter("Kernel Info Setup");

    static bool first_invocation = true;
    static op::f2c::KernelInfo info("op2_k_stencil1d_1_diffuse_m_wrapper",
                                    (void *)op2_k_stencil1d_1_diffuse_m_wrapper,
                                    op2_k_stencil1d_1_diffuse_m_src);

    if (first_invocation) {
        info.add_param("op2_const_alpha_d", &op2_const_alpha, &op2_const_alpha_d, &op2_const_alpha_hash);

        first_invocation = false;
    }

    args[0] = arg0;
    args[1] = arg1;
    args[2] = arg2;

    op_profile_next("MPI Exchanges");
    int n_exec = op_mpi_halo_exchanges_grouped(set, n_args, args, 2);

    if (n_exec == 0) {
        op_profile_exit();
        op_profile_exit();

        op_mpi_wait_all_grouped(n_args, args, 2);


        op_mpi_set_dirtybit_cuda(n_args, args);
        op_profile_exit();
        return;
    }

    setGblIncAtomic(false);




    op_profile_next("Get Kernel");
    auto *kernel_inst = info.get_kernel();
    op_profile_exit();

    std::array<int, 3> sections = {0, set->core_size, set->size + set->exec_size};

    auto [block_limit, block_size] = info.get_launch_config(kernel_inst, set->core_size);
    block_limit = std::min(block_limit, getBlockLimit(args, n_args, block_size, "stencil1d_1_diffuse"));

    int max_blocks = 0;
    for (int i = 1; i < sections.size(); ++i)
        max_blocks = std::max(max_blocks, (sections[i] - sections[i - 1] + (block_size - 1)) / block_size);

    max_blocks = std::min(max_blocks, block_limit);


    op_profile_enter("Prepare GBLs");
    prepareDeviceGbls(args, n_args, block_size * max_blocks);
    bool exit_sync = false;

    arg0 = args[0];
    arg1 = args[1];
    arg2 = args[2];

    op_profile_next("Update GBL Refs");


    op_profile_exit();
    op_profile_next("Computation");

    op_profile_enter("Kernel");

    for (int round = 1; round < sections.size(); ++round) {
        if (round == 2) {
            op_profile_next("MPI Wait");
            op_mpi_wait_all_grouped(n_args, args, 2);
            op_profile_next("Kernel");
        }

        int start = sections[round - 1];
        int end = sections[round];

        if (end - start > 0) {
            int num_blocks = (end - start + (block_size - 1)) / block_size;
            num_blocks = std::min(num_blocks, block_limit);

            int size = f2c::round32(set->size + set->exec_size);
            void *kernel_args[] = {
                &arg0.data_d,
                &arg2.data_d,
                &arg0.map_data_d,
                &start,
                &end,
                &size
            };

            void *kernel_args_jit[] = {
                &arg0.data_d,
                &arg2.data_d,
                &arg0.map_data_d,
                &start,
                &end,
                &size
            };

            info.invoke(kernel_inst, num_blocks, block_size, kernel_args, kernel_args_jit);
        }

    }

    op_profile_exit();

    op_profile_exit();

    op_profile_enter("Finalise");

    op_mpi_set_dirtybit_cuda(n_args, args);
    if (exit_sync) CUDA_SAFE_CALL(cudaStreamSynchronize(0));

    op_profile_exit();
    op_profile_exit();
}