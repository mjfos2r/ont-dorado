# Dorado

## Quickstart

```
$ git clone git@git.oxfordnanolabs.local:machine-learning/dorado.git
$ cd dorado
$ cmake -S . -B cmake-build
$ cmake --build cmake-build --config Release -- -j
```

## Running

```
./dorado dna_r9.4.1_e8_hac@v3.3 fast5_pass/ > calls.fastq
```

## Run unit tests

```
./unit_test/dorado_tests
```

## Setup notes

Note: Dorado requires CUDA 11.3 on linux platforms. If the system you are running on does not have CUDA 11.3 installed, and you do not have sudo privileges, you can install locally from a run file as follows:

```
wget https://developer.download.nvidia.com/compute/cuda/11.3.0/local_installers/cuda_11.3.0_465.19.01_linux.run
sh cuda_11.3.0_465.19.01_linux.run --silent --toolkit --toolkitpath=${PWD}/cuda11.3
```

In this case, cmake should be invoked with `CUDA_TOOLKIT_ROOT_DIR` in order to tell the build process where to find CUDA:

```
cmake -DCUDA_TOOLKIT_ROOT_DIR=~/dorado_deps/cuda11.3 -S . -B cmake-build 
```

Note that a suitable NVIDIA driver will be required in order to run dorado. Also note that the downloaded version of CUDA 11.3 should appear in the path before any other installed version, so that CMake selects the correct version of nvcc. All other dependencies will be fetched automatically by the cmake build process.

If libtorch is already downloaded on the host system and you do not wish the build process to re-download it, you can specify `DORADO_LIBTORCH_DIR` to cmake, in order to specify where the build process should locate it.  For example:

```
cmake -DDORADO_LIBTORCH_DIR=/usr/local/libtorch -S . -B cmake-build 
```
