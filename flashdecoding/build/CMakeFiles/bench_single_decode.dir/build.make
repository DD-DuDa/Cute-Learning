# CMAKE generated file: DO NOT EDIT!
# Generated by "Unix Makefiles" Generator, CMake Version 3.29

# Delete rule output on recipe failure.
.DELETE_ON_ERROR:

#=============================================================================
# Special targets provided by cmake.

# Disable implicit rules so canonical targets will work.
.SUFFIXES:

# Disable VCS-based implicit rules.
% : %,v

# Disable VCS-based implicit rules.
% : RCS/%

# Disable VCS-based implicit rules.
% : RCS/%,v

# Disable VCS-based implicit rules.
% : SCCS/s.%

# Disable VCS-based implicit rules.
% : s.%

.SUFFIXES: .hpux_make_needs_suffix_list

# Command-line flag to silence nested $(MAKE).
$(VERBOSE)MAKESILENT = -s

#Suppress display of executed commands.
$(VERBOSE).SILENT:

# A target that is always out of date.
cmake_force:
.PHONY : cmake_force

#=============================================================================
# Set environment variables for the build.

# The shell in which to execute make rules.
SHELL = /bin/sh

# The CMake executable.
CMAKE_COMMAND = /usr/local/bin/cmake

# The command to remove a file.
RM = /usr/local/bin/cmake -E rm -f

# Escaping for special characters.
EQUALS = =

# The top-level source directory on which CMake was run.
CMAKE_SOURCE_DIR = /home/zhichen/dayou/Cute-Learning/flashdecoding

# The top-level build directory on which CMake was run.
CMAKE_BINARY_DIR = /home/zhichen/dayou/Cute-Learning/flashdecoding/build

# Include any dependencies generated for this target.
include CMakeFiles/bench_single_decode.dir/depend.make
# Include any dependencies generated by the compiler for this target.
include CMakeFiles/bench_single_decode.dir/compiler_depend.make

# Include the progress variables for this target.
include CMakeFiles/bench_single_decode.dir/progress.make

# Include the compile flags for this target's objects.
include CMakeFiles/bench_single_decode.dir/flags.make

CMakeFiles/bench_single_decode.dir/src/bench_single_decode.cu.o: CMakeFiles/bench_single_decode.dir/flags.make
CMakeFiles/bench_single_decode.dir/src/bench_single_decode.cu.o: CMakeFiles/bench_single_decode.dir/includes_CUDA.rsp
CMakeFiles/bench_single_decode.dir/src/bench_single_decode.cu.o: /home/zhichen/dayou/Cute-Learning/flashdecoding/src/bench_single_decode.cu
CMakeFiles/bench_single_decode.dir/src/bench_single_decode.cu.o: CMakeFiles/bench_single_decode.dir/compiler_depend.ts
	@$(CMAKE_COMMAND) -E cmake_echo_color "--switch=$(COLOR)" --green --progress-dir=/home/zhichen/dayou/Cute-Learning/flashdecoding/build/CMakeFiles --progress-num=$(CMAKE_PROGRESS_1) "Building CUDA object CMakeFiles/bench_single_decode.dir/src/bench_single_decode.cu.o"
	/usr/local/cuda-12.4/bin/nvcc -forward-unknown-to-host-compiler $(CUDA_DEFINES) $(CUDA_INCLUDES) $(CUDA_FLAGS) -MD -MT CMakeFiles/bench_single_decode.dir/src/bench_single_decode.cu.o -MF CMakeFiles/bench_single_decode.dir/src/bench_single_decode.cu.o.d -x cu -c /home/zhichen/dayou/Cute-Learning/flashdecoding/src/bench_single_decode.cu -o CMakeFiles/bench_single_decode.dir/src/bench_single_decode.cu.o

CMakeFiles/bench_single_decode.dir/src/bench_single_decode.cu.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color "--switch=$(COLOR)" --green "Preprocessing CUDA source to CMakeFiles/bench_single_decode.dir/src/bench_single_decode.cu.i"
	$(CMAKE_COMMAND) -E cmake_unimplemented_variable CMAKE_CUDA_CREATE_PREPROCESSED_SOURCE

CMakeFiles/bench_single_decode.dir/src/bench_single_decode.cu.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color "--switch=$(COLOR)" --green "Compiling CUDA source to assembly CMakeFiles/bench_single_decode.dir/src/bench_single_decode.cu.s"
	$(CMAKE_COMMAND) -E cmake_unimplemented_variable CMAKE_CUDA_CREATE_ASSEMBLY_SOURCE

CMakeFiles/bench_single_decode.dir/src/flash_fwd_hdim128_fp16_sm80.cu.o: CMakeFiles/bench_single_decode.dir/flags.make
CMakeFiles/bench_single_decode.dir/src/flash_fwd_hdim128_fp16_sm80.cu.o: CMakeFiles/bench_single_decode.dir/includes_CUDA.rsp
CMakeFiles/bench_single_decode.dir/src/flash_fwd_hdim128_fp16_sm80.cu.o: /home/zhichen/dayou/Cute-Learning/flashdecoding/src/flash_fwd_hdim128_fp16_sm80.cu
CMakeFiles/bench_single_decode.dir/src/flash_fwd_hdim128_fp16_sm80.cu.o: CMakeFiles/bench_single_decode.dir/compiler_depend.ts
	@$(CMAKE_COMMAND) -E cmake_echo_color "--switch=$(COLOR)" --green --progress-dir=/home/zhichen/dayou/Cute-Learning/flashdecoding/build/CMakeFiles --progress-num=$(CMAKE_PROGRESS_2) "Building CUDA object CMakeFiles/bench_single_decode.dir/src/flash_fwd_hdim128_fp16_sm80.cu.o"
	/usr/local/cuda-12.4/bin/nvcc -forward-unknown-to-host-compiler $(CUDA_DEFINES) $(CUDA_INCLUDES) $(CUDA_FLAGS) -MD -MT CMakeFiles/bench_single_decode.dir/src/flash_fwd_hdim128_fp16_sm80.cu.o -MF CMakeFiles/bench_single_decode.dir/src/flash_fwd_hdim128_fp16_sm80.cu.o.d -x cu -c /home/zhichen/dayou/Cute-Learning/flashdecoding/src/flash_fwd_hdim128_fp16_sm80.cu -o CMakeFiles/bench_single_decode.dir/src/flash_fwd_hdim128_fp16_sm80.cu.o

CMakeFiles/bench_single_decode.dir/src/flash_fwd_hdim128_fp16_sm80.cu.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color "--switch=$(COLOR)" --green "Preprocessing CUDA source to CMakeFiles/bench_single_decode.dir/src/flash_fwd_hdim128_fp16_sm80.cu.i"
	$(CMAKE_COMMAND) -E cmake_unimplemented_variable CMAKE_CUDA_CREATE_PREPROCESSED_SOURCE

CMakeFiles/bench_single_decode.dir/src/flash_fwd_hdim128_fp16_sm80.cu.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color "--switch=$(COLOR)" --green "Compiling CUDA source to assembly CMakeFiles/bench_single_decode.dir/src/flash_fwd_hdim128_fp16_sm80.cu.s"
	$(CMAKE_COMMAND) -E cmake_unimplemented_variable CMAKE_CUDA_CREATE_ASSEMBLY_SOURCE

CMakeFiles/bench_single_decode.dir/src/flash_fwd_split_hdim128_fp16_sm80.cu.o: CMakeFiles/bench_single_decode.dir/flags.make
CMakeFiles/bench_single_decode.dir/src/flash_fwd_split_hdim128_fp16_sm80.cu.o: CMakeFiles/bench_single_decode.dir/includes_CUDA.rsp
CMakeFiles/bench_single_decode.dir/src/flash_fwd_split_hdim128_fp16_sm80.cu.o: /home/zhichen/dayou/Cute-Learning/flashdecoding/src/flash_fwd_split_hdim128_fp16_sm80.cu
CMakeFiles/bench_single_decode.dir/src/flash_fwd_split_hdim128_fp16_sm80.cu.o: CMakeFiles/bench_single_decode.dir/compiler_depend.ts
	@$(CMAKE_COMMAND) -E cmake_echo_color "--switch=$(COLOR)" --green --progress-dir=/home/zhichen/dayou/Cute-Learning/flashdecoding/build/CMakeFiles --progress-num=$(CMAKE_PROGRESS_3) "Building CUDA object CMakeFiles/bench_single_decode.dir/src/flash_fwd_split_hdim128_fp16_sm80.cu.o"
	/usr/local/cuda-12.4/bin/nvcc -forward-unknown-to-host-compiler $(CUDA_DEFINES) $(CUDA_INCLUDES) $(CUDA_FLAGS) -MD -MT CMakeFiles/bench_single_decode.dir/src/flash_fwd_split_hdim128_fp16_sm80.cu.o -MF CMakeFiles/bench_single_decode.dir/src/flash_fwd_split_hdim128_fp16_sm80.cu.o.d -x cu -c /home/zhichen/dayou/Cute-Learning/flashdecoding/src/flash_fwd_split_hdim128_fp16_sm80.cu -o CMakeFiles/bench_single_decode.dir/src/flash_fwd_split_hdim128_fp16_sm80.cu.o

CMakeFiles/bench_single_decode.dir/src/flash_fwd_split_hdim128_fp16_sm80.cu.i: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color "--switch=$(COLOR)" --green "Preprocessing CUDA source to CMakeFiles/bench_single_decode.dir/src/flash_fwd_split_hdim128_fp16_sm80.cu.i"
	$(CMAKE_COMMAND) -E cmake_unimplemented_variable CMAKE_CUDA_CREATE_PREPROCESSED_SOURCE

CMakeFiles/bench_single_decode.dir/src/flash_fwd_split_hdim128_fp16_sm80.cu.s: cmake_force
	@$(CMAKE_COMMAND) -E cmake_echo_color "--switch=$(COLOR)" --green "Compiling CUDA source to assembly CMakeFiles/bench_single_decode.dir/src/flash_fwd_split_hdim128_fp16_sm80.cu.s"
	$(CMAKE_COMMAND) -E cmake_unimplemented_variable CMAKE_CUDA_CREATE_ASSEMBLY_SOURCE

# Object files for target bench_single_decode
bench_single_decode_OBJECTS = \
"CMakeFiles/bench_single_decode.dir/src/bench_single_decode.cu.o" \
"CMakeFiles/bench_single_decode.dir/src/flash_fwd_hdim128_fp16_sm80.cu.o" \
"CMakeFiles/bench_single_decode.dir/src/flash_fwd_split_hdim128_fp16_sm80.cu.o"

# External object files for target bench_single_decode
bench_single_decode_EXTERNAL_OBJECTS =

bench_single_decode: CMakeFiles/bench_single_decode.dir/src/bench_single_decode.cu.o
bench_single_decode: CMakeFiles/bench_single_decode.dir/src/flash_fwd_hdim128_fp16_sm80.cu.o
bench_single_decode: CMakeFiles/bench_single_decode.dir/src/flash_fwd_split_hdim128_fp16_sm80.cu.o
bench_single_decode: CMakeFiles/bench_single_decode.dir/build.make
bench_single_decode: /home/zhichen/downloads/libtorch/lib/libtorch.so
bench_single_decode: /home/zhichen/downloads/libtorch/lib/libc10.so
bench_single_decode: /home/zhichen/downloads/libtorch/lib/libkineto.a
bench_single_decode: /usr/local/cuda-12.4/lib64/libnvrtc.so
bench_single_decode: /home/zhichen/downloads/libtorch/lib/libc10_cuda.so
bench_single_decode: /home/zhichen/downloads/libtorch/lib/libc10_cuda.so
bench_single_decode: /home/zhichen/downloads/libtorch/lib/libc10.so
bench_single_decode: /usr/local/cuda-12.4/lib64/libcudart.so
bench_single_decode: /usr/local/cuda-12.4/lib64/libnvToolsExt.so
bench_single_decode: CMakeFiles/bench_single_decode.dir/linkLibs.rsp
bench_single_decode: CMakeFiles/bench_single_decode.dir/objects1.rsp
bench_single_decode: CMakeFiles/bench_single_decode.dir/link.txt
	@$(CMAKE_COMMAND) -E cmake_echo_color "--switch=$(COLOR)" --green --bold --progress-dir=/home/zhichen/dayou/Cute-Learning/flashdecoding/build/CMakeFiles --progress-num=$(CMAKE_PROGRESS_4) "Linking CUDA executable bench_single_decode"
	$(CMAKE_COMMAND) -E cmake_link_script CMakeFiles/bench_single_decode.dir/link.txt --verbose=$(VERBOSE)

# Rule to build all files generated by this target.
CMakeFiles/bench_single_decode.dir/build: bench_single_decode
.PHONY : CMakeFiles/bench_single_decode.dir/build

CMakeFiles/bench_single_decode.dir/clean:
	$(CMAKE_COMMAND) -P CMakeFiles/bench_single_decode.dir/cmake_clean.cmake
.PHONY : CMakeFiles/bench_single_decode.dir/clean

CMakeFiles/bench_single_decode.dir/depend:
	cd /home/zhichen/dayou/Cute-Learning/flashdecoding/build && $(CMAKE_COMMAND) -E cmake_depends "Unix Makefiles" /home/zhichen/dayou/Cute-Learning/flashdecoding /home/zhichen/dayou/Cute-Learning/flashdecoding /home/zhichen/dayou/Cute-Learning/flashdecoding/build /home/zhichen/dayou/Cute-Learning/flashdecoding/build /home/zhichen/dayou/Cute-Learning/flashdecoding/build/CMakeFiles/bench_single_decode.dir/DependInfo.cmake "--color=$(COLOR)"
.PHONY : CMakeFiles/bench_single_decode.dir/depend

