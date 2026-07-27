#include "ace/actuator_serial.hpp"

#include <cerrno>
#include <cctype>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <utility>

namespace ace {

namespace {

speed_t baudToTermios(int baud_rate) {
    switch (baud_rate) {
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
#ifdef B230400
        case 230400: return B230400;
#endif
        default:
            throw std::runtime_error("unsupported serial baud rate: " + std::to_string(baud_rate));
    }
}

std::string paramOr(const ActuatorCommand& command, const std::string& key, const std::string& fallback) {
    const auto found = command.params.find(key);
    return found == command.params.end() ? fallback : found->second;
}

std::string upper(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return value;
}

std::string servoAngleValue(const ActuatorCommand& command) {
    const auto angle = command.params.find("angle_deg");
    if (angle != command.params.end()) {
        return angle->second;
    }

    const std::string action = upper(command.action);
    if (action == "OPEN") {
        return paramOr(command, "open_angle_deg", "100");
    }
    if (action == "CLOSE" || action == "CLOSED") {
        return paramOr(command, "close_angle_deg", "15");
    }
    if (action == "CENTER" || action == "IDLE" || action == "STOP") {
        return paramOr(command, "center_angle_deg", "90");
    }

    return paramOr(command, "default_angle_deg", "90");
}

} // namespace

SerialActuator::SerialActuator(std::string id, std::string type, SerialEndpoint endpoint)
    : id_(std::move(id)), type_(std::move(type)), endpoint_(std::move(endpoint)) {
    state_.actuator = id_;
    state_.type = type_;
    state_.action = "idle";
}

SerialActuator::~SerialActuator() {
    closePort();
}

Clock::time_point SerialActuator::apply(const ActuatorCommand& command) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string frame = encodeSerialFrame(command, endpoint_);
    ensureOpen();
    writeFrame(frame);

    state_.action = command.action;
    state_.params = command.params;
    state_.params["serial_device"] = endpoint_.device;
    state_.params["baud_rate"] = std::to_string(endpoint_.baud_rate);
    state_.params["serial_frame"] = frame;
    if (endpoint_.wait_for_ack) {
        state_.params["serial_ack"] = readAck().value_or("timeout");
    }
    state_.committed_at = Clock::now();
    return state_.committed_at;
}

ActuatorState SerialActuator::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

void SerialActuator::ensureOpen() {
    if (fd_ >= 0) {
        return;
    }
    if (endpoint_.device.empty()) {
        throw std::runtime_error("serial device is empty for actuator: " + id_);
    }

    fd_ = ::open(endpoint_.device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
        throw std::runtime_error("failed to open serial device " + endpoint_.device + ": " + std::strerror(errno));
    }

    termios tty{};
    if (::tcgetattr(fd_, &tty) != 0) {
        const std::string message = std::strerror(errno);
        closePort();
        throw std::runtime_error("failed to read serial settings: " + message);
    }

    ::cfmakeraw(&tty);
    const speed_t speed = baudToTermios(endpoint_.baud_rate);
    ::cfsetispeed(&tty, speed);
    ::cfsetospeed(&tty, speed);

    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    if (::tcsetattr(fd_, TCSANOW, &tty) != 0) {
        const std::string message = std::strerror(errno);
        closePort();
        throw std::runtime_error("failed to apply serial settings: " + message);
    }

    ::tcflush(fd_, TCIOFLUSH);
}

void SerialActuator::closePort() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void SerialActuator::writeFrame(const std::string& frame) {
    const char* data = frame.data();
    std::size_t remaining = frame.size();
    while (remaining > 0) {
        const ssize_t written = ::write(fd_, data, remaining);
        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            throw std::runtime_error("serial write failed on " + endpoint_.device + ": " + std::strerror(errno));
        }
        data += written;
        remaining -= static_cast<std::size_t>(written);
    }
    ::tcdrain(fd_);
}

std::optional<std::string> SerialActuator::readAck() {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(fd_, &read_set);

    timeval timeout{};
    timeout.tv_sec = endpoint_.ack_timeout_ms / 1000;
    timeout.tv_usec = (endpoint_.ack_timeout_ms % 1000) * 1000;

    const int ready = ::select(fd_ + 1, &read_set, nullptr, nullptr, &timeout);
    if (ready <= 0) {
        return std::nullopt;
    }

    std::string line;
    char c = '\0';
    while (::read(fd_, &c, 1) == 1) {
        if (c == '\n') {
            break;
        }
        if (c != '\r') {
            line.push_back(c);
        }
    }
    return line.empty() ? std::nullopt : std::optional<std::string>(line);
}

std::string encodeSerialFrame(const ActuatorCommand& command, const SerialEndpoint& endpoint) {
    const std::string channel = paramOr(command, "channel", endpoint.channel);
    if (command.type == "servo") {
        return "SERVO:" + channel + ":" + servoAngleValue(command) + "\n";
    }
    if (command.type == "light") {
        const std::string value = paramOr(command, "state", upper(command.action));
        return "LIGHT:" + channel + ":" + upper(value) + "\n";
    }
    if (command.type == "audio") {
        const std::string file = paramOr(command, "file", command.action);
        return "AUDIO:" + channel + ":PLAY:" + file + "\n";
    }
    return "CMD:" + channel + ":" + upper(command.type) + ":" + upper(command.action) + "\n";
}

} // namespace ace
