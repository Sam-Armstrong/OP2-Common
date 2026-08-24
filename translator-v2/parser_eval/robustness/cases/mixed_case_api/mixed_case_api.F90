program mixed_case_api
    use OP2_FORTRAN_DECLARATIONS
    use OP2_FORTRAN_REFERENCE
    use, intrinsic :: ISO_C_BINDING
    implicit none
    integer(4), parameter :: nnode = 4
    real(8), dimension(:), allocatable :: u
    real(8) :: alpha
    type(op_set) :: nodes
    type(op_dat) :: p_u

    allocate(u(nnode))
    u = 1.0d0
    Call Op_Init(0)
    Call Op_Decl_Set(nnode, nodes, "nodes")
    Call Op_Decl_Dat(nodes, 1, "real(8)", u, p_u, "p_u")
    deallocate(u)
    alpha = 2.0d0
    Call Op_Decl_Const(alpha, 1, "real(8)")
    Call Op_Par_Loop_2(Scale, nodes, &
        & Op_Arg_Dat(p_u, -1, OP_ID, 1, "real(8)", OP_RW), &
        & Op_Arg_Gbl(alpha, 1, "real(8)", OP_READ))
    Call Op_Exit()
contains
    subroutine Scale(u, alpha)
        implicit none
        real(8), intent(inout) :: u
        real(8), intent(in) :: alpha
        u = u * alpha
    end subroutine Scale
end program mixed_case_api
