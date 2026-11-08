#include "faultline/server.hpp"

#include <poll.h>
#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <exception>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "faultline/bench_parser.hpp"
#include "faultline/bit_parallel_simulator.hpp"
#include "faultline/errors.hpp"
#include "faultline/fault_simulator.hpp"
#include "faultline/parallel_fault_simulator.hpp"

namespace faultline {

namespace {

constexpr std::size_t kMaxCircuits = 64;
// Finished jobs are remembered for STATUS/RESULT, but not forever: the oldest
// are forgotten once more than this many have finished.
constexpr std::size_t kKeptFinishedJobs = 256;

std::string format_fixed(double value, const char* format) {
    char text[64];
    std::snprintf(text, sizeof text, format, value);
    return text;
}

}  // namespace

Server::Server(const ServerConfig& config)
    : config_(config),
      listener_(listen_tcp(config.host, config.port)),
      port_(local_port(listener_)),
      pool_(config.job_workers, config.queue_capacity) {
    if (config.max_connections == 0) {
        throw std::invalid_argument("max_connections must be at least 1");
    }
}

Server::~Server() { shutdown(); }

void Server::run(int stop_fd) {
    // poll() waits on both descriptors at once, so a stop request is seen even
    // while no client is connecting.
    pollfd fds[2] = {{listener_.fd(), POLLIN, 0}, {stop_fd, POLLIN, 0}};
    while (true) {
        if (::poll(fds, 2, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::system_error(errno, std::generic_category(), "poll");
        }
        if (fds[1].revents != 0) {
            break;
        }
        if (fds[0].revents != 0) {
            accept_one();
        }
    }
    shutdown();
}

void Server::accept_one() {
    Socket client = accept_connection(listener_);
    if (!client.valid()) {
        return;
    }
    reap_finished_connections();

    std::unique_lock lock(connections_mutex_);
    const auto active = std::count_if(connections_.begin(), connections_.end(),
                                      [](const Connection& c) { return !c.finished; });
    if (static_cast<std::size_t>(active) >= config_.max_connections) {
        lock.unlock();
        try {
            write_frame(client, "ERR server full");  // a few bytes on a new socket: never blocks
        } catch (const std::exception&) {
            // The client is gone already; nothing to tell it.
        }
        return;  // `client` closes the connection
    }
    Connection& connection = connections_.emplace_back(next_connection_id_++, std::move(client));
    try {
        // Started after the element is in the list: the thread keeps a
        // reference to it, and std::list never moves its elements.
        connection.thread = std::jthread([this, &connection] { serve(connection); });
    } catch (const std::system_error&) {
        connections_.pop_back();  // could not start a thread: drop this client, keep serving
    }
}

void Server::reap_finished_connections() {
    std::list<Connection> finished;
    {
        std::lock_guard lock(connections_mutex_);
        for (auto it = connections_.begin(); it != connections_.end();) {
            const auto next = std::next(it);
            if (it->finished) {
                // splice relinks the node; the Connection object stays where it is.
                finished.splice(finished.end(), connections_, it);
            }
            it = next;
        }
    }
    // `finished` is destroyed here, outside the lock: each jthread joins. Those
    // threads have already set `finished`, so they are only returning.
}

void Server::serve(Connection& connection) {
    // Nothing may escape a thread function: an uncaught exception there calls
    // std::terminate and takes the whole server down.
    try {
        while (std::optional<std::string> payload = read_frame(connection.socket)) {
            bool close = false;
            write_frame(connection.socket, handle(*payload, connection.id, close));
            if (close) {
                break;
            }
        }
    } catch (const ProtocolError& error) {
        // Broken framing: we no longer know where the next frame starts, so
        // the only safe answer is an error and a close.
        try {
            write_frame(connection.socket, std::string("ERR ") + error.what());
        } catch (const std::exception&) {
            // The peer is gone too.
        }
    } catch (const std::exception&) {
        // Socket error: the peer reset the connection, or shutdown() woke us.
    }
    cancel_jobs(connection.id, false);  // a client that left does not want its results

    std::lock_guard lock(connections_mutex_);
    connection.socket.close();
    connection.finished = true;
}

std::string Server::handle(const std::string& payload, std::uint64_t connection_id, bool& close) {
    try {
        const Request request = parse_request(payload);
        const std::string& command = request.command;
        if (command == "LOAD") {
            return load(request);
        }
        if (command == "SIM") {
            return simulate(request);
        }
        if (command == "FAULTSIM") {
            return submit_faultsim(request, connection_id);
        }
        if (command == "STATUS") {
            return status(request);
        }
        if (command == "RESULT") {
            return result(request);
        }
        if (command == "CANCEL") {
            return cancel(request);
        }
        close = true;  // QUIT, the only command left after parse_request()
        return "OK bye";
    } catch (const std::exception& error) {
        // A bad request (parse error, unknown circuit, ...) is answered and the
        // connection stays usable: the framing is still intact.
        return std::string("ERR ") + error.what();
    }
}

std::string Server::load(const Request& request) {
    const std::string& name = request.args[0];
    std::istringstream text(request.body);
    // Parsed before taking the lock: other clients are not held up by a big file.
    auto netlist = std::make_shared<const Netlist>(parse_bench(text, name));
    {
        std::lock_guard lock(circuits_mutex_);
        if (!circuits_.contains(name) && circuits_.size() >= kMaxCircuits) {
            return "ERR too many circuits loaded (limit " + std::to_string(kMaxCircuits) + ")";
        }
        circuits_[name] = netlist;
    }
    return "OK " + name + " gates=" + std::to_string(netlist->logic_gate_count()) +
           " inputs=" + std::to_string(netlist->primary_inputs().size()) +
           " outputs=" + std::to_string(netlist->primary_outputs().size()) +
           " depth=" + std::to_string(netlist->depth());
}

std::shared_ptr<const Netlist> Server::find_circuit(const std::string& name) {
    std::lock_guard lock(circuits_mutex_);
    const auto it = circuits_.find(name);
    if (it == circuits_.end()) {
        throw ProtocolError("unknown circuit '" + name + "'");
    }
    return it->second;  // copy of the shared_ptr: usable after the lock is released
}

std::string Server::simulate(const Request& request) {
    const std::shared_ptr<const Netlist> netlist = find_circuit(request.args[0]);
    const std::size_t num_inputs = netlist->primary_inputs().size();
    std::istringstream text(request.body);
    const std::vector<InputVector> vectors = read_vectors(text, "vectors", num_inputs);

    // Short enough to run on the connection thread; only FAULTSIM is a job.
    BitParallelSimulator simulator(*netlist);
    std::string reply = "OK " + std::to_string(vectors.size()) + '\n';
    for (const PatternBlock& block : pack_vectors(vectors, num_inputs)) {
        simulator.simulate(block);
        for (std::size_t lane = 0; lane < block.count; ++lane) {
            reply += to_bit_string(simulator.outputs(lane)) + '\n';
        }
    }
    return reply;
}

std::string Server::submit_faultsim(const Request& request, std::uint64_t connection_id) {
    const FaultsimArgs args = parse_faultsim_args(request);
    std::shared_ptr<const Netlist> netlist = find_circuit(args.circuit);
    std::vector<InputVector> vectors;
    if (args.random_count == 0) {
        // Parsed now, so a bad vector file is reported in the reply, not later.
        std::istringstream text(request.body);
        vectors = read_vectors(text, "vectors", netlist->primary_inputs().size());
    }

    auto job = std::make_shared<Job>();
    job->owner = connection_id;

    std::lock_guard lock(jobs_mutex_);
    if (stopping_) {
        return "ERR server is shutting down";
    }
    while (finished_jobs_.size() > kKeptFinishedJobs) {
        jobs_.erase(finished_jobs_.front());
        finished_jobs_.pop_front();
    }
    job->id = next_job_id_;
    // The task owns shared_ptrs to the job record and the netlist, so both
    // stay alive while it runs even if the record is forgotten or the name is
    // reloaded. Submitting under jobs_mutex_ makes "queue it and record it"
    // one step: a worker that starts the job at once must wait for the lock,
    // and by then the job is in jobs_.
    auto task = [this, job, netlist, vectors = std::move(vectors), args]() mutable {
        run_job(*job, *netlist, std::move(vectors), args);
    };
    // try_submit, not submit: when the queue is full, the client hears BUSY at
    // once instead of its connection thread blocking. The returned future is
    // not needed (run_job catches everything); dropping it does not block.
    if (!pool_.try_submit(std::move(task))) {
        return "BUSY";
    }
    ++next_job_id_;
    jobs_.emplace(job->id, job);
    return "JOB " + std::to_string(job->id);
}

void Server::run_job(Job& job, const Netlist& netlist, std::vector<InputVector> vectors,
                     const FaultsimArgs& args) {
    {
        std::lock_guard lock(jobs_mutex_);
        if (job.state != JobState::Queued) {
            return;  // cancelled while it was waiting in the queue
        }
        job.state = JobState::Running;
    }

    JobState final_state = JobState::Done;
    std::string summary;
    std::string text;
    try {
        if (args.random_count > 0) {
            vectors = random_vectors(args.random_count, netlist.primary_inputs().size(), args.seed);
        }
        const auto start = std::chrono::steady_clock::now();
        // Its own pool, sized by the request; never this server's job pool:
        // waiting on chunks queued behind ourselves could deadlock.
        const FaultSimReport report =
            run_fault_simulation_parallel(netlist, vectors, args.threads, job.stop.get_token());
        const std::chrono::duration<double, std::milli> elapsed =
            std::chrono::steady_clock::now() - start;

        const std::size_t total = report.faults.size();
        const std::size_t detected = report.detected_count();
        const std::string coverage = format_fixed(report.coverage_percent(), "%.2f%%");
        const std::string time_ms = format_fixed(elapsed.count(), "%.3f");
        summary = "vectors=" + std::to_string(report.num_vectors) + " faults=" +
                  std::to_string(total) + " detected=" + std::to_string(detected) +
                  " coverage=" + coverage + " time_ms=" + time_ms;
        // Same lines and the same D/U format as `faultline faultsim`.
        text = "vectors:    " + std::to_string(report.num_vectors) + '\n' +
               "faults:     " + std::to_string(total) + '\n' +
               "detected:   " + std::to_string(detected) + '\n' +
               "undetected: " + std::to_string(total - detected) + '\n' +
               "coverage:   " + coverage + '\n' +
               "time_ms:    " + time_ms + " (" + std::to_string(args.threads) +
               (args.threads == 1 ? " thread)\n" : " threads)\n");
        for (std::size_t i = 0; args.details && i < total; ++i) {
            const std::string name = fault_name(netlist, report.faults[i]);
            text += report.status[i].detected
                        ? "D " + name + ' ' + std::to_string(report.status[i].first_vector) + '\n'
                        : "U " + name + '\n';
        }
    } catch (const Cancelled&) {
        final_state = JobState::Cancelled;
    } catch (const std::exception& error) {
        final_state = JobState::Failed;
        summary = std::string("error=") + error.what();
        text = error.what();
    }

    std::lock_guard lock(jobs_mutex_);
    job.summary = std::move(summary);
    job.result = std::move(text);
    finish_job_locked(job, final_state);
}

void Server::finish_job_locked(Job& job, JobState state) {
    job.state = state;
    finished_jobs_.push_back(job.id);
}

Server::Job& Server::find_job_locked(const std::string& id_text) {
    const std::uint64_t id = parse_job_id(id_text);
    const auto it = jobs_.find(id);
    if (it == jobs_.end()) {
        throw ProtocolError("unknown job " + std::to_string(id));  // handle() replies ERR
    }
    return *it->second;
}

const char* Server::state_name(JobState state) {
    switch (state) {
        case JobState::Queued: return "queued";
        case JobState::Running: return "running";
        case JobState::Done: return "done";
        case JobState::Cancelled: return "cancelled";
        case JobState::Failed: return "failed";
    }
    return "unknown";
}

std::string Server::status(const Request& request) {
    if (request.args.empty()) {
        // Three short lock scopes one after the other, never nested.
        std::size_t connections = 0;
        {
            std::lock_guard lock(connections_mutex_);
            for (const Connection& c : connections_) {
                connections += c.finished ? 0 : 1;
            }
        }
        std::size_t circuits = 0;
        {
            std::lock_guard lock(circuits_mutex_);
            circuits = circuits_.size();
        }
        std::size_t count[5] = {};  // indexed by JobState
        {
            std::lock_guard lock(jobs_mutex_);
            for (const auto& entry : jobs_) {
                ++count[static_cast<int>(entry.second->state)];
            }
        }
        std::string reply = "OK connections=" + std::to_string(connections) +
                            " circuits=" + std::to_string(circuits);
        for (int s = 0; s < 5; ++s) {
            reply += ' ' + std::string(state_name(static_cast<JobState>(s))) + '=' +
                     std::to_string(count[s]);
        }
        return reply;
    }

    std::lock_guard lock(jobs_mutex_);
    const Job& job = find_job_locked(request.args[0]);
    std::string reply = "OK job=" + std::to_string(job.id) + " state=" + state_name(job.state);
    if (!job.summary.empty()) {
        reply += ' ' + job.summary;
    }
    return reply;
}

std::string Server::result(const Request& request) {
    std::lock_guard lock(jobs_mutex_);
    const Job& job = find_job_locked(request.args[0]);
    const std::string id = std::to_string(job.id);
    switch (job.state) {
        case JobState::Queued:
        case JobState::Running: return std::string("PENDING ") + state_name(job.state);
        case JobState::Done: return "OK\n" + job.result;
        case JobState::Cancelled: return "ERR job " + id + " was cancelled";
        case JobState::Failed: return "ERR job " + id + " failed: " + job.result;
    }
    return "ERR internal error";
}

std::string Server::cancel(const Request& request) {
    std::lock_guard lock(jobs_mutex_);
    Job& job = find_job_locked(request.args[0]);
    if (job.state == JobState::Queued) {
        job.stop.request_stop();
        finish_job_locked(job, JobState::Cancelled);  // its worker will skip it
        return "OK";
    }
    if (job.state == JobState::Running) {
        // The job notices within one block of 64 vectors and ends as cancelled.
        job.stop.request_stop();
        return "OK";
    }
    return "ERR job " + std::to_string(job.id) + " is already " + state_name(job.state);
}

void Server::cancel_jobs(std::uint64_t owner, bool all) {
    std::lock_guard lock(jobs_mutex_);
    for (auto& [id, job] : jobs_) {
        if (!all && job->owner != owner) {
            continue;
        }
        if (job->state == JobState::Queued) {
            job->stop.request_stop();
            finish_job_locked(*job, JobState::Cancelled);
        } else if (job->state == JobState::Running) {
            job->stop.request_stop();
        }
    }
}

void Server::shutdown() {
    if (shut_down_) {
        return;
    }
    shut_down_ = true;

    // 1. Stop accepting. New connection attempts are refused by the kernel.
    listener_.close();

    // 2. Refuse new jobs, then stop every queued and running one.
    {
        std::lock_guard lock(jobs_mutex_);
        stopping_ = true;
    }
    cancel_jobs(0, true);

    // 3. Wake every connection thread. shutdown(SHUT_RDWR) makes a blocked
    //    recv() return 0 (end of stream) and later sends fail, so each thread
    //    leaves its loop. The fd stays open until its own thread closes it.
    std::list<Connection> connections;
    {
        std::lock_guard lock(connections_mutex_);
        for (Connection& connection : connections_) {
            if (!connection.finished) {
                ::shutdown(connection.socket.fd(), SHUT_RDWR);
            }
        }
        connections.splice(connections.end(), connections_);
    }
    // 4. Join them, outside the lock: each one takes it to set `finished`.
    connections.clear();

    // 5. Queued tasks see their job cancelled and return at once; running jobs
    //    throw Cancelled within one block. Then the workers are joined.
    pool_.shutdown();
}

}  // namespace faultline
