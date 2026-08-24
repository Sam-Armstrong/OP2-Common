program arg_idx_info
    use OP2_FORTRAN_DECLARATIONS
    use OP2_FORTRAN_REFERENCE
    use, intrinsic :: ISO_C_BINDING
    implicit none
    integer(4), parameter :: nnode = 4
    real(8), dimension(:), allocatable :: u
    integer(4) :: errloc
    type(op_set) :: nodes
    type(op_dat) :: p_u

    allocate(u(nnode))
    u = 1.0d0
    call op_init(0)
    call op_decl_set(nnode, nodes, "nodes")
    call op_decl_dat(nodes, 1, "real(8)", u, p_u, "p_u")
    deallocate(u)
    errloc = 0
    call op_par_loop_3(mark, nodes, &
        & op_arg_dat(p_u, -1, OP_ID, 1, "real(8)", OP_READ), &
        & op_arg_idx(-1, OP_ID), &
        & op_arg_info(errloc, 1, "integer(4)", 1))
    call op_exit()
contains
    subroutine mark(u, idx, errloc)
        implicit none
        real(8), intent(in) :: u
        integer(4), intent(in) :: idx
        integer(4), intent(out) :: errloc
        if (u > 0.5d0) errloc = idx
    end subroutine mark
end program arg_idx_info
