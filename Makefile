CXX ?= c++
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Wpedantic -Iinclude -O2
ACE_SOURCES = \
	src/actuator_bus.cpp \
	src/actuator_serial.cpp \
	src/actuator_virtual.cpp \
	src/json.cpp \
	src/scheduler.cpp \
	src/sequence.cpp \
	src/sync_engine.cpp \
	src/telemetry.cpp \
	src/trigger.cpp

.PHONY: all test clean

all: build/ace_cli build/ace_tests build/stress_actuators

build:
	mkdir -p build

build/ace_cli: $(ACE_SOURCES) apps/ace_cli/main.cpp | build
	$(CXX) $(CXXFLAGS) $(ACE_SOURCES) apps/ace_cli/main.cpp -o $@

build/ace_tests: $(ACE_SOURCES) tests/ace_tests.cpp | build
	$(CXX) $(CXXFLAGS) $(ACE_SOURCES) tests/ace_tests.cpp -o $@

build/stress_actuators: $(ACE_SOURCES) bench/stress_actuators.cpp | build
	$(CXX) $(CXXFLAGS) $(ACE_SOURCES) bench/stress_actuators.cpp -o $@

test: build/ace_tests
	./build/ace_tests
	bash tests/verify_baud_rate.sh
	python3 scripts/fake_pca9685_bridge.py --self-test

clean:
	rm -rf build
