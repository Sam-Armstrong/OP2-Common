module const_mod
    use OP2_FORTRAN_DECLARATIONS
    implicit none
contains
    subroutine declare_alpha(alpha)
        real(8), intent(in) :: alpha
        call op_decl_const(alpha, 1, "real(8)")
    end subroutine declare_alpha
end module const_mod
