// faultline TCP job server (protocol: docs/PROTOCOL.md).
//   faultline_server --port P [--host 127.0.0.1] [--max-connections N]
//                    [--job-workers W] [--queue Q]
// The first line on stdout is "listening on <host>:<port>", so a script that
// passed --port 0 can read the port the kernel chose.

#include <signal.h>
#include <sys/signalfd.h>

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "faultline/server.hpp"
#include "faultline/socket.hpp"

namespace {

int usage() {
    std::cerr << "usage: faultline_server --port P [--host 127.0.0.1] [--max-connections N]\n"
                 "                        [--job-workers W] [--queue Q]\n"
                 "--port 0 picks a free port. Defaults: 16 connections, 2 job workers,\n"
                 "16 queued jobs.\n";
    return EXIT_FAILURE;
}

std::uint64_t parse_number(const std::string& text, const std::string& option,
                           std::uint64_t max) {
    std::uint64_t value = 0;
    const char* end = text.data() + text.size();
    const auto [ptr, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc() || ptr != end || value > max) {
        throw std::invalid_argument(option + " needs an integer from 0 to " +
                                    std::to_string(max) + ", got '" + text + "'");
    }
    return value;
}

faultline::ServerConfig parse_args(const std::vector<std::string>& args) {
    faultline::ServerConfig config;
    bool have_port = false;
    const std::uint64_t big = 1'000'000;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (i + 1 >= args.size()) {
            throw std::invalid_argument("missing value after '" + args[i] + "'");
        }
        const std::string& option = args[i];
        const std::string& value = args[++i];
        if (option == "--port") {
            config.port = static_cast<std::uint16_t>(
                parse_number(value, option, std::numeric_limits<std::uint16_t>::max()));
            have_port = true;
        } else if (option == "--host") {
            config.host = value;
        } else if (option == "--max-connections") {
            config.max_connections = parse_number(value, option, big);
        } else if (option == "--job-workers") {
            config.job_workers = parse_number(value, option, 1024);
        } else if (option == "--queue") {
            config.queue_capacity = parse_number(value, option, big);
        } else {
            throw std::invalid_argument("unknown option '" + option + "'");
        }
    }
    if (!have_port) {
        throw std::invalid_argument("--port is required");
    }
    return config;
}

}  // namespace

int main(int argc, char** argv) {
    faultline::ServerConfig config;
    try {
        config = parse_args(std::vector<std::string>(argv + 1, argv + argc));
    } catch (const std::invalid_argument& error) {
        std::cerr << "faultline_server: " << error.what() << '\n';
        return usage();
    }

    // Shutdown on SIGINT/SIGTERM without a signal handler. The two signals are
    // blocked here, before any thread exists; every thread started later
    // inherits the mask, so no thread is ever interrupted by them. A blocked
    // signal stays pending, and the signalfd turns "pending" into "readable",
    // which run() sees in the same poll() as the listening socket. No handler
    // means no async-signal-safety rules and no global flag.
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr) != 0) {
        std::cerr << "faultline_server: cannot block signals\n";
        return EXIT_FAILURE;
    }

    try {
        const faultline::Socket stop_fd(signalfd(-1, &stop_signals, SFD_CLOEXEC));
        if (!stop_fd.valid()) {
            throw std::system_error(errno, std::generic_category(), "signalfd");
        }
        faultline::Server server(config);
        std::cout << "listening on " << config.host << ':' << server.port() << std::endl;  // flush
        server.run(stop_fd.fd());
        std::cerr << "faultline_server: stopped\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        // The Server destructor has already shut everything down cleanly.
        std::cerr << "faultline_server: error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
