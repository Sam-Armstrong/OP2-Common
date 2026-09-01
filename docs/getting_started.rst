Getting Started
===============

Manual Build
------------

Toolchain and Build Dependencies
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

- **GNU Make** > 4.2
- **C/C++17 compiler** (GCC, Clang, Cray, Intel, IBM XL, NVHPC).
- Optional: **Fortran compiler** (GFortran, Cray, Intel, IBM XL, NVHPC).
- Optional: **MPI implementation** supporting ``mpicc``, ``mpicxx``, and ``mpif90`` compiler wrappers.
- Optional: **NVIDIA CUDA** >= 11.8
- Optional: **AMD HIP** (ROCm)
- Optional: **CMake** >= 3.20 and **LLVM Flang** >= 23 (parser libraries). Required only to build ``op2-flang-scan``, the helper used by the optional LLVM Flang Fortran parser. See `LLVM Flang (optional Fortran parser)`_ below.

These are likely provided in some form by either your distribution's package manager or pre-installed and loaded via commands such as with `Environment Modules <http://modules.sourceforge.net/>`_.

Library Dependencies
^^^^^^^^^^^^^^^^^^^^

These dependencies can also come from package managers or modules, but they must be built with a specific configuration and the same compiler toolchain that you will use to build OP2.

- Optional: `(PT-)Scotch <https://www.labri.fr/perso/pelegrin/scotch/>`_: Used for mesh partitioning. You must build both the sequential Scotch and parallel PT-Scotch with 32-bit indicies (``-DIDXSIZE=32``) and without threading support (remove ``-DSCOTCH_PTHREAD``).
- Optional: `ParMETIS <http://glaros.dtc.umn.edu/gkhome/metis/parmetis/overview>`_: Used for mesh partitioning.
- Optional: `KaHIP <https://kahip.github.io/>`_: Used for mesh partitioning.
- Optional: `HDF5 <https://www.hdfgroup.org/solutions/hdf5/>`_: Used for HDF5 I/O. You may build with and without ``--enable-parallel`` depending on whether MPI support is needed, and then specify both builds using the environment variables listed below.

.. note::
   Building the MPI-enabled OP2 libraries require a parallel HDF5 build. A sequential HDF5 build is needed only for HDF5 support in the sequential OP2 libraries.

Building
^^^^^^^^

(1) Clone the repository:

.. code-block:: shell

   git clone https://github.com/OP-DSL/OP2-Common.git
   cd OP2-Common

(2) Select your compiler:

.. code-block:: shell

   export OP2_COMPILER={gnu, cray, intel, xl, nvhpc}

Alternatively, for a greater level of control:

.. code-block:: shell

   export OP2_C_COMPILER={gnu, clang, cray, intel, xl, nvhpc}
   export OP2_C_CUDA_COMPILER={nvhpc}  # optional
   export OP2_C_HIP_COMPILER={hip}  # optional
   export OP2_F_COMPILER={gnu, cray, intel, xl, nvhpc}   # optional

.. note::
   In some scenarios you may be able to use a profile rather than specifying an ``OP2_COMPILER``. See `Makefile-README <https://github.com/OP-DSL/OP2-Common/blob/master/makefiles/README.md>`_ for more information.

(3) Set library paths (if needed):

.. code-block:: shell

   export PTSCOTCH_INSTALL_PATH=<path/to/ptscotch>
   export PARMETIS_INSTALL_PATH=<path/to/parmetis>
   export KAHIP_INSTALL_PATH=<path/to/kahip>
   export HDF5_{SEQ, PAR}_INSTALL_PATH=<path/to/hdf5>

   export CUDA_INSTALL_PATH=<path/to/cuda/toolkit>
   export HIP_INSTALL_PATH=<path/to/hip/rocm>
   export LLVM_INSTALL_PATH=<path/to/llvm>   # optional; LLVM+Flang prefix for op2-flang-scan

.. note::
   You may not need to specify the ``X_INSTALL_PATH`` varaibles if the include paths and library search paths are automatically injected by your package manager or module system.

If you are using CUDA or HIP, you may also specify a comma separated list of target architectures for which to generate code for:

.. code-block:: shell

   export NV_ARCH={Pascal, Volta, ..., Hopper}[,{Pascal, ...}]
   export HIP_ARCH={gfx803, gfx90a, ..., gfx908}[,{gfx803, ...}]

(4) Configure the build: 

.. code-block:: shell

    make -C op2 config

.. note::
   Check the terminal log to ensure the compilers, libraries, and flags are as expected.

(5) Build OP2 library and an example app:

.. code-block:: shell

   make -C op2 -j$(nproc)
   make -C apps/c/airfoil/airfoil_plain/dp -j$(nproc)

.. note::
   A new folder ``generated`` will be created inside the example app folder containing the generated source files. The compiled executable will be in the example app folder.

.. note::
   If LLVM Flang was found during ``make config``, ``make -C op2`` also builds the
   ``op2-flang-scan`` helper and installs it to ``op2/bin/op2-flang-scan``.
   You can build just the scanner with ``make -C op2 flang-scan``.

.. warning::
   MPI builds require an MPI wrapper (``mpicxx``) pointing to the compiler defined by ``OP2_COMPILER``. You can manually set the MPI executable path using ``MPI_INSTALL_PATH``.

Application Build Variants
^^^^^^^^^^^^^^^^^^^^^^^^^^

When building an application, the following parallelisation variants are available as Make targets:

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Target
     - Description
   * - ``seq``
     - Single-threaded sequential build using the developer sequential library.
   * - ``genseq``
     - Code-generated sequential build (translator-v2 ``seq`` target). Recommended over ``seq`` for performance measurement.
   * - ``openmp``
     - Multi-threaded CPU build using OpenMP.
   * - ``cuda``
     - NVIDIA GPU build using CUDA (translator-v2 ``cuda`` target, ahead-of-time compiled).
   * - ``hip``
     - AMD GPU build using HIP (translator-v2 ``hip`` target, ahead-of-time compiled).
   * - ``c_cuda``
     - NVIDIA GPU build using CUDA with JIT compilation (translator-v2 ``c_cuda`` target). Device kernels are compiled at application start-up using NVRTC, enabling runtime specialisation.
   * - ``c_hip``
     - AMD GPU build using HIP with JIT compilation (translator-v2 ``c_hip`` target). Device kernels are compiled at application start-up using the HIP RTC library.
   * - ``mpi_<variant>``
     - Distributed-memory MPI variant of any of the above (e.g. ``mpi_cuda``, ``mpi_c_hip``). Requires an MPI-enabled OP2 library build.

For example, to build the JIT CUDA variant of the Airfoil benchmark:

.. code-block:: shell

   make -C apps/c/airfoil/airfoil_plain/dp c_cuda

See :doc:`translator` for details on how to generate the required source files for each variant.

Fortran Application Build Variants
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

Fortran application variants are prefixed with ``f_``:

.. list-table::
   :header-rows: 1
   :widths: 20 80

   * - Target
     - Description
   * - ``f_seq``
     - Sequential Fortran build.
   * - ``f_openmp``
     - OpenMP multi-threaded Fortran build.
   * - ``f_cuda``
     - Native CUDA Fortran build. Requires a CUDA Fortran-capable compiler (NVHPC).
   * - ``f_c_cuda``
     - Fortran interop with JIT CUDA kernels (recommended GPU target for Fortran).
   * - ``f_c_hip``
     - Fortran interop with JIT HIP kernels.
   * - ``f_mpi_<variant>``
     - Distributed-memory MPI variant of any of the above.

For example, to build the Fortran Airfoil benchmark with JIT CUDA:

.. code-block:: shell

   make -C apps/fortran/airfoil f_c_cuda

See :ref:`op2-fortran-api` for the Fortran API reference and :doc:`translator` for Fortran code generation targets.

To use the LLVM Flang parser instead of the default fparser2 frontend when
generating Fortran variants, set ``OP2_FORTRAN_PARSER`` (see below).

LLVM Flang (optional Fortran parser)
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

By default the OP2 translator parses Fortran with **fparser2** (a pip-installed
Python package bundled with translator-v2). An optional **LLVM Flang** frontend
is also available: it uses the Flang parser libraries via a small C++ helper
(``op2-flang-scan``) and typically offers better Fortran standards coverage.
fparser2 remains the default; Flang is opt-in.

Installing LLVM Flang
"""""""""""""""""""""

``op2-flang-scan`` uses LLVM Flang's parse-tree API, which is **not
stable across releases**. The scanner is written for the tuple-class
layout (``CallStmt::t``, ``ArrayElement::Subscripts()``,
``LoopBounds::Name()``, ...) that first shipped in **LLVM 23**.

**Requirement: LLVM Flang >= 23**, including:

- Flang parser headers (``include/flang/Parser/parsing.h``)
- The Flang parser libraries (``libFortranParser`` and related ``Fortran*`` archives)
- The LLVM CMake package (``lib/cmake/llvm/LLVMConfig.cmake``)
- **CMake** >= 3.20 (and optionally **Ninja**)

LLVM 18--22 still expose the older named-member layout and will not
compile the scanner. That includes Ubuntu 24.04 **archive** packages
(``libflang-18-dev`` … ``libflang-20-dev``).

**1. Distro packages (LLVM >= 23)**

On Debian / Ubuntu, official archive packages currently stop at LLVM 20
and are too old. Use the LLVM project's APT repository instead
(`apt.llvm.org <https://apt.llvm.org/>`_), which publishes
``libflang-23-dev`` (and newer) for supported releases including Ubuntu
24.04 (noble):

.. code-block:: shell

   wget https://apt.llvm.org/llvm.sh
   chmod +x llvm.sh
   sudo ./llvm.sh 23
   sudo apt-get install -y libflang-23-dev llvm-23-dev cmake ninja-build
   export LLVM_INSTALL_PATH=/usr/lib/llvm-23

Replace ``23`` with a later version if you prefer (``24``, ...).
``apt search libflang`` will show what is available.

On Fedora (when the packaged Flang is >= 23):

.. code-block:: shell

   sudo dnf install cmake ninja-build flang-devel llvm-devel
   export LLVM_INSTALL_PATH=/usr

**2. Homebrew (macOS)**

.. code-block:: shell

   brew install llvm cmake ninja
   export LLVM_INSTALL_PATH="$(brew --prefix llvm)"

Confirm ``$(brew --prefix llvm)/bin/llvm-config --version`` is **>= 23**.
If configure/compile fails on parse-tree members such as
``CallStmt::t`` or ``ArrayElement::Subscripts``, the Homebrew LLVM is
too old; use a newer bottle or the from-source recipe below.

**3. Build from source**

Build from source if a >= 23 package is not available, or if you want
to track LLVM ``main``. A from-source build of ``llvm-project`` with
Flang and MLIR typically takes 30--60 minutes and around 15--30 GB of
disk. On WSL, build on the Linux filesystem (e.g. ``$HOME``), not
``/mnt/c``.

.. code-block:: shell

   sudo apt-get install -y build-essential cmake ninja-build clang lld git   # debian/ubuntu
   git clone --depth 1 https://github.com/llvm/llvm-project.git
   cd llvm-project
   cmake -S llvm -B build -G Ninja \
       -DCMAKE_BUILD_TYPE=Release \
       -DLLVM_ENABLE_PROJECTS="mlir;flang" \
       -DLLVM_TARGETS_TO_BUILD=host \
       -DCMAKE_INSTALL_PREFIX=$HOME/.local/llvm \
       -DCMAKE_C_COMPILER=clang \
       -DCMAKE_CXX_COMPILER=clang++ \
       -DLLVM_USE_LINKER=lld \
       -DLLVM_ENABLE_ASSERTIONS=OFF \
       -DLLVM_ENABLE_RTTI=ON
   cmake --build build --target install -j$(nproc)
   export LLVM_INSTALL_PATH=$HOME/.local/llvm

``op2-flang-scan`` only needs the parser libraries, not a full Clang/Flang
compiler toolchain, which is why ``LLVM_ENABLE_PROJECTS`` omits ``clang``.
MLIR is required to *build* Flang itself even though the scanner does not
lower to MLIR.

**4. Windows**

Use WSL and either the apt.llvm.org packages or the from-source recipe
above. Native Windows builds of Flang are not routinely tested against
``op2-flang-scan``.

If ``make config`` can see CMake and a valid prefix, it prints
``LLVM Flang FOUND (<prefix>)``. You do not need to set ``LLVM_INSTALL_PATH``
when Flang is already in a well-known location (``$HOME/.local/llvm``,
``llvm-config --prefix``, or ``/usr/lib/llvm-<ver>``).

Building ``op2-flang-scan``
"""""""""""""""""""""""""""

Once LLVM Flang is installed, configure and build OP2 as usual. The library
build compiles the scanner and installs it next to the OP2 libraries:

.. code-block:: shell

   export LLVM_INSTALL_PATH=<path/to/llvm>   # if not auto-detected
   make -C op2 config
   make -C op2 -j$(nproc)

The binary is installed to ``op2/bin/op2-flang-scan``. To build only the scanner:

.. code-block:: shell

   make -C op2 flang-scan

You can also configure the scanner by hand (see ``translator-v2/flang-scan/README.md``):

.. code-block:: shell

   cmake -S translator-v2/flang-scan -B translator-v2/flang-scan/build \
       -DCMAKE_BUILD_TYPE=Release \
       -DCMAKE_PREFIX_PATH=$LLVM_INSTALL_PATH
   cmake --build translator-v2/flang-scan/build

Using the Flang parser
""""""""""""""""""""""

The translator still defaults to fparser2. To switch Fortran code generation
to Flang, set ``OP2_FORTRAN_PARSER`` before building an application:

.. code-block:: shell

   export OP2_FORTRAN_PARSER=flang
   make -C apps/fortran/airfoil airfoil_plain_genseq

Equivalent mechanisms (any one is sufficient):

.. list-table::
   :header-rows: 1
   :widths: 35 65

   * - Variable / flag
     - Description
   * - ``OP2_FORTRAN_PARSER=flang``
     - Preferred. Forwarded by ``makefiles/f_app.mk`` as ``--parser flang``.
       Allowed values: ``fparser2`` (default), ``flang``.
   * - ``OP2_EXTRA_TRANSLATOR_FLAGS=--parser flang``
     - Extra flags appended to every translator invocation (Fortran and any
       other flags you already pass this way).
   * - ``OP2_FLANG_SCAN=<path>``
     - Optional path to the ``op2-flang-scan`` binary. If unset, the translator
       looks for ``op2/bin/op2-flang-scan``, then
       ``translator-v2/flang-scan/build/op2-flang-scan``, then ``PATH``.
   * - ``--parser flang`` / ``--flang-scan <path>``
     - Same options when invoking the translator directly
       (see :doc:`translator`).

Example — generate and run sequential Airfoil with Flang:

.. code-block:: shell

   export OP2_COMPILER=gnu
   export LLVM_INSTALL_PATH=/usr/lib/llvm-23   # or $HOME/.local/llvm
   make -C op2 config
   make -C op2 -j$(nproc)
   export OP2_FORTRAN_PARSER=flang
   make -C apps/fortran/airfoil airfoil_plain_genseq
   ./apps/fortran/airfoil/airfoil_plain_genseq

If Flang fails to parse a file, that file falls back to fparser2 automatically.

Spack
-----

A Spack package for OP2 is not yet available. Building from source using the manual steps above is currently the recommended installation method.

If you are using a Spack-managed environment, the required compilers and libraries (MPI, CUDA, HDF5) will generally be available through the Spack-generated environment or compiler wrappers. Once the appropriate modules or environment is activated, follow the manual build steps. You do not need to set ``X_INSTALL_PATH`` variables if the include and library paths are already injected by the module system.
