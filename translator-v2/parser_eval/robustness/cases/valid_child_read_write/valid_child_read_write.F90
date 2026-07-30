program valid_child_read_write
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
    call op_par_loop_1(parent, nodes, &
        & op_arg_dat(p_u, -1, OP_ID, 1, "real(8)", OP_READ))
    call op_exit()
contains
    subroutine parent(u)
        implicit none
        real(8), intent(inout) :: u
        call child(u)
    end subroutine parent

    subroutine child(x)
        implicit none
        real(8), intent(inout) :: x
        x = 1.0d0
    end subroutine child
end program valid_child_read_write
