module op2_m_stencil1d_1_diffuse_m

    use iso_c_binding

    use op2_fortran_declarations
    use op2_fortran_rt_support

    implicit none

    private
    public :: op2_k_stencil1d_1_diffuse_m

    interface

        subroutine op2_k_stencil1d_1_diffuse_m_c( &
            set, &
            arg0, &
            arg1, &
            arg2 &
        ) bind(C, name='op2_k_stencil1d_1_diffuse_m_c')

            use iso_c_binding
            use op2_fortran_declarations

            type(c_ptr), value :: set

            type(op_arg), value :: arg0
            type(op_arg), value :: arg1
            type(op_arg), value :: arg2

        end subroutine

    end interface

contains

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

    call op2_k_stencil1d_1_diffuse_m_c( &
        set%setcptr, &
        arg0, &
        arg1, &
        arg2 &
    )

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