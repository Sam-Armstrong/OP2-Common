program valid_runtime_local
    use OP2_FORTRAN_DECLARATIONS
    use OP2_FORTRAN_REFERENCE
    use, intrinsic :: ISO_C_BINDING
    implicit none
    integer(4), parameter :: nnode = 4
    integer(4) :: n
    real(8), dimension(:), allocatable :: u
    type(op_set) :: nodes
    type(op_dat) :: p_u

    n = 4
    allocate(u(nnode))
    u = 1.0d0
    call op_init(0)
    call op_decl_set(nnode, nodes, "nodes")
    call op_decl_dat(nodes, 1, "real(8)", u, p_u, "p_u")
    deallocate(u)
    call op_decl_const(n, 1, "integer(4)")
    call op_par_loop_2(bad, nodes, &
        & op_arg_dat(p_u, -1, OP_ID, 1, "real(8)", OP_RW), &
        & op_arg_gbl(n, 1, "integer(4)", OP_READ))
    call op_exit()
contains
    subroutine bad(u, n)
        implicit none
        real(8), intent(inout) :: u
        integer(4), intent(in) :: n
        real(8) :: tmp(n)
        tmp(1) = u
        u = tmp(1)
    end subroutine bad
end program valid_runtime_local
