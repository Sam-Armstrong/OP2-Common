program opt_arg_dat
    use OP2_FORTRAN_DECLARATIONS
    use OP2_FORTRAN_REFERENCE
    use, intrinsic :: ISO_C_BINDING
    implicit none
    integer(4), parameter :: nnode = 4
    logical :: use_u
    real(8), dimension(:), allocatable :: u
    type(op_set) :: nodes
    type(op_dat) :: p_u

    allocate(u(nnode))
    u = 1.0d0
    call op_init(0)
    call op_decl_set(nnode, nodes, "nodes")
    call op_decl_dat(nodes, 1, "real(8)", u, p_u, "p_u")
    deallocate(u)
    use_u = .true.
    call op_par_loop_1(bump, nodes, &
        & op_opt_arg_dat(use_u, p_u, -1, OP_ID, 1, "real(8)", OP_RW))
    call op_exit()
contains
    subroutine bump(u)
        implicit none
        real(8), intent(inout) :: u
        u = 1.0d0
    end subroutine bump
end program opt_arg_dat
