module proc_ptr_helpers
    implicit none
    abstract interface
        subroutine cb
        end subroutine cb
    end interface
    procedure(cb), pointer, nopass :: eval => dummy
contains
    subroutine dummy
    end subroutine dummy
end module proc_ptr_helpers

program procedure_pointer_init
    use OP2_FORTRAN_DECLARATIONS
    use OP2_FORTRAN_REFERENCE
    use, intrinsic :: ISO_C_BINDING
    use proc_ptr_helpers
    implicit none
    integer(4), parameter :: nnode = 4
    real(8), dimension(:), allocatable :: u
    real(8) :: alpha
    type(op_set) :: nodes
    type(op_dat) :: p_u

    if (associated(eval)) call eval()
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
contains
    subroutine scale(u, alpha)
        implicit none
        real(8), intent(inout) :: u
        real(8), intent(in) :: alpha
        u = u * alpha
    end subroutine scale
end program procedure_pointer_init
