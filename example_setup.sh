export OP2_COMPILER="{gnu, cray, intel, xl, nvhpc}"

# export PTSCOTCH_INSTALL_PATH=<path/to/ptscotch>
# export PARMETIS_INSTALL_PATH=<path/to/parmetis>
# export KAHIP_INSTALL_PATH=<path/to/kahip>
# export HDF5_{SEQ, PAR}_INSTALL_PATH=<path/to/hdf5>
# export CUDA_INSTALL_PATH=<path/to/cuda/toolkit>
# export NV_ARCH={Fermi, Kepler, ..., Ampere}[,{Fermi, ...}]

make -C op2 config
make -C op2 -j$(nproc)
make -C apps/c/airfoil/airfoil_plain/dp -j$(nproc)
