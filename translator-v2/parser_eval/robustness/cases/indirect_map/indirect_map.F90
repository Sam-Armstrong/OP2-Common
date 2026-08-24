program indirect_map
    use OP2_FORTRAN_DECLARATIONS
    use OP2_FORTRAN_REFERENCE
    use, intrinsic :: ISO_C_BINDING
    implicit none
    integer(4), parameter :: nnode = 4, nedge = 4
    integer(4), dimension(:), allocatable :: ecell
    real(8), dimension(:), allocatable :: u, du
    type(op_set) :: nodes, edges
    type(op_map) :: pecell
    type(op_dat) :: p_u, p_du

    allocate(u(nnode), du(nnode), ecell(2 * nedge))
    u = 1.0d0
    du = 0.0d0
    ecell = [1, 2, 2, 3, 3, 4, 4, 1]
    call op_init(0)
    call op_decl_set(nnode, nodes, "nodes")
    call op_decl_set(nedge, edges, "edges")
    call op_decl_map(edges, nodes, 2, ecell, pecell, "pecell")
    call op_decl_dat(nodes, 1, "real(8)", u, p_u, "p_u")
    call op_decl_dat(nodes, 1, "real(8)", du, p_du, "p_du")
    deallocate(u, du, ecell)
    call op_par_loop_3(flux, edges, &
        & op_arg_dat(p_u,  1, pecell, 1, "real(8)", OP_READ), &
        & op_arg_dat(p_u,  2, pecell, 1, "real(8)", OP_READ), &
        & op_arg_dat(p_du, 1, pecell, 1, "real(8)", OP_INC))
    call op_exit()
contains
    subroutine flux(u1, u2, du)
        implicit none
        real(8), intent(in) :: u1, u2
        real(8), intent(inout) :: du
        du = du + (u1 - u2)
    end subroutine flux
end program indirect_map
