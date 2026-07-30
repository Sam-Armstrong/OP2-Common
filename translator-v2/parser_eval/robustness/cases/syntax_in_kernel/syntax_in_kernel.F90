program syntax_in_kernel
    use OP2_FORTRAN_DECLARATIONS
    use OP2_FORTRAN_REFERENCE
    use, intrinsic :: ISO_C_BINDING
    implicit none
    integer(4), parameter :: nnode = 4
    real(8), dimension(:), allocatable :: u
    real(8) :: alpha
    type(op_set) :: nodes
    type(op_dat) :: p_u

    allocate(u(4 * nnode))
    u = 1.0d0
    call op_init(0)
    call op_decl_set(nnode, nodes, "nodes")
    call op_decl_dat(nodes, 4, "real(8)", u, p_u, "p_u")
    deallocate(u)
    alpha = 2.0d0
    call op_decl_const(alpha, 1, "real(8)")
    call op_par_loop_2(scale_vec, nodes, &
        & op_arg_dat(p_u, -1, OP_ID, 4, "real(8)", OP_RW), &
        & op_arg_gbl(alpha, 1, "real(8)", OP_READ))
    call op_exit()
contains
    subroutine scale_vec(u, alpha)
        implicit none
        real(8), dimension(4), intent(inout) :: u
        real(8), intent(in) :: alpha
        integer :: i
        do concurrent (i = 1:4) shared(u, alpha)
            u(i) = u(i) * alpha
        end do
    end subroutine scale_vec
end program syntax_in_kernel
