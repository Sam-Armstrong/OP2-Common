program kernel_logical
    use OP2_FORTRAN_DECLARATIONS
    use OP2_FORTRAN_REFERENCE
    use, intrinsic :: ISO_C_BINDING
    implicit none
    integer(4), parameter :: nnode = 4
    logical(4), dimension(:), allocatable :: mask
    type(op_set) :: nodes
    type(op_dat) :: p_mask

    allocate(mask(nnode))
    mask = .true.
    call op_init(0)
    call op_decl_set(nnode, nodes, "nodes")
    call op_decl_dat(nodes, 1, "logical", mask, p_mask, "p_mask")
    deallocate(mask)
    call op_par_loop_1(flip, nodes, &
        & op_arg_dat(p_mask, -1, OP_ID, 1, "logical", OP_RW))
    call op_exit()
contains
    subroutine flip(mask)
        implicit none
        logical, intent(inout) :: mask
        mask = .not. mask
    end subroutine flip
end program kernel_logical
