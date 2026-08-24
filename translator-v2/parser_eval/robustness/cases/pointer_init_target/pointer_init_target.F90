! Robustness: F2003/F2008 data-pointer initialisation to a non-null target.
! Valid Fortran (R505/C511); fparser2 0.2.4 rejects it (stfc/fparser#334).
! Not a newer-standard gap: INTEGER, POINTER :: p => NULL() parses; => var does not.

program pointer_init_target
    use OP2_FORTRAN_DECLARATIONS
    use OP2_FORTRAN_REFERENCE
    use, intrinsic :: ISO_C_BINDING
    implicit none
    integer(4), parameter :: nnode = 4
    integer, target :: var
    integer, pointer :: ptr => var
    real(8), dimension(:), allocatable :: u
    real(8) :: alpha
    type(op_set) :: nodes
    type(op_dat) :: p_u

    var = 1
    if (.not. associated(ptr)) ptr => var

    allocate(u(nnode))
    u = 1.0d0
    call op_init(0)
    call op_decl_set(nnode, nodes, "nodes")
    call op_decl_dat(nodes, 1, "real(8)", u, p_u, "p_u")
    deallocate(u)
    alpha = real(ptr, 8)
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
end program pointer_init_target
