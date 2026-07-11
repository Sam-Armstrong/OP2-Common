
#define SIMD_LEN 8
#define op2_s(comp, simd_len) ((comp-1)*simd_len + 1)

module op2_m_stencil1d_1_diffuse_m

    use iso_c_binding
    use omp_lib

    use op2_fortran_declarations
    use op2_fortran_rt_support

    use op2_consts

    implicit none

    private
    public :: op2_k_stencil1d_1_diffuse_m

contains

SUBROUTINE scale_diff(left, right, out)
  IMPLICIT NONE
  REAL(KIND = 8), INTENT(IN) :: left, right
  REAL(KIND = 8), INTENT(OUT) :: out
  out = op2_const_alpha * (right - left)
END SUBROUTINE

SUBROUTINE diffuse_simd(u_left, u_right, du)
  IMPLICIT NONE
  REAL(KIND = 8), INTENT(IN) :: u_left, u_right
  REAL(KIND = 8), INTENT(INOUT) :: du
  REAL(KIND = 8) :: delta
  CALL scale_diff(u_left, u_right, delta)
  du = du + delta
END SUBROUTINE

SUBROUTINE diffuse(u_left, u_right, du)
  IMPLICIT NONE
  REAL(KIND = 8), INTENT(IN) :: u_left, u_right
  REAL(KIND = 8), INTENT(INOUT) :: du
  REAL(KIND = 8) :: delta
  CALL scale_diff(u_left, u_right, delta)
  du = du + delta
END SUBROUTINE

subroutine diffuse_wrapper2( &
    dat0, &
    dat1, &
    map0, &
    start, &
    end &
)
    implicit none

    ! parameters
    real(8), dimension(1, *) :: dat0
    real(8), dimension(1, *) :: dat1

    integer(4), dimension(:, :) :: map0

    integer(4) :: start, end

    ! locals
    integer(4) :: n
    integer(4) :: block, lane, d

    real(8), dimension(SIMD_LEN, 1) :: arg0_local
    real(8), dimension(SIMD_LEN, 1) :: arg1_local
    real(8), dimension(SIMD_LEN, 1) :: arg2_local

    block = start
    do while (block + SIMD_LEN <= end)
        arg2_local = 0

        do lane = 1, SIMD_LEN
            n = block + lane - 1

            arg0_local(lane, :) = dat0(:, map0(1, n + 1) + 1)
            arg1_local(lane, :) = dat0(:, map0(2, n + 1) + 1)
        end do

        !$omp simd
        do lane = 1, SIMD_LEN
            n = block + lane - 1

            call diffuse_simd( &
                arg0_local(lane, 1), &
                arg1_local(lane, 1), &
                arg2_local(lane, 1) &
            )
        end do

        ! Reduction back to globals
        do lane = 1, SIMD_LEN
            n = block + lane - 1

            dat1(:, map0(1, n + 1) + 1) = dat1(:, map0(1, n + 1) + 1) + arg2_local(lane, :)
        end do

        block = block + SIMD_LEN
    end do

    do n = block, end
        call diffuse( &
            dat0(1, map0(1, n + 1) + 1), &
            dat0(1, map0(2, n + 1) + 1), &
            dat1(1, map0(1, n + 1) + 1) &
        )
    end do
end subroutine

subroutine diffuse_wrapper( &
    name, &
    dat0, &
    dat1, &
    map0, &
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

    integer(4), dimension(:, :) :: map0

    type(op_set) :: set
    type(op_arg), dimension(3) :: args

    integer(4) :: num_dats_indirect
    integer(4), dimension(3) :: dats_indirect

    ! locals
    integer(4) :: thread, start, end, n
    integer(4) :: num_threads

    integer(4) :: part_size, col, block_idx, block_offset, num_blocks, block_id, num_elem, offset

    type(op_plan), pointer :: plan
    integer(4), dimension(:), pointer :: plan_ncolblk, plan_blkmap, plan_nelems, plan_offset

#ifdef OP_PART_SIZE_1
    part_size = OP_PART_SIZE_1
#else
    part_size = 0
#endif

    plan => fortranplancaller( &
        name // c_null_char, &
        set%setcptr, &
        part_size, &
        size(args), &
        args, &
        num_dats_indirect, &
        dats_indirect, &
        2 &
    )

    call c_f_pointer(plan%ncolblk, plan_ncolblk, (/ plan%ncolors /))
    call c_f_pointer(plan%blkmap, plan_blkmap, (/ plan%nblocks /))
    call c_f_pointer(plan%nelems, plan_nelems, (/ plan%nblocks /))
    call c_f_pointer(plan%offset, plan_offset, (/ plan%nblocks /))

    block_offset = 0
    do col = 1, plan%ncolors
        if (col == plan%ncolors_core + 1) then
            call op_mpi_wait_all(size(args), args)
        end if

        num_blocks = plan_ncolblk(col)

        !$omp parallel do private(thread, block_idx, block_id, num_elem, offset, n)
        do block_idx = 1, num_blocks
            thread = omp_get_thread_num() + 1

            block_id = plan_blkmap(block_idx + block_offset) + 1
            num_elem = plan_nelems(block_id)
            offset = plan_offset(block_id)

            call diffuse_wrapper2( &
                dat0, &
                dat1, &
                map0, &
                offset, &
                offset + num_elem - 1 &
            )
        end do

        block_offset = block_offset + num_blocks
    end do
end subroutine

subroutine op2_k_stencil1d_1_diffuse_m( &
    name, &
    set, &
    arg0, &
    arg1, &
    arg2 &
)
    implicit none

    ! parameters
    character(kind=c_char, len=*) :: name
    type(op_set) :: set

    type(op_arg) :: arg0
    type(op_arg) :: arg1
    type(op_arg) :: arg2

    ! locals
    type(op_arg), dimension(3) :: args

    integer(4) :: num_dats_indirect
    integer(4), dimension(3) :: dats_indirect

    integer(4) :: set_size

    real(8), pointer, dimension(:, :) :: dat0
    real(8), pointer, dimension(:, :) :: dat1

    integer(4), pointer, dimension(:, :) :: map0


    args(1) = arg0
    args(2) = arg1
    args(3) = arg2

    num_dats_indirect = 2
    dats_indirect = (/0, 0, 1/)

    call op_profile_enter_kernel("stencil1d_1_diffuse", "openmp", "Indirect")

    call op_profile_enter("MPI Exchanges")
    set_size = op_mpi_halo_exchanges(set%setcptr, size(args), args)

    call op_profile_next("Computation")

    call c_f_pointer(arg0%data, dat0, (/1, getsetsizefromoparg(arg0)/))
    call c_f_pointer(arg2%data, dat1, (/1, getsetsizefromoparg(arg2)/))

    call c_f_pointer(arg0%map_data, map0, (/getmapdimfromoparg(arg0), set%setptr%size/))

    call diffuse_wrapper( &
        name, &
        dat0, &
        dat1, &
        map0, &
        set, &
        args, &
        num_dats_indirect, &
        dats_indirect &
    )

    call op_profile_next("MPI Wait")
    if ((set_size .eq. 0) .or. (set_size .eq. set%setptr%core_size)) then
        call op_mpi_wait_all(size(args), args)
    end if

    call op_profile_exit()

    call op_mpi_set_dirtybit(size(args), args)
    call op_profile_exit()
end subroutine

end module

module op2_m_stencil1d_1_diffuse_fb

    use iso_c_binding

    use op2_fortran_declarations
    use op2_fortran_rt_support

    use op2_consts

    implicit none

    private
    public :: op2_k_stencil1d_1_diffuse_fb

contains

SUBROUTINE scale_diff(left, right, out)
  IMPLICIT NONE
  REAL(KIND = 8), INTENT(IN) :: left, right
  REAL(KIND = 8), INTENT(OUT) :: out
  out = op2_const_alpha * (right - left)
END SUBROUTINE

SUBROUTINE diffuse(u_left, u_right, du)
  IMPLICIT NONE
  REAL(KIND = 8), INTENT(IN) :: u_left, u_right
  REAL(KIND = 8), INTENT(INOUT) :: du
  REAL(KIND = 8) :: delta
  CALL scale_diff(u_left, u_right, delta)
  du = du + delta
END SUBROUTINE

subroutine op2_k_stencil1d_1_diffuse_wr( &
    dat0, &
    dat1, &
    map0, &
    n_exec, &
    set, &
    args &
)
    implicit none

    ! parameters
    real(8), dimension(:, :) :: dat0
    real(8), dimension(:, :) :: dat1

    integer(4), dimension(:, :) :: map0

    integer(4) :: n_exec
    type(op_set) :: set
    type(op_arg), dimension(3) :: args

    ! locals
    integer(4) :: n

    do n = 1, n_exec
        if (n == set%setptr%core_size + 1) then
            call op_profile_next("MPI Wait")
            call op_mpi_wait_all(size(args), args)
            call op_profile_next("Computation")
        end if

        call diffuse( &
            dat0(1, map0(1, n) + 1), &
            dat0(1, map0(2, n) + 1), &
            dat1(1, map0(1, n) + 1) &
        )
    end do
end subroutine

subroutine op2_k_stencil1d_1_diffuse_fb( &
    name, &
    set, &
    arg0, &
    arg1, &
    arg2 &
)
    implicit none

    ! parameters
    character(kind=c_char, len=*) :: name
    type(op_set) :: set

    type(op_arg) :: arg0
    type(op_arg) :: arg1
    type(op_arg) :: arg2

    ! locals
    type(op_arg), dimension(3) :: args

    integer(4) :: n_exec

    real(8), pointer, dimension(:, :) :: dat0
    real(8), pointer, dimension(:, :) :: dat1

    integer(4), pointer, dimension(:, :) :: map0

    real(4) :: transfer

    args(1) = arg0
    args(2) = arg1
    args(3) = arg2

    call op_profile_enter_kernel("stencil1d_1_diffuse", "seq", "Indirect")

    call op_profile_enter("MPI Exchanges")
    n_exec = op_mpi_halo_exchanges(set%setcptr, size(args), args)

    call op_profile_next("Computation")

    call c_f_pointer(arg0%data, dat0, (/1, getsetsizefromoparg(arg0)/))
    call c_f_pointer(arg2%data, dat1, (/1, getsetsizefromoparg(arg2)/))

    call c_f_pointer(arg0%map_data, map0, (/getmapdimfromoparg(arg0), set%setptr%size/))

    call op2_k_stencil1d_1_diffuse_wr( &
        dat0, &
        dat1, &
        map0, &
        n_exec, &
        set, &
        args &
    )

    call op_profile_next("MPI Wait")
    if ((n_exec == 0) .or. (n_exec == set%setptr%core_size)) then
        call op_mpi_wait_all(size(args), args)
    end if

    call op_profile_exit()

    call op_mpi_set_dirtybit(size(args), args)
    call op_profile_exit()
end subroutine

end module

module op2_m_stencil1d_1_diffuse

    use iso_c_binding

    use op2_fortran_declarations
    use op2_fortran_rt_support

    use op2_m_stencil1d_1_diffuse_fb
    use op2_m_stencil1d_1_diffuse_m

    implicit none

    private
    public :: op2_k_stencil1d_1_diffuse

contains

subroutine op2_k_stencil1d_1_diffuse( &
    name, &
    set, &
    arg0, &
    arg1, &
    arg2 &
)
    character(kind=c_char, len=*) :: name
    type(op_set) :: set

    type(op_arg) :: arg0
    type(op_arg) :: arg1
    type(op_arg) :: arg2

    if (op_check_whitelist("stencil1d_1_diffuse")) then
        call op2_k_stencil1d_1_diffuse_m( &
            name, &
            set, &
            arg0, &
            arg1, &
            arg2 &
        )
    else
        call op_check_fallback_mode("stencil1d_1_diffuse")
        call op2_k_stencil1d_1_diffuse_fb( &
            name, &
            set, &
            arg0, &
            arg1, &
            arg2 &
        )
    end if

end subroutine

end module