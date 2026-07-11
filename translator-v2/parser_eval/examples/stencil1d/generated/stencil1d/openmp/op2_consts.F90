#define UNUSED(x) if (.false.) print *, SHAPE(x)

module op2_consts

    implicit none

    real(8) :: op2_const_alpha

contains

    subroutine op_decl_const_alpha(ptr, dim)
        real(8) :: ptr
        integer(4) :: dim

        UNUSED(dim)
        op2_const_alpha = ptr
    end subroutine

end module