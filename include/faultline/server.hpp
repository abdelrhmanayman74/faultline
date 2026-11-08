#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "faultline/netlist.hpp"
#include "faultline/protocol.hpp"
#include "faultline/socket.hpp"
#include "faultline/thread_pool.hpp"
#include "faultline/vectors.hpp"

namespace faultline {

struct ServerConfig {
    std::string host = "127.0.0.1";  // loopback only unless asked otherwise
    std::uint16_t port = 0;          // 0 = any free port; see Server::port()
    std::size_t max_connections = 16;
    std::size_t job_workers = 2;
    std::size_t queue_capacity = 16;
};

// TCP server for the protocol in docs/PROTOCOL.md. Threads (DESIGN.md, "Server"):
//   - the thread that calls run(): polls the listening socket and the stop fd,
//     accepts connections, starts and joins connection threads;
//   - one thread per connection: reads a request, answers it, repeats;
//   - the job pool: runs FAULTSIM jobs (each one makes its own simulation pool).
//
// Three mutexes guard three independent pieces of state. No code path holds
// two of them at once, except jobs_mutex_ -> the pool's queue mutex inside
// try_submit, and nothing takes them in the other order, so they cannot deadlock.
//
// SIGINT/SIGTERM must be blocked in the calling thread BEFORE the Server is
// constructed: the constructor starts the job workers, and threads inherit the
// signal mask of the thread that creates them.
class Server {
public:
    explicit Server(const ServerConfig& config);  // binds and listens
    ~Server();                                     // calls shutdown()

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    std::uint16_t port() const { return port_; }

    // Serves until `stop_fd` becomes readable (the app passes a signalfd), then
    // shuts down: stop accepting, cancel jobs, close connections, join threads.
    void run(int stop_fd);

private:
    enum class JobState { Queued, Running, Done, Cancelled, Failed };

    struct Job {
        std::uint64_t id = 0;
        std::uint64_t owner = 0;  // connection that submitted it
        std::stop_source stop;    // request_stop() is thread-safe on its own
        // Guarded by jobs_mutex_:
        JobState state = JobState::Queued;
        std::string summary;      // one line for STATUS: counts if done, message if failed
        std::string result;       // full text for RESULT once done
    };

    struct Connection {
        Connection(std::uint64_t connection_id, Socket client)
            : id(connection_id), socket(std::move(client)) {}
        std::uint64_t id;
        // fd read by the connection thread without a lock; the accept thread
        // calls ::shutdown() on it during shutdown under connections_mutex_.
        // Closed only by the connection thread, also under that mutex, so the
        // number cannot be reused by a new file while the other thread uses it.
        Socket socket;
        bool finished = false;  // guarded by connections_mutex_
        std::jthread thread;    // joins in its destructor
    };

    void accept_one();
    void reap_finished_connections();
    void serve(Connection& connection);
    std::string handle(const std::string& payload, std::uint64_t connection_id, bool& close);

    std::string load(const Request& request);
    std::string simulate(const Request& request);
    std::string submit_faultsim(const Request& request, std::uint64_t connection_id);
    std::string status(const Request& request);
    std::string result(const Request& request);
    std::string cancel(const Request& request);

    std::shared_ptr<const Netlist> find_circuit(const std::string& name);
    void run_job(Job& job, const Netlist& netlist, std::vector<InputVector> vectors,
                 const FaultsimArgs& args);
    Job& find_job_locked(const std::string& id_text);  // throws ProtocolError if unknown
    void finish_job_locked(Job& job, JobState state);
    void cancel_jobs(std::uint64_t owner, bool all);
    static const char* state_name(JobState state);
    void shutdown();

    ServerConfig config_;
    Socket listener_;
    std::uint16_t port_ = 0;
    bool shut_down_ = false;  // only the run()/destructor thread uses it

    // Loaded circuits. A plain mutex, held only to copy a shared_ptr; parsing
    // happens outside it. shared_ptr<const>: a running SIM or job keeps its
    // netlist alive even if a LOAD replaces the name meanwhile.
    std::mutex circuits_mutex_;
    std::map<std::string, std::shared_ptr<const Netlist>> circuits_;

    std::mutex jobs_mutex_;
    std::map<std::uint64_t, std::shared_ptr<Job>> jobs_;
    std::deque<std::uint64_t> finished_jobs_;  // oldest first; trimmed on each submit
    std::uint64_t next_job_id_ = 1;
    bool stopping_ = false;  // set by shutdown(): refuse new jobs

    std::mutex connections_mutex_;
    std::list<Connection> connections_;  // std::list: elements never move, threads hold references
    std::uint64_t next_connection_id_ = 1;

    // Last member, so it is destroyed first: its workers run jobs that use the
    // members above. shutdown() stops it explicitly anyway.
    ThreadPool pool_;
};

}  // namespace faultline
