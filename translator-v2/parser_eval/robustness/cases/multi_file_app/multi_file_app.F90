program multi_file_app
    use OP2_FORTRAN_DECLARATIONS
    use OP2_FORTRAN_REFERENCE
    use, intrinsic :: ISO_C_BINDING
    use rb_kernels
    implicit none
    integer(4), parameter :: nnode = 4
    real(8), dimension(:), allocatable :: u
    real(8) :: alpha
    type(op_set) :: nodes
    type(op_dat) :: p_u

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
end program multi_file_app
