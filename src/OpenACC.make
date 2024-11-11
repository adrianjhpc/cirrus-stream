COMPILER=NVIDIA
TARGET=VOLTA
#TARGET=SNB
ifndef COMPILER
define compiler_help
Set COMPILER to ensure correct flags are set.
Available compilers are:
  NVIDIA GNU
endef
$(info $(compiler_help))
endif

COMPILER_ = $(CXX)
COMPILER_NVIDIA = nvc++
COMPILER_CRAY = g++

FLAGS_ = -O3 -std=c++11

FLAGS_NVIDIA = -std=c++11 -O3 
ifeq ($(COMPILER), NVIDIA)
define target_help
Set a TARGET to ensure NVIDIA targets the correct offload device.
Available targets are:
  VOLTA SNB
endef
ifndef TARGET
$(error $(target_help))
endif
TARGET_FLAGS_VOLTA  = -acc=gpu -gpu=cc70
TARGET_FLAGS_SNB = -acc=multicore -tp=sandybridge
ifeq ($(TARGET_FLAGS_$(TARGET)),)
$(error $(target_help))
endif

FLAGS_NVIDIA += $(TARGET_FLAGS_$(TARGET))

endif

FLAGS_CRAY = -hstd=c++11
CXXFLAGS = $(FLAGS_$(COMPILER))

acc-stream: main.cpp ACCStream.cpp
	$(COMPILER_$(COMPILER)) $(CXXFLAGS) -DACC $^ $(EXTRA_FLAGS) -o $@

.PHONY: clean
clean:
	rm -f acc-stream main.o ACCStream.o
