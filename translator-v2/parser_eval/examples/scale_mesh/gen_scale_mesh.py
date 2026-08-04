#!/usr/bin/env python3
"""Generate the scale_mesh multi-file OP2 Fortran example (idempotent)."""

from __future__ import annotations

import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent
N_FILES = 8
BODY_REPS = 40


def kernel_body(indent: str = "        ") -> str:
    lines = [
        f"{indent}real(8) :: a, b, c, d, t",
        f"{indent}a = ul + ur",
        f"{indent}b = ul - ur",
        f"{indent}c = 1.0_8",
        f"{indent}d = alpha",
    ]
    for _ in range(BODY_REPS):
        lines.append(f"{indent}t = 0.5_8 * a + 0.5_8 * b * d")
        lines.append(
            f"{indent}c = 0.5_8 * c + 0.5_8 / (1.0_8 + abs(t) * c + 1.0d-30)"
        )
        lines.append(f"{indent}a = a + t * c")
        lines.append(f"{indent}b = b - t / (c + 1.0d-30)")
        lines.append(f"{indent}d = d * 0.999_8 + 0.001_8 * c")
    lines += [
        f"{indent}delta = alpha * (ur - ul) * c / (c + 1.0d-30)",
        f"{indent}dul = dul + delta",
        f"{indent}dur = dur - delta",
    ]
    return "\n".join(lines)


def cell_body(indent: str = "        ") -> str:
    lines = [
        f"{indent}real(8) :: area2, w, s",
        f"{indent}area2 = abs((x2(1) - x1(1)) * (x3(2) - x1(2)) "
        f"- (x3(1) - x1(1)) * (x2(2) - x1(2)))",
        f"{indent}w = 1.0_8 + area2",
        f"{indent}s = du",
    ]
    for _ in range(BODY_REPS // 2):
        lines.append(
            f"{indent}s = 0.5_8 * s + 0.5_8 * w / (1.0_8 + abs(s) + 1.0d-30)"
        )
        lines.append(f"{indent}w = w + 1.0d-6 * s")
    lines += [
        f"{indent}u = u + du",
        f"{indent}residual = residual + abs(du) * w",
        f"{indent}du = 0.0_8",
        f"{indent}u_max = max(u_max, u)",
    ]
    return "\n".join(lines)


def main() -> None:
    (ROOT / "scale_mesh_consts.F90").write_text(
        """module scale_mesh_consts
    implicit none
    private
    public :: alpha
    real(8) :: alpha = 0.15_8
end module
""",
        encoding="utf-8",
    )

    sources = ["scale_mesh_consts.F90"]
    for i in range(1, N_FILES + 1):
        mod = f"scale_mesh_kernels_{i:02d}"
        edge = f"edge_flux_{i:02d}"
        cell = f"cell_update_{i:02d}"
        text = f"""module {mod}
    use scale_mesh_consts
    implicit none
    private
    public :: {edge}, {cell}

contains

    subroutine {edge}(ul, ur, dul, dur)
        implicit none
        real(8), intent(in) :: ul, ur
        real(8), intent(inout) :: dul, dur
        real(8) :: delta
{kernel_body()}
    end subroutine

    subroutine {cell}(x1, x2, x3, u, du, residual, u_max)
        implicit none
        real(8), dimension(2), intent(in) :: x1, x2, x3
        real(8), intent(inout) :: u, du, residual, u_max
{cell_body()}
    end subroutine

end module
"""
        fname = f"{mod}.F90"
        (ROOT / fname).write_text(text, encoding="utf-8")
        sources.append(fname)

    # only kernel 01 drives the time loop; other modules still inflate Stage-1 parse
    main_f90 = f"""! Large multi-file unstructured-mesh example for Stage-1 scaling studies.
! Eight fat kernel modules; only edge_flux_01 / cell_update_01 drive the
! time loop so GPU runtime stays comparable to tri_diff.

program scale_mesh
    use OP2_FORTRAN_DECLARATIONS
    use OP2_FORTRAN_REFERENCE
    use scale_mesh_consts
    use scale_mesh_kernels_01

    use, intrinsic :: ISO_C_BINDING

    implicit none

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

    real(8) :: residual, u_max, u_sum

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

    call op_decl_const(alpha, 1, "real(8)")

    call op_profile_start("SCALE_MESH")

    do i = 1, niter
        call op_par_loop_4(edge_flux_01, edges, &
            op_arg_dat(p_u,  1, pecell, 1, "real(8)", OP_READ), &
            op_arg_dat(p_u,  2, pecell, 1, "real(8)", OP_READ), &
            op_arg_dat(p_du, 1, pecell, 1, "real(8)", OP_INC),  &
            op_arg_dat(p_du, 2, pecell, 1, "real(8)", OP_INC))

        residual = 0.0_8
        u_max = 0.0_8

        call op_par_loop_7(cell_update_01, cells, &
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
    if (abs(u_sum - 1.0_8) < tolerance .and. maxval(u) > 0.0_8 &
            .and. maxval(u) <= 1.0_8 + tolerance) then
        write(*, "(A,I0,A,I0,A,ES12.5,A,ES12.5)") &
            "scale_mesh ok: ncell=", ncell, " nedge=", nedge, &
            " sum(u)=", u_sum, " u_max=", maxval(u)
        print *, "Test PASSED"
    else
        write(*, "(A,ES12.5,A,ES12.5)") &
            "scale_mesh fail: sum(u)=", u_sum, " u_max=", maxval(u)
        print *, "Test FAILED"
    end if

    deallocate(u)
    call op_exit()

end program
"""
    (ROOT / "scale_mesh.F90").write_text(main_f90, encoding="utf-8")
    sources.append("scale_mesh.F90")

    (ROOT / "Makefile").write_text(
        "include ../../../../makefiles/common.mk\n\n"
        "APP_NAME := scale_mesh\n"
        f"APP_SRC := {' '.join(sources)}\n\n"
        "APP_EXTRA_TRANSLATOR_FLAGS := --consts-module scale_mesh_consts.F90\n\n"
        "VARIANT_FILTER_OUT := mpi_%\n\n"
        "include ../../../../makefiles/f_app.mk\n",
        encoding="utf-8",
    )

    ex = {
        "name": "scale_mesh",
        "description": (
            "Large multi-file triangular mesh app for Stage-1 scaling "
            "(8 fat kernel modules + main)"
        ),
        "workdir": ".",
        "sources": sources,
        "translator_flags": ["--consts-module", "scale_mesh_consts.F90"],
        "targets": ["seq", "openmp", "c_cuda"],
        "app_name": "scale_mesh",
        "runtime": {
            "variant": "c_cuda",
            "make_target": "scale_mesh_c_cuda",
            "binary": "./scale_mesh_c_cuda",
            "args": ["OP_PART_SIZE=128", "OP_BLOCK_SIZE=192"],
            "pass_regex": "Test PASSED",
            "timeout_s": 120,
            "runtime_ratio_tol": 0.5,
        },
        "codegen_timeout_s": 300,
    }
    (ROOT / "example.json").write_text(json.dumps(ex, indent=2) + "\n", encoding="utf-8")

    total = 0
    for s in sources:
        n = len((ROOT / s).read_text(encoding="utf-8").splitlines())
        total += n
        print(f"{s}: {n} lines")
    print(f"TOTAL: {total} lines, {len(sources)} files")


if __name__ == "__main__":
    main()
