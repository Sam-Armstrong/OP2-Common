#define UNUSED(x) if (.false.) print *, SHAPE(x)

module op2_kernels

    use iso_c_binding

    use op2_fortran_declarations
    use op2_fortran_rt_support

    use op2_consts

    use op2_m_stencil1d_1_diffuse
    use op2_m_stencil1d_2_update

end module