DEBUG     := 0
WITH_DYNAREC := 1

DYNAFLAGS :=
INCFLAGS  := -I.
COREFLAGS :=
CPUFLAGS  :=

PLATOFRM_EXT :=

UNAME=$(shell uname -a)

# Cross compile ?

ifeq (,$(TARGET_ARCH))
	TARGET_ARCH = $(shell uname -m)
endif

ifneq (,$(findstring 64,$(TARGET_ARCH)))
   TARGET_ARCH := x86_64
else ifneq (,$(findstring 86,$(TARGET_ARCH)))
   TARGET_ARCH := x86
endif

ifeq ($(platform),)
	platform = unix
	ifeq ($(UNAME),)
		platform = win
	else ifneq ($(findstring MINGW,$(UNAME)),)
		platform = win
	else ifneq ($(findstring Darwin,$(UNAME)),)
		platform = osx
		LDFLAGS += -lm
	else ifneq ($(findstring win,$(UNAME)),)
		platform = win
	endif
endif

ifeq ($(platform), unix)
LDFLAGS += -lm
endif

# Dirs
CORE_DIR			= ..
FFMPEGDIR		= $(CORE_DIR)/ffmpeg
LIBRETRODIR		= $(CORE_DIR)/libretro
COREDIR			= $(CORE_DIR)/Core
COMMONDIR		= $(CORE_DIR)/Common
GPUCOMMONDIR	= $(CORE_DIR)/GPU/Common
GPUDIR			= $(CORE_DIR)/GPU
NATIVEDIR		= $(CORE_DIR)/native
EXTDIR			= $(CORE_DIR)/ext

TARGET_NAME := ppsspp

FFMPEGINCFLAGS :=
FFMPEGLIBDIR :=
FFMPEGLIBS :=

# Unix
ifneq (,$(findstring unix,$(platform)))
	TARGET := $(TARGET_NAME)_libretro.so
	LDFLAGS += -shared -Wl,--version-script=link.T -Wl,--no-undefined

	FFMPEGINCFLAGS += -I$(FFMPEGDIR)/linux/$(TARGET_ARCH)/include
	FFMPEGLIBDIR := $(FFMPEGDIR)/linux/$(TARGET_ARCH)/lib
	FFMPEGLDFLAGS += -L$(FFMPEGLIBDIR) -lavformat -lavcodec -lavutil -lswresample -lswscale
	fpic = -fPIC
	ifneq (,$(findstring gles,$(platform)))
		GLES = 1
		GL_LIB := -lGLESv2
	else
		GL_LIB := -lGL
	endif
	PLATFORM_EXT = unix
	LDFLAGS += -lrt -ldl

# Raspberry Pi
else ifneq (,$(findstring rpi,$(platform)))
	TARGET := $(TARGET_NAME)_libretro.so
	LDFLAGS += -shared -Wl,--version-script=link.T
	fpic = -fPIC
	GLES = 1
	GL_LIB := -lGLESv2
	INCFLAGS += -I/opt/vc/include
	CPUFLAGS += -DARMv5_ONLY
	PLATFORM_EXT = unix
	TARGET_ARCH = arm
	LDFLAGS += -lrt -ldl

# ARM64
else ifneq (,$(findstring arm64,$(platform)))
	TARGET := $(TARGET_NAME)_libretro.so
	TARGET_ARCH = arm64
	fpic := -fPIC
	LDFLAGS += -shared -Wl,--version-script=link.T -Wl,--no-undefined
	FFMPEGINCFLAGS += -I$(FFMPEGDIR)/linux/aarch64/include
	FFMPEGLIBDIR := $(FFMPEGDIR)/linux/aarch64/lib
	FFMPEGLDFLAGS += -L$(FFMPEGLIBDIR) -lavformat -lavcodec -lavutil -lswresample -lswscale
	INCFLAGS += -I.
	HAVE_NEON=1
	ifneq (,$(findstring gles,$(platform)))
		GLES := 1
		GL_LIB := -lGLESv2 -lEGL
		LDFLAGS += -lGLESv2 -lEGL
	else
		GL_LIB := -lGL
	endif
	CPUFLAGS += -D__arm64__ -DARM64_ASM -D__NEON_OPT
	LDFLAGS += -lrt -ldl
	PLATFORM_EXT = unix

# i.MX6
else ifneq (,$(findstring imx6,$(platform)))
	TARGET := $(TARGET_NAME)_libretro.so
	LDFLAGS += -shared -Wl,--version-script=link.T
	fpic = -fPIC
	GLES = 1
	GL_LIB := -lGLESv2
	CPUFLAGS +=
	PLATFORM_EXT = unix
	TARGET_ARCH = arm
	HAVE_NEON=1
	FFMPEGINCFLAGS += -I$(FFMPEGDIR)/linux/$(TARGET_ARCH)/include
	FFMPEGLIBDIR := $(FFMPEGDIR)/linux/$(TARGET_ARCH)/lib
	FFMPEGLDFLAGS += -L$(FFMPEGLIBDIR) -lavformat -lavcodec -lavutil -lswresample -lswscale
	LDFLAGS += -lrt -ldl

# OS X
else ifneq (,$(findstring osx,$(platform)))
	TARGET := $(TARGET_NAME)_libretro.dylib
   MINVERSION :=
	LDFLAGS += -dynamiclib
	OSXVER = `sw_vers -productVersion | cut -d. -f 2`
	OSX_LT_MAVERICKS = `(( $(OSXVER) <= 9 )) && echo "YES"`
	ifeq ($(OSX_LT_MAVERICKS),"YES")
      MINVERSION += -mmacosx-version-min=10.5
	endif
	LDFLAGS += $(MINVERSION)
   LDFLAGS += -stdlib=libc++
	fpic = -fPIC

	FFMPEGINCFLAGS += -I$(FFMPEGDIR)/macosx/universal/include
	FFMPEGLIBDIR := $(FFMPEGDIR)/macosx/universal/lib
	FFMPEGLDFLAGS += -liconv -L$(FFMPEGLIBDIR) -lavformat -lavcodec -lavutil -lswresample -lswscale
	PLATCFLAGS += -D__MACOSX__
	GL_LIB := -framework OpenGL
	PLATFORM_EXT = darwin

   ifeq ($(CROSS_COMPILE),1)
		TARGET_RULE   = -target $(LIBRETRO_APPLE_PLATFORM) -isysroot $(LIBRETRO_APPLE_ISYSROOT)
		PLATCFLAGS   += $(TARGET_RULE)
		LDFLAGS      += $(TARGET_RULE)
   endif

	PLATCFLAGS  += $(ARCHFLAGS)
	LDFLAGS += $(ARCHFLAGS)

# iOS
else ifneq (,$(findstring ios,$(platform)))
	TARGET := $(TARGET_NAME)_libretro_ios.dylib
	LDFLAGS += -dynamiclib -marm
	fpic = -fPIC
	GLES = 1
	GL_LIB := -framework OpenGLES
   MINVERSION :=
	HAVE_NEON = 1

	FFMPEGINCFLAGS += -I$(FFMPEGDIR)/ios/universal/include
	FFMPEGLIBDIR := $(FFMPEGDIR)/ios/universal/lib
	FFMPEGLDFLAGS += -L$(FFMPEGLIBDIR) -lavformat -lavcodec -lavutil -lswresample -lswscale

	ifeq ($(IOSSDK),)
		IOSSDK := $(shell xcodebuild -version -sdk iphoneos Path)
	endif

ifeq ($(platform),ios-arm64)
	CC = clang -arch arm64 -isysroot $(IOSSDK)
	CXX = clang++ -arch arm64 -isysroot $(IOSSDK)
else
	CC = clang -arch armv7 -isysroot $(IOSSDK)
	CXX = clang++ -arch armv7 -isysroot $(IOSSDK)
endif
	OSXVER = `sw_vers -productVersion | cut -c 4`
   ifneq ($(OSXVER),9)
      MINVERSION = -miphoneos-version-min=5.0
   else
      MINVERSION = -miphoneos-version-min=8.0
   endif
	PLATCFLAGS += $(MINVERSION)
	PLATCFLAGS += -DHAVE_POSIX_MEMALIGN
	CPUFLAGS += -DARMv5_ONLY -DARM
	PLATFORM_EXT = darwin
	TARGET_ARCH = arm

else ifeq ($(platform), tvos-arm64)
	TARGET := $(TARGET_NAME)_libretro_tvos.dylib
	LDFLAGS += -dynamiclib -marm
	fpic = -fPIC
	GLES = 1
	GL_LIB := -framework OpenGLES
   MINVERSION :=
	HAVE_NEON = 1

	FFMPEGINCFLAGS += -I$(FFMPEGDIR)/ios/universal/include
	FFMPEGLIBDIR := $(FFMPEGDIR)/ios/universal/lib
	FFMPEGLDFLAGS += -L$(FFMPEGLIBDIR) -lavformat -lavcodec -lavutil -lswresample -lswscale

	ifeq ($(IOSSDK),)
		IOSSDK := $(shell xcodebuild -version -sdk appletvos Path)
	endif

	CC = clang -arch arm64 -isysroot $(IOSSDK)
	CXX = clang++ -arch arm64 -isysroot $(IOSSDK)
	OSXVER = `sw_vers -productVersion | cut -c 4`
	PLATCFLAGS += $(MINVERSION)
	PLATCFLAGS += -DHAVE_POSIX_MEMALIGN
	CPUFLAGS += -DARMv5_ONLY -DARM
	PLATFORM_EXT = darwin
	TARGET_ARCH = arm

# Android
else ifneq (,$(findstring android,$(platform)))
	fpic = -fPIC
	TARGET := $(TARGET_NAME)_libretro_android.so
	LDFLAGS += -shared -Wl,--version-script=link.T -Wl,--no-undefined -Wl,--warn-common
	GL_LIB := -lGLESv2

	CC = arm-linux-androideabi-gcc
	CXX = arm-linux-androideabi-g++
	TARGET_ARCH = arm
	GLES = 1
	PLATCFLAGS += -DANDROID
	CPUCFLAGS  +=
	HAVE_NEON = 1
	CPUFLAGS += -marm -mcpu=cortex-a8 -mfpu=neon -mfloat-abi=softfp -D__arm__ -DARM_ASM -D__NEON_OPT
	CFLAGS += -DANDROID
	LDFLAGS += -llog -lGLESv2 -lEGL

	FFMPEGINCFLAGS += -I$(FFMPEGDIR)/android/armv7/include
	FFMPEGLIBDIR := $(FFMPEGDIR)/android/armv7/lib
	FFMPEGLDFLAGS += -L$(FFMPEGLIBDIR) -lavformat -lavcodec -lavutil -lswresample -lswscale

	PLATFORM_EXT = android

# QNX
else ifeq ($(platform), qnx)
	fpic = -fPIC
	TARGET := $(TARGET_NAME)_libretro_qnx.so
	LDFLAGS += -shared -Wl,--version-script=link.T -Wl,--no-undefined -Wl,--warn-common -Wl,--gc-sections
	# Keep QNX libc last: the GCC driver specs do not add it automatically,
	# and static libstdc++/FFmpeg archives introduce libc references.
	GL_LIB := -lGLESv2 -lEGL -lc

	CC = arm-unknown-nto-qnx6.5.0eabi-gcc
	AS = arm-unknown-nto-qnx6.5.0eabi-gcc
	CXX = arm-unknown-nto-qnx6.5.0eabi-g++
	AR = arm-unknown-nto-qnx6.5.0eabi-ar
	TARGET_ARCH = arm
	GLES = 1
	PLATCFLAGS += -D__BLACKBERRY_QNX__
	PPSSPP_OFFLINE := 1
	HAVE_NEON = 1
	# MHI2Q is a Qualcomm Krait, not a Cortex-A9: wide out-of-order, closest
	# in-tree model is cortex-a15 (GCC 8.5 has no -mtune=krait). Scheduling
	# for an A9 pipeline costs throughput for nothing. PCSX-ReARMed already
	# tunes for a15 here.
	#
	# -O3 was tried here and REVERTED 2026-08-22: the core started dying with
	# SIGSEGV in ldqnx.so.2's __gnu_Unwind_Find_exidx (C++ exception unwinding,
	# faulting near null). Not conclusively pinned on -O3 - the same build also
	# turned on real CPU detection, which enables threads that were previously
	# off - but -O3 is the likeliest to expose latent UB, so it goes first.
	# Match the capabilities now reported by ArmCPUDetect.cpp.  VFPv4 is real
	# on Krait and lets both the C++ core and its ARM helpers use the same ISA
	# that the JIT selects at runtime.  The remaining flags mirror PPSSPP's
	# CMake release policy: old emulator code relies on aliasing/signed wrap,
	# libm calls do not need errno, and preserving r11 as a frame pointer costs
	# a scarce ARM register in the hottest code.
	CPUFLAGS += -B/opt/tools/gas-compat/bin -marm -march=armv7-a -mtune=cortex-a15 -mfpu=neon-vfpv4 -mfloat-abi=softfp -D__arm__ -DARM_ASM -D__NEON_OPT -fno-strict-aliasing -fwrapv -fno-math-errno -fomit-frame-pointer -ffunction-sections -fdata-sections
	COREFLAGS += -D__QNX__ -DPPSSPP_OFFLINE=1
	QNX_C_FORCE_INCLUDE := -include stddef.h
	CFLAGS += $(QNX_C_FORCE_INCLUDE)
	CXXFLAGS += -include bits/os_defines.h -include ../Common/QnxCompat.h
	FFMPEGINCFLAGS += -I$(FFMPEGDIR)/blackberry/armv7/include
	FFMPEGLIBDIR := $(FFMPEGDIR)/blackberry/armv7/lib
	FFMPEGLDFLAGS += -L$(FFMPEGLIBDIR) -lavformat -lavcodec -lavutil -lswresample -lswscale
	# Keep PPSSPP's exception runtime and EHABI unwinder in the core.  The HU's
	# dynamic loader crashes in __gnu_Unwind_Find_exidx when a GCC 8 exception
	# crosses the separately loaded libstdc++.so.6 boundary.  build.sh supplies
	# a sanitized archive until the rebuilt toolchain contains the same fix.
	ifneq ($(QNX_STATIC_CXX_LIBDIR),)
		LDFLAGS += -L$(QNX_STATIC_CXX_LIBDIR)
	endif
	LDFLAGS += -lstdc++fs -lm -static-libstdc++ -static-libgcc

	PLATFORM_EXT = unix

# ARM
else ifneq (,$(findstring armv,$(platform)))
	TARGET := $(TARGET_NAME)_libretro.so
	TARGET_ARCH = armv7
	fpic := -fPIC
	LDFLAGS += -shared -Wl,--version-script=link.T -Wl,--no-undefined
	FFMPEGINCFLAGS += -I$(FFMPEGDIR)/linux/$(TARGET_ARCH)/include
	FFMPEGLIBDIR := $(FFMPEGDIR)/linux/$(TARGET_ARCH)/lib
	FFMPEGLDFLAGS += -L$(FFMPEGLIBDIR) -lavformat -lavcodec -lavutil -lswresample -lswscale
	INCFLAGS += -I.
	ifneq (,$(findstring gles,$(platform)))
		GLES := 1
		GL_LIB := -lGLESv2 -lEGL
		LDFLAGS += -lGLESv2 -lEGL
	else
		GL_LIB := -lGL
	endif
	ifneq (,$(findstring cortexa8,$(platform)))
		CPUFLAGS += -marm -mcpu=cortex-a8
	else ifneq (,$(findstring cortexa9,$(platform)))
		CPUFLAGS += -marm -mcpu=cortex-a9
	endif
		CPUFLAGS += -marm
	ifneq (,$(findstring neon,$(platform)))
		CPUFLAGS += -mfpu=neon -D__NEON_OPT
		HAVE_NEON = 1
	endif
	ifneq (,$(findstring softfloat,$(platform)))
		CPUFLAGS += -mfloat-abi=softfp
	else ifneq (,$(findstring hardfloat,$(platform)))
		CPUFLAGS += -mfloat-abi=hard
	endif
	CPUFLAGS += -D__arm__ -DARM_ASM
	LDFLAGS += -lrt -ldl

# emscripten
else ifeq ($(platform), emscripten)
	TARGET_ARCH = wasm32
	GLES = 1
	TARGET := $(TARGET_NAME)_libretro_emscripten.bc
	PLATCFLAGS += -msimd128 -msse3 -mssse3 -msse4.1 -pthread -D__EMSCRIPTEN__ -DNO_MMAP
	PLATFORM_EXT = emscripten
	STATIC_LINKING = 1

# Windows MSVC all architectures
else ifneq (,$(findstring windows_msvc2019,$(platform)))

	PlatformSuffix = $(subst windows_msvc2019_,,$(platform))
	ifneq (,$(findstring desktop,$(PlatformSuffix)))
		WinPartition = desktop
		MSVCCompileFlags = -DWINAPI_FAMILY=WINAPI_FAMILY_DESKTOP_APP -D_UNICODE -DUNICODE -DWINVER=0x0600 -D_WIN32_WINNT=0x0600
		LDFLAGS += -MANIFEST -NXCOMPAT -DYNAMICBASE -DEBUG -OPT:REF -INCREMENTAL:NO -SUBSYSTEM:WINDOWS -MANIFESTUAC:"level='asInvoker' uiAccess='false'" -OPT:ICF -ERRORREPORT:PROMPT -NOLOGO -TLBID:1
		LIBS += kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib
	else ifneq (,$(findstring uwp,$(PlatformSuffix)))
		WinPartition = uwp
		MSVCCompileFlags = -DWINAPI_FAMILY=WINAPI_FAMILY_APP -DWINDLL -D_UNICODE -DUNICODE -DWRL_NO_DEFAULT_LIB
		LDFLAGS += -APPCONTAINER -NXCOMPAT -DYNAMICBASE -MANIFEST:NO -OPT:REF -SUBSYSTEM:CONSOLE -MANIFESTUAC:NO -OPT:ICF -ERRORREPORT:PROMPT -NOLOGO -TLBID:1 -DEBUG:FULL -WINMD:NO
		LIBS += WindowsApp.lib
	endif

	ifeq ($(DEBUG), 1)
		MSVCCompileFlags += -DEBUG
	endif

	CFLAGS += $(MSVCCompileFlags) -nologo
	CXXFLAGS += $(MSVCCompileFlags) -nologo -EHsc -DARMIPS_USE_STD_FILESYSTEM

	TargetArchMoniker = $(subst $(WinPartition)_,,$(PlatformSuffix))

	CC  = cl.exe
	CXX = cl.exe

	SPACE :=
	SPACE := $(SPACE) $(SPACE)
	BACKSLASH :=
	BACKSLASH := \$(BACKSLASH)
	filter_out1 = $(filter-out $(firstword $1),$1)
	filter_out2 = $(call filter_out1,$(call filter_out1,$1))

	reg_query = $(call filter_out2,$(subst $2,,$(shell reg query "$2" -v "$1" 2>/dev/null)))
	fix_path = $(subst $(SPACE),\ ,$(subst \,/,$1))

	VSCommand := bash VSWhere.sh -latest -property installationPath
	VSInstallPathw := $(shell cmd //c "$(VSCommand)")
	VSInstallPath := $(shell cygpath "$(VSInstallPathw)")

	WindowsSdkDir ?= $(call reg_query,InstallationFolder,HKEY_LOCAL_MACHINE\SOFTWARE\Wow6432Node\Microsoft\Microsoft SDKs\Windows\v10.0)
	WindowsSdkDir ?= $(call reg_query,InstallationFolder,HKEY_CURRENT_USER\SOFTWARE\Wow6432Node\Microsoft\Microsoft SDKs\Windows\v10.0)
	WindowsSdkDir ?= $(call reg_query,InstallationFolder,HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Microsoft SDKs\Windows\v10.0)
	WindowsSdkDir ?= $(call reg_query,InstallationFolder,HKEY_CURRENT_USER\SOFTWARE\Microsoft\Microsoft SDKs\Windows\v10.0)
	WindowsSdkDir := $(WindowsSdkDir)

	WindowsSDKVersion ?= $(firstword $(foreach folder,$(subst $(subst \,/,$(WindowsSdkDir)Include/),,$(wildcard $(call fix_path,$(WindowsSdkDir)Include\*))),$(if $(wildcard $(call fix_path,$(WindowsSdkDir)Include/$(folder)/um/Windows.h)),$(folder),)))$(BACKSLASH)
	WindowsSDKVersion := $(WindowsSDKVersion)

	VsInstallRoot := $(VSInstallPath)

	VcCompilerToolsVer := $(shell cat "$(VsInstallRoot)/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt" | grep -o '[0-9\.]*')
	VcCompilerToolsDir := $(VsInstallRoot)/VC/Tools/MSVC/$(VcCompilerToolsVer)
	VcCompilerLibDir := $(VcCompilerToolsDir)/lib/$(TargetArchMoniker)

	WindowsSDKSharedIncludeDir := $(shell cygpath -w "$(WindowsSdkDir)\Include\$(WindowsSDKVersion)\shared")
	WindowsSDKUCRTIncludeDir := $(shell cygpath -w "$(WindowsSdkDir)\Include\$(WindowsSDKVersion)\ucrt")
	WindowsSDKUMIncludeDir := $(shell cygpath -w "$(WindowsSdkDir)\Include\$(WindowsSDKVersion)\um")
	WindowsSDKWinRTIncludeDir := $(shell cygpath -w "$(WindowsSdkDir)\Include\$(WindowsSDKVersion)\winrt")
	WindowsSDKUCRTLibDir := $(shell cygpath -w "$(WindowsSdkDir)\Lib\$(WindowsSDKVersion)\ucrt\$(TargetArchMoniker)")
	WindowsSDKUMLibDir := $(shell cygpath -w "$(WindowsSdkDir)\Lib\$(WindowsSDKVersion)\um\$(TargetArchMoniker)")

	LIB := $(shell IFS=$$'\n'; cygpath -w "$(VcCompilerLibDir)")
	INCLUDE := $(shell IFS=$$'\n'; cygpath -w "$(VcCompilerToolsDir)/include")

	# For some reason the HostX86 compiler doesn't like compiling for x64
	# ("no such file" opening a shared library), and vice-versa.
	# Work around it for now by using the strictly x86 compiler for x86, and x64 for x64.
	# NOTE: What about ARM?
	ifneq (,$(findstring x64,$(TargetArchMoniker)))
		override TARGET_ARCH = x86_64
		VCCompilerToolsBinDir := $(VcCompilerToolsDir)/bin/HostX64/$(TargetArchMoniker)
	else
		override TARGET_ARCH = x86
		VCCompilerToolsBinDir := $(VcCompilerToolsDir)/bin/HostX86/$(TargetArchMoniker)
	endif

	PATH := $(shell IFS=$$'\n'; cygpath "$(VCCompilerToolsBinDir)"):$(PATH)
	PATH := $(PATH):$(shell IFS=$$'\n'; cygpath "$(VsInstallRoot)/Common7/IDE")

	export INCLUDE := $(INCLUDE);$(WindowsSDKSharedIncludeDir);$(WindowsSDKUCRTIncludeDir);$(WindowsSDKUMIncludeDir);$(WindowsSDKWinRTIncludeDir)
	export LIB := $(LIB);$(WindowsSDKUCRTLibDir);$(WindowsSDKUMLibDir);$(FFMPEGDIR)/Windows/$(TARGET_ARCH)/lib
	TARGET := $(TARGET_NAME)_libretro.dll
	PSS_STYLE :=2
	LDFLAGS += -DLL
	PLATFORM_EXT = win32
	FFMPEGINCFLAGS += -I$(FFMPEGDIR)/Windows/$(TARGET_ARCH)/include
	FFMPEGLIBDIR := $(FFMPEGDIR)/Windows/$(TARGET_ARCH)/lib
	FFMPEGLDFLAGS += -LIBPATH:$(FFMPEGLIBDIR)
	GL_LIB := opengl32.lib
	LDFLAGS += ws2_32.lib user32.lib shell32.lib avcodec.lib avutil.lib swresample.lib swscale.lib avformat.lib advapi32.lib winmm.lib gdi32.lib Iphlpapi.lib

# Windows
else ifneq (,$(findstring win,$(platform)))
	TARGET := $(TARGET_NAME)_libretro.dll
	CFLAGS += -D_UNICODE -DUNICODE
	CXXFLAGS += -fpermissive -Wno-multichar -D_UNICODE -DUNICODE -DARMIPS_USE_STD_FILESYSTEM
	LDFLAGS += -shared -Wl,--no-undefined -static-libgcc -static-libstdc++ -Wl,--version-script=link.T -lwinmm -lgdi32 -lwsock32 -lws2_32
	GL_LIB := -lopengl32
	PLATFORM_EXT = win32
   FFMPEGINCFLAGS += -I$(FFMPEGDIR)/Windows/$(TARGET_ARCH)/include
   FFMPEGLDFLAGS += -lavformat -lavcodec -lavutil -lswresample -lswscale
   fpic = -fPIC
	CC = gcc
	CXX = g++
endif

include Makefile.common

ifeq ($(GLES), 1)
	GLFLAGS += -DGLES -DUSING_GLES2
else
	GLFLAGS += -DHAVE_OPENGL
endif

COREFLAGS += -D__LIBRETRO__ -DPPSSPP -DGLEW_STATIC -DGLEW_NO_GLU
ifneq ($(PPSSPP_OFFLINE),1)
	COREFLAGS += -DWITH_UPNP -DMINIUPNP_STATICLIB
endif
ifneq ($(platform), emscripten)
	# ffmpeg does not work with emscripten
	COREFLAGS += -DUSE_FFMPEG
endif

ifeq ($(DEBUG), 1)
	ifneq (,$(findstring msvc,$(platform)))
		CPUOPTS += -Od -MDd -Zi -FS
		LDFLAGS += -DEBUG
	else
		CPUOPTS += -O0 -g
	endif
	CPUOPTS += -D_DEBUG
else ifeq ($(platform), emscripten)
	CPUOPTS += -O3 -DNDEBUG
else
	CPUOPTS += -O2 -DNDEBUG
endif

ifeq (,$(findstring msvc,$(platform)))
   CXXFLAGS += -std=c++17
else
   CXXFLAGS += -std:c++17
endif

### Finalize ###
OBJECTS		+= $(SOURCES_CXX:.cpp=.o) $(SOURCES_C:.c=.o) $(ASMFILES:.S=.o)
CXXFLAGS    += $(CPUOPTS) $(COREFLAGS) $(FFMPEGINCFLAGS) $(INCFLAGS) $(INCFLAGS_PLATFORM) $(PLATCFLAGS) $(fpic) $(PLATCFLAGS) $(CPUFLAGS) $(GLFLAGS) $(DYNAFLAGS)
CFLAGS		+= $(CPUOPTS) $(COREFLAGS) $(FFMPEGINCFLAGS) $(INCFLAGS) $(INCFLAGS_PLATFORM) $(PLATCFLAGS) $(fpic) $(PLATCFLAGS) $(CPUFLAGS) $(GLFLAGS) $(DYNAFLAGS)

LDFLAGS		+= $(FFMPEGLDFLAGS) $(fpic)
ifeq (,$(findstring android,$(platform))$(findstring msvc,$(platform))$(findstring qnx,$(platform)))
   LDFLAGS	+= -lpthread
endif

OBJOUT   = -o
LINKOUT  = -o

ifneq (,$(findstring msvc,$(platform)))
	OBJOUT = -Fo
	LINKOUT = -out:
	LD = link.exe
else ifneq (,$(findstring osx,$(platform)))
	LD = $(CXX)
else
	LD = $(CXX)
   CFLAGS += -ffunction-sections -fdata-sections
   CXXFLAGS += -ffunction-sections -fdata-sections
   LDFLAGS += -Wl,--gc-sections
endif

all: $(TARGET)

%.o: %.S
	$(AS) $(filter-out $(QNX_C_FORCE_INCLUDE),$(CFLAGS)) -c $(OBJOUT)$@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c $(OBJOUT)$@ $<

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $(OBJOUT)$@ $<

$(TARGET): $(OBJECTS)
ifeq ($(STATIC_LINKING), 1)
	$(AR) rcs $(TARGET) $(OBJECTS)
else
	$(LD) $(LINKOUT)$@ $(OBJECTS) $(LDFLAGS) $(GL_LIB)
endif

clean:
	rm -f $(filter-out $(CORE_DIR)/ext/zstd/lib/decompress/huf_decompress_amd64.S,$(OBJECTS)) $(TARGET)

.PHONY: clean

print-%:
	@echo '$*=$($*)'
