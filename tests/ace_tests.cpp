#include "ace/actuator_bus.hpp"
#include "ace/actuator_serial.hpp"
#include "ace/actuator_virtual.hpp"
#include "ace/scheduler.hpp"
#include "ace/sequence.hpp"
#include "ace/trigger.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void testSequenceParser() {
    const auto sequence = ace::loadSequenceFile("sequences/dome_spin_beep.seq.json");
    require(sequence.name == "dome_spin_beep", "sequence name parsed");
    require(sequence.events.size() == 4, "sequence event count");
    require(sequence.events[0].command.actuator == "dome_rotation", "first actuator parsed");
    require(sequence.events[1].command.params.at("state") == "on", "params parsed");
}

void testTriggerParser() {
    const auto triggers = ace::loadTriggerMapFile("triggers/trigger_map.json");
    require(triggers.contains("panel_3_open"), "trigger found");
    require(triggers.at("panel_3_open").commands.size() == 2, "trigger command count");
}

void testSchedulerDispatchesTimedEvents() {
    ace::ActuatorBus bus;
    bus.registerActuator(std::make_unique<ace::VirtualActuator>("test_light", "light"));
    bus.registerActuator(std::make_unique<ace::VirtualActuator>("test_speaker", "audio"));

    ace::ActuatorCommand light;
    light.actuator = "test_light";
    light.type = "light";
    light.action = "set";
    light.params["state"] = "on";

    ace::ActuatorCommand speaker;
    speaker.actuator = "test_speaker";
    speaker.type = "audio";
    speaker.action = "play";
    speaker.params["file"] = "chirp.wav";

    ace::CompiledSequence sequence;
    sequence.name = "unit_test";
    sequence.events = {
        ace::TimedEvent{0, 0, light},
        ace::TimedEvent{15, 1, speaker},
    };
    sequence.events[1].command.scheduled_ms = 15;

    ace::Scheduler scheduler(bus);
    scheduler.load(sequence);
    scheduler.run();

    const auto telemetry = scheduler.telemetry();
    require(telemetry.events_dispatched == 2, "scheduler dispatched two events");
    require(telemetry.max_jitter_ms < 50.0, "scheduler jitter under broad local tolerance");
}

void testSerialEncoding() {
    ace::SerialEndpoint endpoint;
    endpoint.channel = "1";

    ace::ActuatorCommand servo;
    servo.type = "servo";
    servo.action = "move_to";
    servo.params["angle_deg"] = "90";
    require(ace::encodeSerialFrame(servo, endpoint) == "SERVO:1:90\n", "servo angle frame");

    ace::ActuatorCommand panel;
    panel.type = "servo";
    panel.action = "open";
    panel.params["channel"] = "0";
    panel.params["open_angle_deg"] = "105";
    require(ace::encodeSerialFrame(panel, endpoint) == "SERVO:0:105\n", "servo action frame");

    ace::ActuatorCommand light;
    light.type = "light";
    light.action = "set";
    light.params["channel"] = "13";
    light.params["state"] = "on";
    require(ace::encodeSerialFrame(light, endpoint) == "LIGHT:13:ON\n", "light frame");
}

void testSerialEncodingUsesPca9685ChannelZero() {
    ace::SerialEndpoint endpoint;
    endpoint.channel = "0";

    ace::ActuatorCommand servo;
    servo.type = "servo";
    servo.action = "move_to";
    servo.params["angle_deg"] = "120";

    require(ace::encodeSerialFrame(servo, endpoint) == "SERVO:0:120\n", "servo channel zero frame");
}

void testSerialEncodingClosesNamedActionsToAngles() {
    ace::SerialEndpoint endpoint;
    endpoint.channel = "0";

    ace::ActuatorCommand close;
    close.type = "servo";
    close.action = "close";
    require(ace::encodeSerialFrame(close, endpoint) == "SERVO:0:15\n", "servo close default frame");

    ace::ActuatorCommand center;
    center.type = "servo";
    center.action = "center";
    center.params["center_angle_deg"] = "88";
    require(ace::encodeSerialFrame(center, endpoint) == "SERVO:0:88\n", "servo center override frame");
}

} // namespace

int main() {
    try {
        testSequenceParser();
        testTriggerParser();
        testSchedulerDispatchesTimedEvents();
        testSerialEncoding();
        testSerialEncodingUsesPca9685ChannelZero();
        testSerialEncodingClosesNamedActionsToAngles();
    } catch (const std::exception& error) {
        std::cerr << "test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    std::cout << "ace_tests: ok\n";
    return EXIT_SUCCESS;
}
