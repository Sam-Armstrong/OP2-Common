program do_concurrent_local_init
    use OP2_FORTRAN_DECLARATIONS
    use OP2_FORTRAN_REFERENCE
    use, intrinsic :: ISO_C_BINDING
    implicit none
    integer(4), parameter :: nnode = 4
    integer(4) :: i, t
    real(8), dimension(4) :: u
    real(8) :: alpha
    type(op_set) :: nodes
    type(op_dat) :: p_u

    t = 1
    do concurrent (i = 1:nnode) local_init(t)
        u(i) = real(t, 8)
    end do

    call op_init(0)
    call op_decl_set(nnode, nodes, "nodes")
    call op_decl_dat(nodes, 1, "real(8)", u, p_u, "p_u")
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
end program do_concurrent_local_init
