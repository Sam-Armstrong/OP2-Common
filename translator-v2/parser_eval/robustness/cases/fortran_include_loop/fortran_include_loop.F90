! Inverse robustness: Fortran INCLUDE of an op_par_loop site.
! fparser2 resolves INCLUDE via include_dirs; op2-flang-scan receives
! preprocessed text over stdin, materialises it under /tmp, and Flang
! then fails to find the .inc file -> Stage 1 fparser2 fallback.

program fortran_include_loop
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
    call op_init(0)
    call op_decl_set(nnode, nodes, "nodes")
    call op_decl_dat(nodes, 1, "real(8)", u, p_u, "p_u")
    deallocate(u)
    alpha = 2.0d0
    call op_decl_const(alpha, 1, "real(8)")
      include "loop_site.inc"
    call op_exit()
contains
    subroutine scale(u, alpha)
        implicit none
        real(8), intent(inout) :: u
        real(8), intent(in) :: alpha
        u = u * alpha
    end subroutine scale
end program fortran_include_loop
