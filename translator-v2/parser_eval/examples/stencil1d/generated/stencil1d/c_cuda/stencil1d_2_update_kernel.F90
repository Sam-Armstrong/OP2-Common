module op2_m_stencil1d_2_update_m

    use iso_c_binding

    use op2_fortran_declarations
    use op2_fortran_rt_support

    implicit none

    private
    public :: op2_k_stencil1d_2_update_m

    interface

        subroutine op2_k_stencil1d_2_update_m_c( &
            set, &
            arg0, &
            arg1, &
            arg2, &
            arg3 &
        ) bind(C, name='op2_k_stencil1d_2_update_m_c')

            use iso_c_binding
            use op2_fortran_declarations

            type(c_ptr), value :: set

            type(op_arg), value :: arg0
            type(op_arg), value :: arg1
            type(op_arg), value :: arg2
            type(op_arg), value :: arg3

        end subroutine

    end interface

contains

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

    call op2_k_stencil1d_2_update_m_c( &
        set%setcptr, &
        arg0, &
        arg1, &
        arg2, &
        arg3 &
    )

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