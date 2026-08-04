include ../../../../makefiles/common.mk

APP_NAME := scale_mesh
APP_SRC := scale_mesh_consts.F90 scale_mesh_kernels_01.F90 scale_mesh_kernels_02.F90 scale_mesh_kernels_03.F90 scale_mesh_kernels_04.F90 scale_mesh_kernels_05.F90 scale_mesh_kernels_06.F90 scale_mesh_kernels_07.F90 scale_mesh_kernels_08.F90 scale_mesh.F90

APP_EXTRA_TRANSLATOR_FLAGS := --consts-module scale_mesh_consts.F90

VARIANT_FILTER_OUT := mpi_%

include ../../../../makefiles/f_app.mk
