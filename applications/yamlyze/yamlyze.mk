NAME := yamlyze

$(NAME)_SOURCES := yamlyze.cpp

$(NAME)_REQUIRED_COMPONENTS := clang-dev \
                               pugixml \
                               yaml-cpp \
                               json \
                               cxxopts

$(NAME)_gcc_compiler_GLOBAL_CXXFLAGS := -std=c++17 -g -fno-rtti
$(NAME)_gcc_compiler_GLOBAL_LDFLAGS := -lclangTooling -lclangFrontend -lclangParse -lclangSema -lclangAnalysis -lclangAST -lclangBasic -lclangDriver -lclangSerialization -lclangLex -lclangEdit -lLLVM

$(NAME)_msvc_compiler_GLOBAL_CXXFLAGS :=
$(NAME)_msvc_compiler_GLOBAL_LDFLAGS  :=