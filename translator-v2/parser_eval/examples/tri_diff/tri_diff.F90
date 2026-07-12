! Unstructured triangular-mesh cell diffusion for OP2 parser evaluation.
! Splits each quad of an Nx x Ny lattice into two triangles and builds
! interior edges between adjacent cells — classic OP2 edge->cell indirection.

program tri_diff
    use OP2_FORTRAN_DECLARATIONS
    use OP2_FORTRAN_REFERENCE

    use, intrinsic :: ISO_C_BINDING

    implicit none

    ! ~7.2M triangles; sized for ~10-30s on c_cuda with airfoil-like maps
    integer(4), parameter :: nx = 1900
    integer(4), parameter :: ny = 1900
    integer(4), parameter :: nnode = (nx + 1) * (ny + 1)
    integer(4), parameter :: ncell = 2 * nx * ny
    integer(4), parameter :: niter = 700
    real(8), parameter :: tolerance = 1.0d-6

    integer(4) :: i, j, n, c, e, nedge
    integer(4) :: n0, n1, n2, n3
    integer(4) :: c_bl, c_br, c_tl, c_tr
    integer(4), dimension(:), allocatable :: ecell, cell
    real(8), dimension(:), allocatable :: u, du, x

    real(8) :: alpha, residual, u_max, u_sum

    type(op_set) :: nodes, cells, edges
    type(op_map) :: pecell, pcell
    type(op_dat) :: p_u, p_du, p_x

    call op_init(0)

    allocate(ecell(2 * (3 * nx * ny)))
    allocate(cell(3 * ncell))
    allocate(u(ncell))
    allocate(du(ncell))
    allocate(x(2 * nnode))

    do j = 0, ny
        do i = 0, nx
            n = i + j * (nx + 1) + 1
            x(2 * (n - 1) + 1) = real(i, 8) / real(nx, 8)
            x(2 * (n - 1) + 2) = real(j, 8) / real(ny, 8)
        end do
    end do

    ! two triangles per quad:
    !   n3 -- n2
    !   |  \  |
    !   n0 -- n1
    c = 1
    do j = 0, ny - 1
        do i = 0, nx - 1
            n0 = i + j * (nx + 1) + 1
            n1 = (i + 1) + j * (nx + 1) + 1
            n2 = (i + 1) + (j + 1) * (nx + 1) + 1
            n3 = i + (j + 1) * (nx + 1) + 1

            cell(3 * (c - 1) + 1) = n0
            cell(3 * (c - 1) + 2) = n1
            cell(3 * (c - 1) + 3) = n2
            c = c + 1

            cell(3 * (c - 1) + 1) = n0
            cell(3 * (c - 1) + 2) = n2
            cell(3 * (c - 1) + 3) = n3
            c = c + 1
        end do
    end do

    e = 1
    do j = 0, ny - 1
        do i = 0, nx - 1
            c_bl = 2 * (i + j * nx) + 1
            c_tl = c_bl + 1

            ecell(2 * (e - 1) + 1) = c_bl
            ecell(2 * (e - 1) + 2) = c_tl
            e = e + 1

            if (i < nx - 1) then
                c_br = 2 * ((i + 1) + j * nx) + 1
                ecell(2 * (e - 1) + 1) = c_bl
                ecell(2 * (e - 1) + 2) = c_br
                e = e + 1
            end if

            if (j < ny - 1) then
                c_tr = 2 * (i + (j + 1) * nx) + 2
                ecell(2 * (e - 1) + 1) = c_tl
                ecell(2 * (e - 1) + 2) = c_tr
                e = e + 1
            end if
        end do
    end do
    nedge = e - 1

    u = 0.0_8
    u(1) = 1.0_8
    du = 0.0_8

    call op_decl_set(nnode, nodes, "nodes")
    call op_decl_set(ncell, cells, "cells")
    call op_decl_set(nedge, edges, "edges")

    call op_decl_map(edges, cells, 2, ecell, pecell, "pecell")
    call op_decl_map(cells, nodes, 3, cell, pcell, "pcell")

    call op_decl_dat(nodes, 2, "real(8)", x, p_x, "p_x")
    call op_decl_dat(cells, 1, "real(8)", u, p_u, "p_u")
    call op_decl_dat(cells, 1, "real(8)", du, p_du, "p_du")

    deallocate(ecell)
    deallocate(cell)
    deallocate(u)
    deallocate(du)
    deallocate(x)

    alpha = 0.15_8
    call op_decl_const(alpha, 1, "real(8)")

    call op_profile_start("TRI_DIFF")

    do i = 1, niter
        call op_par_loop_4(edge_flux, edges, &
            op_arg_dat(p_u,  1, pecell, 1, "real(8)", OP_READ), &
            op_arg_dat(p_u,  2, pecell, 1, "real(8)", OP_READ), &
            op_arg_dat(p_du, 1, pecell, 1, "real(8)", OP_INC),  &
            op_arg_dat(p_du, 2, pecell, 1, "real(8)", OP_INC))

        residual = 0.0_8
        u_max = 0.0_8

        ! cell->node map used for a geometric weight (unstructured access)
        call op_par_loop_7(cell_update, cells, &
            op_arg_dat(p_x,   1, pcell, 2, "real(8)", OP_READ), &
            op_arg_dat(p_x,   2, pcell, 2, "real(8)", OP_READ), &
            op_arg_dat(p_x,   3, pcell, 2, "real(8)", OP_READ), &
            op_arg_dat(p_u,  -1, OP_ID, 1, "real(8)", OP_RW), &
            op_arg_dat(p_du, -1, OP_ID, 1, "real(8)", OP_RW), &
            op_arg_gbl(residual, 1, "real(8)", OP_INC), &
            op_arg_gbl(u_max, 1, "real(8)", OP_MAX))
    end do

    call op_profile_end()

    allocate(u(ncell))
    call op_fetch_data(p_u, u)

    u_sum = sum(u)
    if (abs(u_sum - 1.0_8) < tolerance .and. maxval(u) > 0.0_8 .and. maxval(u) <= 1.0_8 + tolerance) then
        write(*, "(A,I0,A,I0,A,ES12.5,A,ES12.5)") &
            "tri_diff ok: ncell=", ncell, " nedge=", nedge, " sum(u)=", u_sum, " u_max=", maxval(u)
        print *, "Test PASSED"
    else
        write(*, "(A,ES12.5,A,ES12.5)") "tri_diff fail: sum(u)=", u_sum, " u_max=", maxval(u)
        print *, "Test FAILED"
    end if

    deallocate(u)
    call op_exit()

contains

    subroutine flux_pair(ul, ur, dul, dur)
        implicit none
        real(8), intent(in) :: ul, ur
        real(8), intent(inout) :: dul, dur
        integer(4), parameter :: burn = 64
        integer(4) :: k
        real(8) :: delta, damp

        ! dependent recurrence so the compiler cannot fold the burn away
        delta = alpha * (ur - ul)
        damp = 1.0_8
        do k = 1, burn
            damp = 0.5_8 * damp + 0.5_8 / (1.0_8 + abs(delta) * damp)
        end do
        delta = delta * (damp / (damp + 1.0d-30)) * ((damp + 1.0d-30) / damp)
        dul = dul + delta
        dur = dur - delta
    end subroutine

    subroutine edge_flux(ul, ur, dul, dur)
        implicit none
        real(8), intent(in) :: ul, ur
        real(8), intent(inout) :: dul, dur

        call flux_pair(ul, ur, dul, dur)
    end subroutine

    subroutine cell_update(x1, x2, x3, u, du, residual, u_max)
        implicit none
        real(8), dimension(2), intent(in) :: x1, x2, x3
        real(8), intent(inout) :: u, du, residual, u_max
        real(8) :: area2

        ! touch geometry via cell->node map (area used only in residual)
        area2 = abs((x2(1) - x1(1)) * (x3(2) - x1(2)) - (x3(1) - x1(1)) * (x2(2) - x1(2)))
        u = u + du
        residual = residual + abs(du) * (1.0_8 + area2)
        du = 0.0_8
        u_max = max(u_max, u)
    end subroutine

end program
