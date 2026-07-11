
#define SIMD_LEN 8
#define op2_s(comp, simd_len) ((comp-1)*simd_len + 1)

module op2_m_stencil1d_2_update_m

    use iso_c_binding
    use omp_lib

    use op2_fortran_declarations
    use op2_fortran_rt_support

    use op2_consts

    implicit none

    private
    public :: op2_k_stencil1d_2_update_m

contains

SUBROUTINE update_simd(u, du, residual, u_max)
  IMPLICIT NONE
  REAL(KIND = 8), INTENT(INOUT) :: u, du, residual, u_max
  u = u + du
  residual = residual + ABS(du)
  du = 0.0_8
  u_max = MAX(u_max, u)
END SUBROUTINE

SUBROUTINE update(u, du, residual, u_max)
  IMPLICIT NONE
  REAL(KIND = 8), INTENT(INOUT) :: u, du, residual, u_max
  u = u + du
  residual = residual + ABS(du)
  du = 0.0_8
  u_max = MAX(u_max, u)
END SUBROUTINE

subroutine update_wrapper2( &
    dat0, &
    dat1, &
    gbl2, &
    gbl3, &
    start, &
    end &
)
    implicit none

    ! parameters
    real(8), dimension(1, *) :: dat0
    real(8), dimension(1, *) :: dat1

    real(8), dimension(1) :: gbl2
    real(8), dimension(1) :: gbl3

    integer(4) :: start, end

    ! locals
    integer(4) :: n
    integer(4) :: block, lane, d

    real(8), dimension(SIMD_LEN, 1) :: arg2_local
    real(8), dimension(SIMD_LEN, 1) :: arg3_local

    block = start
    do while (block + SIMD_LEN <= end)
        arg2_local = 0

        do lane = 1, SIMD_LEN
            n = block + lane - 1

            arg3_local(lane, :) = gbl3
        end do

        !$omp simd
        do lane = 1, SIMD_LEN
            n = block + lane - 1

            call update_simd( &
                dat0(1, n + 1), &
                dat1(1, n + 1), &
                arg2_local(lane, 1), &
                arg3_local(lane, 1) &
            )
        end do

        ! Reduction back to globals
        do lane = 1, SIMD_LEN
            n = block + lane - 1

            gbl2 = gbl2 + arg2_local(lane, :)
            gbl3 = MAX(gbl3, arg3_local(lane, :))
        end do

        block = block + SIMD_LEN
    end do

    do n = block, end
        call update( &
            dat0(1, n + 1), &
            dat1(1, n + 1), &
            gbl2(1), &
            gbl3(1) &
        )
    end do
end subroutine

subroutine update_wrapper( &
    name, &
    dat0, &
    dat1, &
    gbl2, &
    gbl3, &
    set, &
    args, &
    num_dats_indirect, &
    dats_indirect &
)
    implicit none

    ! parameters
    character(kind=c_char, len=*) :: name

    real(8), dimension(1, *) :: dat0
    real(8), dimension(1, *) :: dat1

    real(8), dimension(1) :: gbl2
    real(8), dimension(1) :: gbl3

    type(op_set) :: set
    type(op_arg), dimension(4) :: args

    integer(4) :: num_dats_indirect
    integer(4), dimension(4) :: dats_indirect

    ! locals
    integer(4) :: thread, start, end, n
    integer(4) :: num_threads


    real(8), dimension(:), allocatable :: gbl2_temp
    real(8), dimension(:), allocatable :: gbl3_temp

    num_threads = omp_get_max_threads()

    allocate(gbl2_temp(num_threads * 64))
    gbl2_temp = 0

    allocate(gbl3_temp(num_threads * 64))

    do thread = 1, num_threads
        start = (thread - 1) * 64 + 1
        gbl3_temp(start : start + 0) = gbl3
    end do

    !$omp parallel do private(thread, start, end, n)
    do thread = 1, num_threads
        start = (set%setptr%size * (thread - 1)) / num_threads
        end = (set%setptr%size * thread) / num_threads - 1

        call update_wrapper2( &
            dat0, &
            dat1, &
            gbl2_temp(omp_get_thread_num() * 64 + 1), &
            gbl3_temp(omp_get_thread_num() * 64 + 1), &
            start, &
            end &
        )
    end do

    do thread = 1, num_threads
        start = (thread - 1) * 64 + 1
        gbl2 = gbl2 + gbl2_temp(start : start + 0)
    end do

    do thread = 1, num_threads
        start = (thread - 1) * 64 + 1
        gbl3 = MAX(gbl3, gbl3_temp(start : start + 0))
    end do
end subroutine

subroutine op2_k_stencil1d_2_update_m( &
    name, &
    set, &
    arg0, &
    arg1, &
    arg2, &
    arg3 &
)
    implicit none

    ! parameters
    character(kind=c_char, len=*) :: name
    type(op_set) :: set

    type(op_arg) :: arg0
    type(op_arg) :: arg1
    type(op_arg) :: arg2
    type(op_arg) :: arg3

    ! locals
    type(op_arg), dimension(4) :: args

    integer(4) :: num_dats_indirect
    integer(4), dimension(4) :: dats_indirect

    integer(4) :: set_size

    real(8), pointer, dimension(:, :) :: dat0
    real(8), pointer, dimension(:, :) :: dat1

    real(8), pointer, dimension(:) :: gbl2
    real(8), pointer, dimension(:) :: gbl3


    args(1) = arg0
    args(2) = arg1
    args(3) = arg2
    args(4) = arg3

    num_dats_indirect = 0
    dats_indirect = (/-1, -1, -1, -1/)

    call op_profile_enter_kernel("stencil1d_2_update", "openmp", "Direct")

    call op_profile_enter("MPI Exchanges")
    set_size = op_mpi_halo_exchanges(set%setcptr, size(args), args)

    call op_profile_next("Computation")

    call c_f_pointer(arg0%data, dat0, (/1, getsetsizefromoparg(arg0)/))
    call c_f_pointer(arg1%data, dat1, (/1, getsetsizefromoparg(arg1)/))

    call c_f_pointer(arg2%data, gbl2, (/1/))
    call c_f_pointer(arg3%data, gbl3, (/1/))

    call update_wrapper( &
        name, &
        dat0, &
        dat1, &
        gbl2, &
        gbl3, &
        set, &
        args, &
        num_dats_indirect, &
        dats_indirect &
    )

    call op_profile_next("MPI Wait")
    if ((set_size .eq. 0) .or. (set_size .eq. set%setptr%core_size)) then
        call op_mpi_wait_all(size(args), args)
    end if

    call op_mpi_reduce_double(arg2, arg2%data)
    call op_mpi_reduce_double(arg3, arg3%data)

    call op_profile_exit()

    call op_mpi_set_dirtybit(size(args), args)
    call op_profile_exit()
end subroutine

end module

module op2_m_stencil1d_2_update_fb

    use iso_c_binding

    use op2_fortran_declarations
    use op2_fortran_rt_support

    use op2_consts

    implicit none

    private
    public :: op2_k_stencil1d_2_update_fb

contains

SUBROUTINE update(u, du, residual, u_max)
  IMPLICIT NONE
  REAL(KIND = 8), INTENT(INOUT) :: u, du, residual, u_max
  u = u + du
  residual = residual + ABS(du)
  du = 0.0_8
  u_max = MAX(u_max, u)
END SUBROUTINE

subroutine op2_k_stencil1d_2_update_wr( &
    dat0, &
    dat1, &
    gbl2, &
    gbl3, &
    n_exec, &
    set, &
    args &
)
    implicit none

    ! parameters
    real(8), dimension(:, :) :: dat0
    real(8), dimension(:, :) :: dat1

    real(8), dimension(:) :: gbl2
    real(8), dimension(:) :: gbl3

    integer(4) :: n_exec
    type(op_set) :: set
    type(op_arg), dimension(4) :: args

    ! locals
    integer(4) :: n

    do n = 1, n_exec
        call update( &
            dat0(1, n), &
            dat1(1, n), &
            gbl2(1), &
            gbl3(1) &
        )
    end do
end subroutine

subroutine op2_k_stencil1d_2_update_fb( &
    name, &
    set, &
    arg0, &
    arg1, &
    arg2, &
    arg3 &
)
    implicit none

    ! parameters
    character(kind=c_char, len=*) :: name
    type(op_set) :: set

    type(op_arg) :: arg0
    type(op_arg) :: arg1
    type(op_arg) :: arg2
    type(op_arg) :: arg3

    ! locals
    type(op_arg), dimension(4) :: args

    integer(4) :: n_exec

    real(8), pointer, dimension(:, :) :: dat0
    real(8), pointer, dimension(:, :) :: dat1

    real(8), pointer, dimension(:) :: gbl2
    real(8), pointer, dimension(:) :: gbl3

    real(4) :: transfer

    args(1) = arg0
    args(2) = arg1
    args(3) = arg2
    args(4) = arg3

    call op_profile_enter_kernel("stencil1d_2_update", "seq", "Direct")

    call op_profile_enter("MPI Exchanges")
    n_exec = op_mpi_halo_exchanges(set%setcptr, size(args), args)

    call op_profile_next("Computation")

    call c_f_pointer(arg0%data, dat0, (/1, getsetsizefromoparg(arg0)/))
    call c_f_pointer(arg1%data, dat1, (/1, getsetsizefromoparg(arg1)/))

    call c_f_pointer(arg2%data, gbl2, (/1/))
    call c_f_pointer(arg3%data, gbl3, (/1/))

    call op2_k_stencil1d_2_update_wr( &
        dat0, &
        dat1, &
        gbl2, &
        gbl3, &
        n_exec, &
        set, &
        args &
    )

    call op_profile_next("MPI Wait")
    if ((n_exec == 0) .or. (n_exec == set%setptr%core_size)) then
        call op_mpi_wait_all(size(args), args)
    end if

    call op_profile_next("MPI Reduce")

    call op_mpi_reduce_double(arg2, arg2%data)
    call op_mpi_reduce_double(arg3, arg3%data)

    call op_profile_exit()

    call op_mpi_set_dirtybit(size(args), args)
    call op_profile_exit()
end subroutine

end module

module op2_m_stencil1d_2_update

    use iso_c_binding

    use op2_fortran_declarations
    use op2_fortran_rt_support

    use op2_m_stencil1d_2_update_fb
    use op2_m_stencil1d_2_update_m

    implicit none

    private
    public :: op2_k_stencil1d_2_update

contains

subroutine op2_k_stencil1d_2_update( &
    name, &
    set, &
    arg0, &
    arg1, &
    arg2, &
    arg3 &
)
    character(kind=c_char, len=*) :: name
    type(op_set) :: set

    type(op_arg) :: arg0
    type(op_arg) :: arg1
    type(op_arg) :: arg2
    type(op_arg) :: arg3

    if (op_check_whitelist("stencil1d_2_update")) then
        call op2_k_stencil1d_2_update_m( &
            name, &
            set, &
            arg0, &
            arg1, &
            arg2, &
            arg3 &
        )
    else
        call op_check_fallback_mode("stencil1d_2_update")
        call op2_k_stencil1d_2_update_fb( &
            name, &
            set, &
            arg0, &
            arg1, &
            arg2, &
            arg3 &
        )
    end if

end subroutine

end module