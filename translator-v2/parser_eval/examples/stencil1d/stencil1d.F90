! Self-contained 1D Jacobi-style stencil for parser evaluation.
! Exercises: OP_ID / mapped INC, globals (INC/MAX), a const, and a helper
! subroutine dependency from a kernel (non-trivial depends tree).

program stencil1d
    use OP2_FORTRAN_DECLARATIONS
    use OP2_FORTRAN_REFERENCE

    use, intrinsic :: ISO_C_BINDING

    implicit none

    integer(4), parameter :: nnode = 256
    integer(4), parameter :: nedge = nnode
    integer(4), parameter :: niter = 40
    real(8), parameter :: tolerance = 1.0d-9

    integer(4) :: i
    integer(4), dimension(:), allocatable :: pp
    real(8), dimension(:), allocatable :: u, du

    real(8) :: alpha, residual, u_max
    real(8) :: expected_max

    type(op_set) :: nodes, edges
    type(op_map) :: ppedge
    type(op_dat) :: p_u, p_du

    call op_init(0)

    allocate(pp(2 * nedge))
    allocate(u(nnode))
    allocate(du(nnode))

    ! ring: edge i connects node i -> node i+1 (wrap)
    do i = 1, nedge
        pp(2 * (i - 1) + 1) = i
        if (i < nnode) then
            pp(2 * (i - 1) + 2) = i + 1
        else
            pp(2 * (i - 1) + 2) = 1
        end if
    end do

    ! initial condition: unit impulse at node 1
    u = 0.0_8
    u(1) = 1.0_8
    du = 0.0_8

    call op_decl_set(nnode, nodes, "nodes")
    call op_decl_set(nedge, edges, "edges")
    call op_decl_map(edges, nodes, 2, pp, ppedge, "ppedge")

    call op_decl_dat(nodes, 1, "real(8)", u, p_u, "p_u")
    call op_decl_dat(nodes, 1, "real(8)", du, p_du, "p_du")

    deallocate(pp)
    deallocate(u)
    deallocate(du)

    alpha = 0.25_8
    call op_decl_const(alpha, 1, "real(8)")

    call op_profile_start("STENCIL1D")

    do i = 1, niter
        call op_par_loop_3(diffuse, edges, &
            op_arg_dat(p_u,  1, ppedge, 1, "real(8)", OP_READ), &
            op_arg_dat(p_u,  2, ppedge, 1, "real(8)", OP_READ), &
            op_arg_dat(p_du, 1, ppedge, 1, "real(8)", OP_INC))

        residual = 0.0_8
        u_max = 0.0_8

        call op_par_loop_4(update, nodes, &
            op_arg_dat(p_u,  -1, OP_ID, 1, "real(8)", OP_RW),  &
            op_arg_dat(p_du, -1, OP_ID, 1, "real(8)", OP_RW),  &
            op_arg_gbl(residual, 1, "real(8)", OP_INC), &
            op_arg_gbl(u_max, 1, "real(8)", OP_MAX))
    end do

    call op_profile_end()

    allocate(u(nnode))
    call op_fetch_data(p_u, u)

    ! mass is conserved on the ring under this stencil; max must stay in (0,1]
    expected_max = maxval(u)
    if (abs(sum(u) - 1.0_8) < tolerance .and. expected_max > 0.0_8 .and. expected_max <= 1.0_8 + tolerance) then
        write(*, "(A,ES12.5,A,ES12.5)") "stencil1d ok: sum(u)=", sum(u), " u_max=", expected_max
        print *, "Test PASSED"
    else
        write(*, "(A,ES12.5,A,ES12.5)") "stencil1d fail: sum(u)=", sum(u), " u_max=", expected_max
        print *, "Test FAILED"
    end if

    deallocate(u)
    call op_exit()

contains

    ! helper used by diffuse — keeps a non-trivial depends edge in the store
    subroutine scale_diff(left, right, out)
        implicit none
        real(8), intent(in) :: left, right
        real(8), intent(out) :: out

        out = alpha * (right - left)
    end subroutine

    subroutine diffuse(u_left, u_right, du)
        implicit none
        real(8), intent(in) :: u_left, u_right
        real(8), intent(inout) :: du
        real(8) :: delta

        call scale_diff(u_left, u_right, delta)
        du = du + delta
    end subroutine

    subroutine update(u, du, residual, u_max)
        implicit none
        real(8), intent(inout) :: u, du, residual, u_max

        u = u + du
        residual = residual + abs(du)
        du = 0.0_8
        u_max = max(u_max, u)
    end subroutine

end program
