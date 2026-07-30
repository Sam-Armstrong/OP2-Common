module rb_kernels
    implicit none
    private
    public :: scale
contains
    subroutine scale(u, alpha)
        implicit none
        real(8), intent(inout) :: u
        real(8), intent(in) :: alpha
        u = u * alpha
    end subroutine scale
end module rb_kernels
