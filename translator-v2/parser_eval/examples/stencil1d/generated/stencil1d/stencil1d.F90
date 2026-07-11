PROGRAM stencil1d
  USE op2_kernels
  USE OP2_FORTRAN_DECLARATIONS
  USE, INTRINSIC :: ISO_C_BINDING
  IMPLICIT NONE
  INTEGER(KIND = 4), PARAMETER :: nnode = 256
  INTEGER(KIND = 4), PARAMETER :: nedge = nnode
  INTEGER(KIND = 4), PARAMETER :: niter = 40
  REAL(KIND = 8), PARAMETER :: tolerance = 1.0D-9
  INTEGER(KIND = 4) :: i
  INTEGER(KIND = 4), DIMENSION(:), ALLOCATABLE :: pp
  REAL(KIND = 8), DIMENSION(:), ALLOCATABLE :: u, du
  REAL(KIND = 8) :: alpha, residual, u_max
  REAL(KIND = 8) :: expected_max
  TYPE(op_set) :: nodes, edges
  TYPE(op_map) :: ppedge
  TYPE(op_dat) :: p_u, p_du
  CALL op_init(0)
  ALLOCATE(pp(2 * nedge))
  ALLOCATE(u(nnode))
  ALLOCATE(du(nnode))
  DO i = 1, nedge
    pp(2 * (i - 1) + 1) = i
    IF (i < nnode) THEN
      pp(2 * (i - 1) + 2) = i + 1
    ELSE
      pp(2 * (i - 1) + 2) = 1
    END IF
  END DO
  u = 0.0_8
  u(1) = 1.0_8
  du = 0.0_8
  CALL op_decl_set(nnode, nodes, "nodes")
  CALL op_decl_set(nedge, edges, "edges")
  CALL op_decl_map(edges, nodes, 2, pp, ppedge, "ppedge")
  CALL op_decl_dat(nodes, 1, "real(8)", u, p_u, "p_u")
  CALL op_decl_dat(nodes, 1, "real(8)", du, p_du, "p_du")
  DEALLOCATE(pp)
  DEALLOCATE(u)
  DEALLOCATE(du)
  alpha = 0.25_8
  CALL op_decl_const_alpha(alpha, 1)
  CALL op_profile_start("STENCIL1D")
  DO i = 1, niter
    CALL op2_k_stencil1d_1_diffuse("diffuse", edges, op_arg_dat(p_u, 1, ppedge, 1, "real(8)", OP_READ), op_arg_dat(p_u, 2, ppedge, 1, "real(8)", OP_READ), op_arg_dat(p_du, 1, ppedge, 1, "real(8)", OP_INC))
    residual = 0.0_8
    u_max = 0.0_8
    CALL op2_k_stencil1d_2_update("update", nodes, op_arg_dat(p_u, - 1, OP_ID, 1, "real(8)", OP_RW), op_arg_dat(p_du, - 1, OP_ID, 1, "real(8)", OP_RW), op_arg_gbl(residual, 1, "real(8)", OP_INC), op_arg_gbl(u_max, 1, "real(8)", OP_MAX))
  END DO
  CALL op_profile_end
  ALLOCATE(u(nnode))
  CALL op_fetch_data(p_u, u)
  expected_max = MAXVAL(u)
  IF (ABS(SUM(u) - 1.0_8) < tolerance .AND. expected_max > 0.0_8 .AND. expected_max <= 1.0_8 + tolerance) THEN
    WRITE(*, "(A,ES12.5,A,ES12.5)") "stencil1d ok: sum(u)=", SUM(u), " u_max=", expected_max
    PRINT *, "Test PASSED"
  ELSE
    WRITE(*, "(A,ES12.5,A,ES12.5)") "stencil1d fail: sum(u)=", SUM(u), " u_max=", expected_max
    PRINT *, "Test FAILED"
  END IF
  DEALLOCATE(u)
  CALL op_exit
  CONTAINS
  SUBROUTINE scale_diff(left, right, out)
    IMPLICIT NONE
    REAL(KIND = 8), INTENT(IN) :: left, right
    REAL(KIND = 8), INTENT(OUT) :: out
    out = alpha * (right - left)
  END SUBROUTINE
  SUBROUTINE diffuse(u_left, u_right, du)
    IMPLICIT NONE
    REAL(KIND = 8), INTENT(IN) :: u_left, u_right
    REAL(KIND = 8), INTENT(INOUT) :: du
    REAL(KIND = 8) :: delta
    CALL scale_diff(u_left, u_right, delta)
    du = du + delta
  END SUBROUTINE
  SUBROUTINE update(u, du, residual, u_max)
    IMPLICIT NONE
    REAL(KIND = 8), INTENT(INOUT) :: u, du, residual, u_max
    u = u + du
    residual = residual + ABS(du)
    du = 0.0_8
    u_max = MAX(u_max, u)
  END SUBROUTINE
END PROGRAM