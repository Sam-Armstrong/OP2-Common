! Robustness: F2018 assumed-type dummy argument type(*).
! fparser2 0.2.0 (std=f2008) rejects type(*).

module assumed_type_helpers
    implicit none
contains
    subroutine ignore_payload(x)
        type(*), intent(in) :: x
        ! host-side placeholder; body intentionally empty
    end subroutine ignore_payload
end module assumed_type_helpers

program assumed_type
    use OP2_FORTRAN_DECLARATIONS
    use OP2_FORTRAN_REFERENCE
    use, intrinsic :: ISO_C_BINDING
    use assumed_type_helpers

    implicit none

    integer(4), parameter :: nnode = 4
    integer(4) :: marker
    real(8), dimension(:), allocatable :: u
    real(8) :: alpha

    type(op_set) :: nodes
    type(op_dat) :: p_u

    marker = 1
    call ignore_payload(marker)

    allocate(u(nnode))
    u = 1.0d0

    call op_init(0)
    call op_decl_set(nnode, nodes, "nodes")
    call op_decl_dat(nodes, 1, "real(8)", u, p_u, "p_u")
    deallocate(u)

    alpha = 2.0d0
    call op_decl_const(alpha, 1, "real(8)")

    call op_par_loop_2(scale, nodes, &
        & op_arg_dat(p_u, -1, OP_ID, 1, "real(8)", OP_RW), &
        & op_arg_gbl(alpha, 1, "real(8)", OP_READ))

    call op_exit()

contains

    subroutine scale(u, alpha)
        implicit none
        real(8), intent(inout) :: u
        real(8), intent(in) :: alpha
        u = u * alpha
    end subroutine scale

end program assumed_type
