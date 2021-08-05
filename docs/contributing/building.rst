.. _building:

========
Building
========

Setting up a Build
==================

The quickest way to start hacking on Katana is to look at
``scripts/setup_dev_ubuntu.sh`` and use that as the basis for installing a
development environment on your own machine.

The Katana repo supports both Conan and Conda for installing additional library
dependencies on top of the system libraries dependencies installed by
``scripts/setup_dev_ubuntu.sh``.

If you are not familar with either of Conan or Conda, follow the instructions
for Conda.

.. warning::
   
   Conan and conda builds are incompatible. If you mix artifacts, build
   directories, configuration, etc. from one system to the other, you will get
   build and linker errors, and possibly, dynamic library loading errors.

Conda
-----

`Install conda <https://docs.conda.io/en/latest/miniconda.html>`_ if needed.
See the `Conda User Guide <https://docs.conda.io/projects/conda/en/latest/user-guide/install/index.html>`_ for more details.

.. code-block:: bash

   wget https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-x86_64.sh
   bash Miniconda3-latest-Linux-x86_64.sh

You will need to log out and back in again to ensure conda is properly
configured. Then create and activate the development environment:

.. code-block:: bash

   SRC_DIR=<repo/root>
   conda config --add channels conda-forge
   conda env create --name katana-dev --file $SRC_DIR/conda_recipe/environment.yml
   conda activate katana-dev
   conda install numactl-devel-cos6-x86_64 # For x86_64 builds

Now, run ``cmake`` to configure your build directory and ``make`` to build Katana.

.. code-block:: bash

   BUILD_DIR=$(pwd)/build
   mkdir -p $BUILD_DIR
   cd $BUILD_DIR
   cmake -S $SRC_DIR -B $BUILD_DIR -DKATANA_LANG_BINDINGS=python
   make

This will build Katana and place the built libraries and executables in
``$BUILD_DIR``.

Conda Performance
^^^^^^^^^^^^^^^^^

Conda is slow to install packages.
This makes installing a new development take a few minutes.
More importantly, it makes conda package building very slow (~40 minutes for this repository), because the build process installs at least 7 conda environments.
This can be mitigated by using `Mamba <https://github.com/mamba-org/mamba#the-fast-cross-platform-package-manager>`_.
Mamba is a (mostly) drop-in replacement for the ``conda`` command that uses a native dependency solver and reduces installation time by 2x in many cases.
However, Mamba is not as stable or well tested as Conda and does not have the same level of support.

To use Mamba, install it in your conda environment with ``conda install mamba``.
Then you can use ``mamba install`` as a drop-in replacement for ``conda install``, and similarly for ``mamba env create`` and ``mamba env update``.
To use Mamba during conda package builds, install `Boa <https://github.com/mamba-org/boa#the-fast-conda-and-mamba-package-builder>`_ with ``mamba install boa``.
Then you can use ``conda mambabuild`` (*note:* the top level command is ``conda``, *not* ``mamba``) as a replacement for ``conda build``.
(We are not using Boa proper as the package builder.)

To get a leaner, Mamba using environment in a fresh install, use `Mambaforge <https://github.com/conda-forge/miniforge#mambaforge>`_.
It is an installer, similar to miniconda, which installs an environment with conda-forge packages and mamba pre-installed (boa must still be installed separately).

Conan
-----

After running ``scripts/setup_dev_ubuntu.sh``, run the following commands from
the project source directory to build the system:

.. code-block:: bash

   conan profile update settings.compiler.libcxx=libstdc++11 default

   BUILD_DIR=$(pwd)/build
   SRC_DIR=$(pwd)

   mkdir -p $BUILD_DIR
   cd $BUILD_DIR
   conan install $SRC_DIR/config --build=missing
   cmake -S $SRC_DIR -B $BUILD_DIR -DCMAKE_TOOLCHAIN_FILE=conan_paths.cmake -DKATANA_LANG_BINDINGS=python
   make

Python
======

To use the Python libraries from the build directory, use
``$BUILD_DIR/python_env.sh``. You can either use this script as a launcher,

.. code-block:: bash

   $BUILD_DIR/python_env.sh python

or source it into your shell,

.. code-block:: bash

   . $BUILD_DIR/python_env.sh


Specifying and Resolving C++ Dependencies
=========================================

The above instructions should work if you have installed the C++ library
dependencies in ``scripts/setup_dev_ubuntu.sh`` (e.g., llvm-dev, arrow) in their
standard system locations (typically ``/usr/lib`` or ``/usr/local/lib``). If you
need to tell ``cmake`` about additional library locations, you can use the CMake
option ``CMAKE_PREFIX_PATH``, as in:

.. code-block:: bash

   cmake -DCMAKE_TOOLCHAIN_FILE=conan_paths.cmake \
     -DCMAKE_PREFIX_PATH=<path/to/cmakefiles/for/library>;<another/path> ..

As a sidenote, CMake toolchain file is simply a method for initially defining
``CMAKE_PREFIX_PATH`` and other CMake options. You can verify this by looking at
the contents of ``conan_paths.cmake``.

A common issue is that you have multiple versions of the same dependency,
located in different directories, and CMake picks the wrong version.

The process by which CMake finds packages is involved, and the
`CMake documentation <https://cmake.org/cmake/help/latest/command/find_package.html#search-procedure>`_
contains all the gory details. One implication, though, is that CMake adds
directories in your path to its set of search locations.

Thus, if the LLVM C++ compiler (clang++) is in your path, CMake will attempt to
use the LLVM support libraries (e.g., libLLVMSupport.a, libclang.so) associated
with your compiler installation by default, even though your compiler and the
version of the LLVM support libraries you use are not strictly related to each
other.

You can work around this by putting the location of the LLVM support libraries
in ``CMAKE_PREFIX_PATH`` because that takes precedence over locations in your
path. Alternatively, you can indicate the location of the LLVM libraries
directly with ``LLVM_DIR``:

.. code-block:: bash

   cmake -DCMAKE_TOOLCHAIN_FILE=conan_paths.cmake \
     -DLLVM_DIR="$(llvm-config-X --cmakedir)" ..

Adding New E(x)ternal Dependencies
==================================

Adding new dependencies should generally be avoided since it makes it more
likely that satisfying local development requirements, conda build requirements,
production library requirements, etc. will become impossible. If you do choose
to require a new 3rd party library for a good reason you should:

0. Choose a version of the library that is available both in `conda-forge
   <https://anaconda.org/conda-forge/repo>`_ and in `ConanCenter
   <https://conan.io/center/>`_. If it is not available in both places, Ubuntu
   package managers like `apt` or `snap` can work but adding it will be
   different (and you should consider picking another library since this puts
   an extra burden on developers).

1. Add the dependency to the ``config/conanfile.py`` in the style of the
   dependencies that are already there.

2. Add the dependency to the ``conda_recipe/meta.yaml`` in the style of what's
   there. There are two sections; `host` and `run`. Any runtime dependencies
   need to be added to both sections. But dependencies which are totally
   compiled into Katana (i.e., they are not exposed in our API and don't
   require a shared library at run time), can be in `host` only.

3. It is possible that you may have to modify the
   ``cmake/KatanaConfig.cmake.in`` as well so `cmake` will find your dependency
   during the Conda build (again the best advice is to look at how other
   dependencies handle this). This should only be necessary if the new
   dependency is a runtime or user-code dependency. For instance, this should
   not be necessary for header-only libraries that are not used in public
   headers.

If you do end up choosing a library that is not in conda-forge and ConanCenter
(really?) make sure to update the dependency list in ``README.md``, and make
sure the script for setting up a dev environment,
``scripts/setup_dev_ubuntu.sh``, is updated as well. There will likely also be
changes to the CI scripts that are needed.

You should be particularly weary of libraries that are not in conda-forge. If
absolutely necessary, discuss it with the current Conda package maintainer
(currently @arthurp). Not handling them correctly there will totally break the
Conda packages.

Building in Docker
==================

Instead of setting up a development environment explicitly you can build Katana
in docker.

.. code-block:: bash

   scripts/build_in_container.py -B $BUILD_DIR --type conda

where ``$BUILD_DIR`` is a path at which to place the resulting build directory.
Build types other than `conda` may be supported in the future.
You can also pass build targets to the command.

For example,

.. code-block:: bash

   scripts/build_in_container.py -B ~/katana-build --type conda docs

will build the documentation (C++ and Python). The documentation will be in
`~/katana-build/docs/*_python`.