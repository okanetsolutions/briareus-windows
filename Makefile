# Briareus for Windows: native C, Win32, no third-party dependencies. One file, app/canvas.cpp, is C++ in C style:
# the Windows SDK declares DirectWrite for C++ only.
# Build with MinGW-w64 GCC (for example WinLibs): `mingw32-make` or `make`. See build.bat for the same steps.

CC      = gcc
CXX     = g++
WINDRES = windres
BUILD   ?= build
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CXXFLAGS = -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -fno-exceptions -fno-rtti
DEFINES = -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000006 -D_CRT_SECURE_NO_WARNINGS -Icore -Iapp
CFLAGS  += $(DEFINES)
CXXFLAGS += $(DEFINES)
LDFLAGS ?= -static -static-libgcc
CORE_LIBS = -lwinhttp -ladvapi32 -lole32
APP_LIBS  = $(CORE_LIBS) -lcomctl32 -lgdi32 -luser32 -lshell32 -luuid -ldwmapi -lwinmm -lmfplat -lmfreadwrite -lmfuuid -lshlwapi -luxtheme -lcomdlg32 -lmsimg32 -ld2d1 -ldwrite

CORE_SRC = $(wildcard core/*.c)
APP_SRC  = $(wildcard app/*.c)
APP_CXX  = $(wildcard app/*.cpp)
TEST_SRC = $(wildcard tests/*.c)
CORE_OBJ = $(patsubst core/%.c,$(BUILD)/core/%.o,$(CORE_SRC))
APP_OBJ  = $(patsubst app/%.c,$(BUILD)/app/%.o,$(APP_SRC)) $(patsubst app/%.cpp,$(BUILD)/app/%.o,$(APP_CXX))
TEST_OBJ = $(patsubst tests/%.c,$(BUILD)/tests/%.o,$(TEST_SRC))
RES      = $(BUILD)/briareus.res.o

.PHONY: all app test clean run

all: app test

app: $(BUILD)/Briareus.exe

$(BUILD)/Briareus.exe: $(CORE_OBJ) $(APP_OBJ) $(RES)
	$(CC) $(CFLAGS) -municode -mwindows -o $@ $^ $(LDFLAGS) $(APP_LIBS)

$(BUILD)/core_tests.exe: $(CORE_OBJ) $(TEST_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(CORE_LIBS)

test: $(BUILD)/core_tests.exe
	$(BUILD)/core_tests.exe

run: app
	$(BUILD)/Briareus.exe

$(BUILD)/core/%.o: core/%.c core/*.h | $(BUILD)/core
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/app/%.o: app/%.c app/*.h core/*.h | $(BUILD)/app
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/app/%.o: app/%.cpp app/*.h core/*.h | $(BUILD)/app
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/tests/%.o: tests/%.c core/*.h | $(BUILD)/tests
	$(CC) $(CFLAGS) -c -o $@ $<

$(RES): res/briareus.rc res/briareus.manifest res/briareus.ico app/resource.h | $(BUILD)
	$(WINDRES) -Ires -Iapp -i res/briareus.rc -o $@

$(BUILD) $(BUILD)/core $(BUILD)/app $(BUILD)/tests:
	mkdir -p $@

clean:
	rm -rf $(BUILD)
