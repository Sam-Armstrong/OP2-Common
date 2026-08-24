program valid_arg_idx_write
    use OP2_FORTRAN_DECLARATIONS
    use OP2_FORTRAN_REFERENCE
    use, intrinsic :: ISO_C_BINDING
    implicit none
    integer(4), parameter :: nnode = 4
    real(8), dimension(:), allocatable :: u
    type(op_set) :: nodes
    type(op_dat) :: p_u

    allocate(u(nnode))
    u = 1.0d0
    call op_init(0)
    call op_decl_set(nnode, nodes, "nodes")
    call op_decl_dat(nodes, 1, "real(8)", u, p_u, "p_u")
    deallocate(u)
    call op_par_loop_2(bad, nodes, &
        & op_arg_dat(p_u, -1, OP_ID, 1, "real(8)", OP_READ), &
        & op_arg_idx(-1, OP_ID))
    call op_exit()
contains
    subroutine bad(u, idx)
        implicit none
        real(8), intent(in) :: u
        integer(4), intent(inout) :: idx
        idx = 0
        if (u < 0.0d0) idx = 1
    end subroutine bad
end program valid_arg_idx_write
