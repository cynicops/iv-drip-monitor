# Host-side tests for the IV drip monitor firmware.
#   make            build and run all tests
#   make report     run all tests and write build/test-results.md
#   make clean
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
SKETCH   := ../firmware/IV_Drip_Monitor/IV_Drip_Monitor.ino
HDRS     := harness.h $(wildcard mocks/*.h)

all: run

build:
	mkdir -p build

build/test_runner: test_main.cpp $(HDRS) $(SKETCH) | build
	$(CXX) $(CXXFLAGS) -Imocks -o $@ test_main.cpp -pthread

# Same sketch with CALIBRATION_MODE switched on, to test the Serial Plotter mode
build/IV_Drip_Monitor_calibration.ino: $(SKETCH) | build
	sed 's/^const bool CALIBRATION_MODE   = false;/const bool CALIBRATION_MODE   = true; /' $< > $@
	grep -q 'CALIBRATION_MODE   = true' $@

build/test_calibration: test_main.cpp $(HDRS) build/IV_Drip_Monitor_calibration.ino
	$(CXX) $(CXXFLAGS) -Imocks -DCALIBRATION_BUILD \
	  -DSKETCH='"build/IV_Drip_Monitor_calibration.ino"' -o $@ test_main.cpp -pthread

run: build/test_runner build/test_calibration
	./build/test_runner
	./build/test_calibration

report: build/test_runner build/test_calibration
	rm -f build/test-results.md
	printf '| Group | Test | What it checks | Result | Measured |\n|---|---|---|---|---|\n' > build/test-results.md
	TEST_REPORT=build/test-results.md ./build/test_runner
	TEST_REPORT=build/test-results.md ./build/test_calibration

clean:
	rm -rf build

.PHONY: all run report clean
