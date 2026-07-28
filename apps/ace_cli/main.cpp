#include "ace/actuator_bus.hpp"
#include "ace/actuator_serial.hpp"
#include "ace/actuator_virtual.hpp"
#include "ace/scheduler.hpp"
#include "ace/sequence.hpp"
#include "ace/trigger.hpp"

#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace {

void registerActuators(ace::ActuatorBus& bus, const std::vector<ace::TimedEvent>& events, const std::string& serial_device = "") {
    std::map<std::string, std::string> actuators;
    for (const auto& event : events) {
        actuators[event.command.actuator] = event.command.type;
    }
    for (const auto& [id, type] : actuators) {
        if (!serial_device.empty() && type == "servo") {
            ace::SerialEndpoint endpoint;
            endpoint.device = serial_device;
            endpoint.wait_for_ack = true;
            endpoint.ack_timeout_ms = 500;
            bus.registerActuator(std::make_unique<ace::SerialActuator>(id, type, endpoint));
        } else {
            bus.registerActuator(std::make_unique<ace::VirtualActuator>(id, type));
        }
    }
}

void printTelemetry(const ace::TelemetrySnapshot& telemetry) {
    std::cout << "events=" << telemetry.events_dispatched
              << " misses=" << telemetry.deadline_misses
              << " mean_jitter_ms=" << std::fixed << std::setprecision(3) << telemetry.mean_jitter_ms
              << " p99_jitter_ms=" << telemetry.p99_jitter_ms
              << " max_jitter_ms=" << telemetry.max_jitter_ms << '\n';

    if (!telemetry.sync_violations.empty()) {
        std::cout << "sync_violations=" << telemetry.sync_violations.size() << '\n';
        for (const auto& violation : telemetry.sync_violations) {
            std::cout << "  t=" << violation.scheduled_ms
                      << " drift_ms=" << violation.drift_ms << '\n';
        }
    }
}

void printStates(const ace::ActuatorBus& bus) {
    for (const auto& state : bus.states()) {
        std::cout << state.actuator << " [" << state.type << "] -> " << state.action;
        if (!state.params.empty()) {
            std::cout << " {";
            bool first = true;
            for (const auto& [key, value] : state.params) {
                if (!first) {
                    std::cout << ", ";
                }
                std::cout << key << "=" << value;
                first = false;
            }
            std::cout << "}";
        }
        std::cout << '\n';
    }
}

int runSequence(const std::string& path) {
    ace::CompiledSequence sequence = ace::loadSequenceFile(path);
    ace::ActuatorBus bus;
    registerActuators(bus, sequence.events);

    ace::Scheduler scheduler(bus);
    scheduler.load(sequence);
    scheduler.run();

    std::cout << "sequence=" << sequence.name << '\n';
    printTelemetry(scheduler.telemetry());
    printStates(bus);
    return 0;
}

int runSequenceSerial(const std::string& path, const std::string& serial_device) {
    ace::CompiledSequence sequence = ace::loadSequenceFile(path);
    ace::ActuatorBus bus;
    registerActuators(bus, sequence.events, serial_device);

    ace::Scheduler scheduler(bus);
    scheduler.load(sequence);
    scheduler.run();

    std::cout << "sequence=" << sequence.name << '\n';
    std::cout << "serial_device=" << serial_device << '\n';
    printTelemetry(scheduler.telemetry());
    printStates(bus);
    return 0;
}

int runTrigger(const std::string& path, const std::string& trigger_id) {
    const ace::TriggerMap triggers = ace::loadTriggerMapFile(path);
    const auto found = triggers.find(trigger_id);
    if (found == triggers.end()) {
        throw std::runtime_error("unknown trigger: " + trigger_id);
    }

    ace::CompiledSequence immediate = ace::compileImmediateSequence(trigger_id, found->second.commands);
    ace::ActuatorBus bus;
    registerActuators(bus, immediate.events);

    ace::Scheduler scheduler(bus);
    scheduler.fireTrigger(found->second);

    std::cout << "trigger=" << trigger_id << '\n';
    printTelemetry(scheduler.telemetry());
    printStates(bus);
    return 0;
}

void printUsage() {
    std::cerr << "usage:\n"
              << "  ace_cli sequence <file.seq.json>\n"
              << "  ace_cli sequence-serial <file.seq.json> <serial-device>\n"
              << "  ace_cli trigger <trigger_map.json> <trigger_id>\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc >= 3 && std::string(argv[1]) == "sequence") {
            return runSequence(argv[2]);
        }
        if (argc >= 4 && std::string(argv[1]) == "sequence-serial") {
            return runSequenceSerial(argv[2], argv[3]);
        }
        if (argc >= 4 && std::string(argv[1]) == "trigger") {
            return runTrigger(argv[2], argv[3]);
        }
        printUsage();
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
