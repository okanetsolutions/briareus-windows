# Briareus for Windows: native C, Win32, and one library: miniaudio (public domain) for the meeting assistant's audio,
# downloaded at a pinned version into third_party/ on the first build and checked against its SHA-256. One file, app/canvas.cpp, is C++ in C style:
# the Windows SDK declares DirectWrite for C++ only.
# Build with MinGW-w64 GCC (for example WinLibs): `mingw32-make` or `make`. See build.bat for the same steps.

CC      = gcc
CXX     = g++
WINDRES = windres
BUILD   ?= build
OPT     ?= -O2
WARNINGS = -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wshadow -Wformat=2 -Wpointer-arith \
	-Wundef -Wvla -Wdouble-promotion
# CI builds with WERROR=1, so a new warning fails the pull request instead of piling up.
ifdef WERROR
WARNINGS += -Werror
endif
CFLAGS  ?= -std=c11 $(OPT) $(WARNINGS) -Wstrict-prototypes -Wmissing-prototypes
CXXFLAGS = -std=c++17 $(OPT) $(WARNINGS) -fno-exceptions -fno-rtti
DEFINES = -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A000006 -D_CRT_SECURE_NO_WARNINGS -Icore -Iapp
CFLAGS  += $(DEFINES)
CXXFLAGS += $(DEFINES)
LDFLAGS ?= -static -static-libgcc
CORE_LIBS = -lwinhttp -ladvapi32 -lole32
APP_LIBS  = $(CORE_LIBS) -lcomctl32 -lgdi32 -luser32 -lshell32 -luuid -ldwmapi -lwinmm -lmfplat -lmfreadwrite -lmfuuid -lshlwapi -luxtheme -lcomdlg32 -lmsimg32 -ld2d1 -ldwrite

CORE_SRC = $(wildcard core/*.c)
APP_SRC  = $(wildcard app/*.c)
APP_CXX  = $(wildcard app/*.cpp)
CORE_TEST_SRC = tests/harness.c $(wildcard tests/core_*.c)
APP_TEST_SRC  = tests/harness.c $(wildcard tests/app_*.c)
CORE_OBJ = $(patsubst core/%.c,$(BUILD)/core/%.o,$(CORE_SRC))
APP_OBJ  = $(patsubst app/%.c,$(BUILD)/app/%.o,$(APP_SRC)) $(patsubst app/%.cpp,$(BUILD)/app/%.o,$(APP_CXX))
CORE_TEST_OBJ = $(patsubst tests/%.c,$(BUILD)/tests/%.o,$(CORE_TEST_SRC))
APP_TEST_OBJ  = $(patsubst tests/%.c,$(BUILD)/tests/%.o,$(APP_TEST_SRC))
RES      = $(BUILD)/briareus.res.o

.PHONY: all app test coverage sanitize lint clean run

all: app test

app: $(BUILD)/Briareus.exe

$(BUILD)/Briareus.exe: $(CORE_OBJ) $(APP_OBJ) $(RES)
	$(CC) $(CFLAGS) -municode -mwindows -o $@ $^ $(LDFLAGS) $(APP_LIBS)

$(BUILD)/core_tests.exe: $(CORE_OBJ) $(CORE_TEST_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(CORE_LIBS)

# The app's objects, its WinMain included, under a console main of the tests' own: the shared helpers run without a window.
$(BUILD)/app_tests.exe: $(CORE_OBJ) $(APP_OBJ) $(APP_TEST_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(APP_LIBS)

test: $(BUILD)/core_tests.exe $(BUILD)/app_tests.exe
	$(BUILD)/core_tests.exe
	$(BUILD)/app_tests.exe

# Line coverage of core/ under the tests, in a build of its own: fails below COVERAGE_MIN percent and writes an HTML
# report to build-cov/coverage/index.html. Needs gcovr (pip install gcovr). app/ is drawing code, checked by running it.
GCOVR ?= gcovr
COVERAGE_MIN ?= 90
coverage:
	rm -rf build-cov/coverage && find build-cov -name '*.gcda' -delete 2>/dev/null; true
	$(MAKE) BUILD=build-cov OPT="-O0 --coverage" test
	mkdir -p build-cov/coverage
	$(GCOVR) --root . --object-directory build-cov --filter core/ --txt-summary --html-details build-cov/coverage/index.html 		--fail-under-line $(COVERAGE_MIN)

# The tests in a build of their own with the checks GCC adds at run time: undefined behaviour (signed overflow, bad shifts,
# misaligned or null access, out-of-bounds indexing of arrays) traps, fortified libc calls stop on an overrun they can
# see, and the stack protector on a smashed frame. Traps need no runtime library, so this works with MinGW, where
# AddressSanitizer does not: CI runs that one with MSVC.
SANITIZE = -fsanitize=undefined -fsanitize-undefined-trap-on-error -fno-sanitize=vptr -D_FORTIFY_SOURCE=2 -fstack-protector-strong
sanitize:
	$(MAKE) BUILD=build-san OPT="-O1 -g $(SANITIZE)" test

# Static analysis with cppcheck: fails on any finding. canvas.cpp is C in a .cpp file, so the C++-only checks are off there.
# third_party/ (miniaudio, compiled through app/miniaudio.c) is someone else's code and is left out.
CPPCHECK ?= cppcheck
lint: $(MINIAUDIO)
	$(CPPCHECK) -q -j4 --error-exitcode=1 --enable=warning,performance,portability --inline-suppr --std=c11 		-DUNICODE -D_UNICODE -D_WIN32 -D_WIN64 -Icore -Iapp --suppress=missingIncludeSystem 		--suppress=uninitMemberVarNoCtor --suppress=dangerousTypeCast:app/canvas.cpp 		--config-exclude=third_party --suppress='*:third_party/*' -i app/miniaudio.c core app tests

run: app
	$(BUILD)/Briareus.exe

$(BUILD)/core/%.o: core/%.c core/*.h | $(BUILD)/core
	$(CC) $(CFLAGS) -c -o $@ $<

# miniaudio is not kept in the repository: the pinned release is fetched once, and a download whose hash differs fails
# the build. To update it, change both lines and delete third_party/miniaudio.
MINIAUDIO_VERSION = 0.11.25
MINIAUDIO_SHA256 = ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89
MINIAUDIO = third_party/miniaudio/miniaudio.h
$(MINIAUDIO):
	mkdir -p $(dir $@)
	curl -fsSL https://raw.githubusercontent.com/mackron/miniaudio/$(MINIAUDIO_VERSION)/miniaudio.h -o $@.download
	echo "$(MINIAUDIO_SHA256)  $@.download" | sha256sum -c --quiet
	mv $@.download $@

$(BUILD)/app/miniaudio.o: app/miniaudio.c $(MINIAUDIO) | $(BUILD)/app
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/app/meet_audio.o: $(MINIAUDIO)

$(BUILD)/app/%.o: app/%.c app/*.h core/*.h | $(BUILD)/app
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/app/%.o: app/%.cpp app/*.h core/*.h | $(BUILD)/app
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILD)/tests/%.o: tests/%.c tests/*.h app/*.h core/*.h | $(BUILD)/tests
	$(CC) $(CFLAGS) -c -o $@ $<

$(RES): res/briareus.rc res/briareus.manifest res/briareus.ico app/resource.h | $(BUILD)
	$(WINDRES) -Ires -Iapp -i res/briareus.rc -o $@

$(BUILD) $(BUILD)/core $(BUILD)/app $(BUILD)/tests:
	mkdir -p $@

clean:
	rm -rf $(BUILD) build-cov build-san
