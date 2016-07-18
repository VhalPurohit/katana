#!/bin/sh

INPUT=$(cd $(dirname "$1") && pwd -P)/$(basename "$1")
if [ -n "$2" ]; then
  if ! [ -d "$2" ]; then
    mkdir $2
  fi
  OUTPUT_DIR="$( cd $2 && pwd )"
else
  OUTPUT_DIR="$( cd "$(dirname "$0" )" && pwd )"
fi

OS=$(lsb_release -si)

if [ $OS = "Scientific" ]; then
  if [ -z "$ABELIAN_LLVM_BUILD" ]; then
    ABELIAN_LLVM_BUILD=/net/faraday/workspace/ggill/source_build/llvm_build2
  fi
  if [ -z "$ABELIAN_GALOIS_ROOT" ]; then
    ABELIAN_GALOIS_ROOT=/net/faraday/workspace/ggill/Dist_latest/dist_hetero_new
  fi
  if [ -z "$ABELIAN_GALOIS_BUILD" ]; then
    ABELIAN_GALOIS_BUILD=/net/faraday/workspace/ggill/Dist_latest/build_dist_hetero/release_new_clang/
  fi
  if [ -z "$ABELIAN_NON_HETEROGENEOUS" ]; then
    if [ -z "$ABELIAN_GGC_ROOT" ]; then
      ABELIAN_GGC_ROOT=/net/velocity/workspace/SourceCode/ggc
    fi
  fi
  MPI_INCLUDE=/opt/apps/ossw/libraries/mpich2/mpich2-3.1.3/sl6/gcc-4.8/include
  TBB_INCLUDE=/opt/apps/ossw/libraries/tbb/tbb-4.0/sl6/gcc-4.8/include
  BOOST_INCLUDE=/opt/apps/ossw/libraries/boost/boost-1.58.0/sl6/gcc-4.8/include
elif [ $OS = "CentOS" ]; then
  if [ -z "$ABELIAN_LLVM_BUILD" ]; then
    ABELIAN_LLVM_BUILD=/net/velocity/workspace/SourceCode/llvm/build
  fi
  if [ -z "$ABELIAN_GALOIS_ROOT" ]; then
    ABELIAN_GALOIS_ROOT=/net/velocity/workspace/SourceCode/GaloisCpp
  fi
  if [ -z "$ABELIAN_GALOIS_BUILD" ]; then
    ABELIAN_GALOIS_BUILD=/net/velocity/workspace/SourceCode/GaloisCpp/build/atc1.2-debug
  fi
  if [ -z "$ABELIAN_NON_HETEROGENEOUS" ]; then
    if [ -z "$ABELIAN_GGC_ROOT" ]; then
      ABELIAN_GGC_ROOT=/net/velocity/workspace/SourceCode/ggc
    fi
  fi
  MPI_INCLUDE=/opt/apps/ossw/libraries/mpich2/mpich2-1.5/c7/clang-system/include
  TBB_INCLUDE=/net/faraday/workspace/local/modules/tbb-4.2/include
  BOOST_INCLUDE=/opt/apps/ossw/libraries/boost/boost-1.58.0/c7/clang-system/include
fi

echo "Using LLVM build:" $ABELIAN_LLVM_BUILD
echo "Using Galois:" $ABELIAN_GALOIS_ROOT
if [ -z "$ABELIAN_NON_HETEROGENEOUS" ]; then
  echo "Using GGC:" $ABELIAN_GGC_ROOT
fi

CXX_DEFINES="-DGALOIS_COPYRIGHT_YEAR=2015 -DGALOIS_USE_EXP -DGALOIS_VERSION=2.3.0 -DGALOIS_VERSION_MAJOR=2 -DGALOIS_VERSION_MINOR=3 -DGALOIS_VERSION_PATCH=0 -D__STDC_LIMIT_MACROS"
CXX_FLAGS="-g -Wall -gcc-toolchain /net/faraday/workspace/local/modules/gcc-4.9/bin/.. -fcolor-diagnostics -O3 -DNDEBUG -I$ABELIAN_GALOIS_ROOT/libexp/include -I$MPI_INCLUDE -I$TBB_INCLUDE -I$BOOST_INCLUDE -I$ABELIAN_GALOIS_ROOT/lonestar/include -I$ABELIAN_GALOIS_ROOT/libruntime/include -I$ABELIAN_GALOIS_ROOT/libnet/include -I$ABELIAN_GALOIS_ROOT/libsubstrate/include -I$ABELIAN_GALOIS_ROOT/libllvm/include -I$ABELIAN_GALOIS_BUILD/libllvm/include -I$ABELIAN_GALOIS_ROOT/libgraphs/include -std=gnu++11"
if [ -z "$ABELIAN_NON_HETEROGENEOUS" ]; then
  GGC_FLAGS="--cuda-worklist basic --cuda-graph basic"
  if [ -f "$OUTPUT_DIR/GGCFLAGS" ]; then
    GGC_FLAGS+=$(head -n 1 "$OUTPUT_DIR/GGCFLAGS")
  fi
  echo "Using GGC FLAGS:" $GGC_FLAGS
fi

CXX=$ABELIAN_LLVM_BUILD/bin/clang++
GPREPROCESS_CXX="$CXX -Xclang -load -Xclang $ABELIAN_LLVM_BUILD/lib/GaloisFunctionsPreProcess.so -Xclang -plugin -Xclang galois-preProcess"
GANALYSIS_CXX="$CXX -Xclang -load -Xclang $ABELIAN_LLVM_BUILD/lib/GaloisFunctionsAnalysis.so -Xclang -plugin -Xclang galois-analysis"
GFUNCS_CXX="$CXX -Xclang -load -Xclang $ABELIAN_LLVM_BUILD/lib/GaloisFunctions.so -Xclang -plugin -Xclang galois-fns"
if [ -z "$ABELIAN_NON_HETEROGENEOUS" ]; then
  IRGL_CXX="$CXX -Xclang -load -Xclang $ABELIAN_LLVM_BUILD/lib/GaloisFunctions.so -Xclang -plugin -Xclang irgl"
  GGC="$ABELIAN_GGC_ROOT/src/ggc"
fi

log=.log

cd $OUTPUT_DIR

echo "Cleaning generated files"
rm -f $log gen.cpp gen_cuda.py gen_cuda.cu gen_cuda.cuh gen_cuda.h
cp $INPUT gen.cpp

echo "Preprocessing global variables"
$GPREPROCESS_CXX $CXX_DEFINES $CXX_FLAGS -o .temp.o -c gen.cpp &>$log

echo "Generating analysis information"
$GANALYSIS_CXX $CXX_DEFINES $CXX_FLAGS -o .temp.o -c gen.cpp >>$log 2>&1
echo "Generating communication code"
$GFUNCS_CXX $CXX_DEFINES $CXX_FLAGS -o .temp.o -c gen.cpp >>$log 2>&1

if [ -z "$ABELIAN_NON_HETEROGENEOUS" ]; then
  echo "Generating IrGL code"
  $IRGL_CXX $CXX_DEFINES $CXX_FLAGS -o .temp.o -c gen.cpp >>$log 2>&1
  echo "Generating CUDA code from IrGL"
  $GGC $GGC_FLAGS -o gen_cuda.cu gen_cuda.py >>$log 2>&1
fi

if [ -z "$ABELIAN_NON_HETEROGENEOUS" ]; then
  echo "Generated files in $OUTPUT_DIR: gen.cpp gen_cuda.py gen_cuda.h gen_cuda.cuh gen_cuda.cu" 
else
  echo "Generated files in $OUTPUT_DIR: gen.cpp" 
fi

rm -f Entry-*.dot

