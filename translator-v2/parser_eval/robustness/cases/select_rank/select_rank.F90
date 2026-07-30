! Robustness: F2018 SELECT RANK construct.
! fparser2 0.2.0 (std=f2008) rejects assumed-rank and SELECT RANK.

module select_rank_helpers
    implicit none
contains
    subroutine fill_ones(x)
        real(8), intent(inout) :: x(..)
        integer :: i
        select rank (x)
        rank (1)
            do i = 1, size(x)
                x(i) = 1.0d0
            end do
        rank default
            continue
        end select
    end subroutine fill_ones
end module select_rank_helpers

program select_rank_case
    use OP2_FORTRAN_DECLARATIONS
    use OP2_FORTRAN_REFERENCE
    use, intrinsic :: ISO_C_BINDING
    use select_rank_helpers

    implicit none

    integer(4), parameter :: nnode = 4
    real(8), dimension(:), allocatable :: u
    real(8) :: alpha

    type(op_set) :: nodes
    type(op_dat) :: p_u

    allocate(u(nnode))
    call fill_ones(u)

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

end program select_rank_case
