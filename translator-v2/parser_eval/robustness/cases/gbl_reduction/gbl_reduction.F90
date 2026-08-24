program gbl_reduction
    use OP2_FORTRAN_DECLARATIONS
    use OP2_FORTRAN_REFERENCE
    use, intrinsic :: ISO_C_BINDING
    implicit none
    integer(4), parameter :: nnode = 4
    real(8), dimension(:), allocatable :: u
    real(8) :: s, mx
    type(op_set) :: nodes
    type(op_dat) :: p_u

    allocate(u(nnode))
    u = 1.0d0
    call op_init(0)
    call op_decl_set(nnode, nodes, "nodes")
    call op_decl_dat(nodes, 1, "real(8)", u, p_u, "p_u")
    deallocate(u)
    s = 0.0d0
    mx = 0.0d0
    call op_par_loop_3(accum, nodes, &
        & op_arg_dat(p_u, -1, OP_ID, 1, "real(8)", OP_READ), &
        & op_arg_gbl(s, 1, "real(8)", OP_INC), &
        & op_arg_gbl(mx, 1, "real(8)", OP_MAX))
    call op_exit()
contains
    subroutine accum(u, s, mx)
        implicit none
        real(8), intent(in) :: u
        real(8), intent(inout) :: s, mx
        s = s + u
        if (u > mx) mx = u
    end subroutine accum
end program gbl_reduction
