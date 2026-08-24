program valid_slice_gbl
    use OP2_FORTRAN_DECLARATIONS
    use OP2_FORTRAN_REFERENCE
    use, intrinsic :: ISO_C_BINDING
    implicit none
    integer(4), parameter :: nnode = 4
    real(8), dimension(:), allocatable :: u
    real(8) :: rms(2)
    type(op_set) :: nodes
    type(op_dat) :: p_u

    allocate(u(nnode))
    u = 1.0d0
    call op_init(0)
    call op_decl_set(nnode, nodes, "nodes")
    call op_decl_dat(nodes, 1, "real(8)", u, p_u, "p_u")
    deallocate(u)
    rms = 0.0d0
    call op_par_loop_2(bad, nodes, &
        & op_arg_dat(p_u, -1, OP_ID, 1, "real(8)", OP_READ), &
        & op_arg_gbl(rms, 2, "real(8)", OP_INC))
    call op_exit()
contains
    subroutine bad(u, rms)
        implicit none
        real(8), intent(in) :: u
        real(8), dimension(2), intent(inout) :: rms
        rms(:) = rms(:) + u
    end subroutine bad
end program valid_slice_gbl
