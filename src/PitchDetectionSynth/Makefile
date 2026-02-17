# Project Name
TARGET = pitch_detection_synth

# C++ 20 required for Q DSP library
CPP_STANDARD = -std=c++20

# Use DaisySP LGPL modules
USE_DAISYSP_LGPL = 1

# Sources and Hothouse header files
CPP_SOURCES = pitch_detection_synth.cpp ../hothouse.cpp
C_INCLUDES = -I..

# Q DSP library (header-only, from buzzbox_octa_squawker)
C_INCLUDES += -I../buzzbox_octa_squawker/lib/q/q_lib/include

# Infra library (Q dependency)
C_INCLUDES += -I../buzzbox_octa_squawker/lib/infra/include

# Library Locations
LIBDAISY_DIR = ../../libDaisy
DAISYSP_DIR = ../../DaisySP

# Core location, and generic Makefile.
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

# Global helpers
include ../Makefile
