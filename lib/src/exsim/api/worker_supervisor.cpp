#include "exsim/api/research_service.hpp"
#include "exsim/api/api_json.hpp"
#include "exsim/research/run_artifacts.hpp"
#include <algorithm>

namespace exsim::api {
std::pair<std::string, std::string> ResearchService::worker_snapshot() const {
    if (options_.worker_run_id.empty()) return {"{}", "{}"};
    return find_run(options_.worker_run_id)->worker_snapshot();
}

ResearchService::WorkerResponse ResearchService::handle_worker_request(const HttpRequest& request) {
    // Capture the acknowledgment and status together at a slice boundary.
    // Both control locks are released before any socket write.
    std::lock_guard lock(mutation_mutex_);
    auto response = handle(request);
    auto [record, view] = worker_snapshot();
    return {std::move(response), std::move(record), std::move(view)};
}

std::shared_ptr<WorkerProcess> ResearchService::find_worker(const std::string& run_id) const {
    std::lock_guard lock(state_mutex_);
    const auto found = workers_.find(run_id);
    return found == workers_.end() ? nullptr : found->second;
}

void ResearchService::publish_worker_reply(const std::string& run_id, const WorkerReply& reply) {
    const auto previous = find_run(run_id);
    const auto experiment = find_experiment(previous->experiment_id());
    auto snapshot = std::shared_ptr<ResearchRun>(ResearchRun::load(options_.data_directory / "runs" / run_id,
        experiment->translated, postgres_, reply.record, options_.executable_path, render_build_provenance(), true));
    if (snapshot->run_id() != run_id || snapshot->experiment_id() != previous->experiment_id() ||
        snapshot->master_seed() != previous->master_seed()) throw std::runtime_error("worker snapshot identity mismatch");
    const auto directory = options_.data_directory / "runs" / run_id;
    if (snapshot->state() == ApiRunState::Completed || snapshot->state() == ApiRunState::Cancelled ||
        (snapshot->state() == ApiRunState::Failed && std::filesystem::exists(directory / "events.bin"))) {
        for (const auto* name : {"manifest.json", "summary.json", "events.bin", "fills.bin", "fill_positions.bin", "markouts.bin", "samples.bin"})
            if (!std::filesystem::is_regular_file(directory / name))
                throw std::runtime_error("worker terminal inspection snapshot is incomplete");
    }
    if (postgres_) postgres_->put(PostgresStore::Records::Runs, reply.record);
    else research::write_file_atomically(options_.data_directory / "runs" / run_id / "run.json", reply.record + "\n");
    std::lock_guard lock(state_mutex_);
    runs_by_id_[run_id] = snapshot;
    for (auto& run : runs_in_creation_order_) if (run->run_id() == run_id) { run = snapshot; break; }
    if (is_terminal(snapshot->state())) {
        workers_.erase(run_id);
        worker_run_views_.erase(run_id);
    } else worker_run_views_[run_id] = reply.run_view;
}

void ResearchService::fail_worker(const std::string& run_id, const std::string& reason) {
    auto worker = find_worker(run_id);
    // Stop/reap before publishing failure or releasing its scheduling slot.
    if (worker) worker->stop();
    try {
        // A process can die between individual cache renames. Preserve those
        // bytes as forensic evidence, never as a committed inspection snapshot.
        preserve_interrupted_caches(options_.data_directory / "runs" / run_id);
        find_run(run_id)->abandon(reason + "; final worker state is unavailable, position is last acknowledged");
    }
    catch (...) { postgres_requires_restart_ = true; }
    std::lock_guard lock(state_mutex_);
    workers_.erase(run_id);
    worker_run_views_.erase(run_id);
}

bool ResearchService::supervise_workers() {
    std::vector<std::pair<std::string, std::shared_ptr<WorkerProcess>>> active;
    {
        std::lock_guard lock(state_mutex_);
        for (const auto& entry : workers_) active.push_back(entry);
    }
    for (const auto& [id, worker] : active) {
        try {
            HttpRequest status;
            status.method = "GET"; status.target = status.path = "/v1/runs/" + id;
            const auto reply = worker->request(status);
            require_worker_success(reply);
            publish_worker_reply(id, reply);
        } catch (const std::exception& error) { fail_worker(id, error.what()); }
    }
    std::vector<std::shared_ptr<ResearchRun>> queued;
    {
        std::lock_guard lock(state_mutex_);
        for (const auto& run : runs_in_creation_order_) if (run->state() == ApiRunState::Queued) queued.push_back(run);
    }
    for (const auto& run : queued) {
        if (postgres_requires_restart_) break;
        {
            std::lock_guard lock(state_mutex_);
            if (workers_.size() >= options_.max_live_simulations) break;
        }
        try {
            const auto experiment = find_experiment(run->experiment_id());
            const auto [record, view] = run->worker_snapshot();
            (void)view;
            ApiJsonWriter bootstrap;
            bootstrap.begin_object().key("apiExecutable").string(options_.executable_path).key("root").string(std::filesystem::canonical(options_.data_directory).string())
                .key("runId").string(run->run_id()).key("config").raw(experiment->config_json).key("record").raw(record)
                .key("sourceRevision").string(options_.source_revision).key("releaseId").string(options_.release_id).end_object();
            // Persist the launch reservation before starting a writer. Recovery
            // never retries a possibly executed attempt under the same run ID.
            run->reserve_worker_launch();
            auto worker = std::make_shared<WorkerProcess>(options_.executable_path, bootstrap.take(),
                postgres_ ? postgres_->artifact_lock_fd() : worker_owner_lock_->descriptor());
            {
                std::lock_guard lock(state_mutex_);
                workers_.emplace(run->run_id(), worker);
            }
            HttpRequest status;
            status.method = "GET"; status.target = status.path = "/v1/runs/" + run->run_id();
            const auto reply = worker->request(status);
            require_worker_success(reply);
            publish_worker_reply(run->run_id(), reply);
        } catch (const std::exception& error) { fail_worker(run->run_id(), error.what()); }
    }
    // The executor waits between passes even while workers run. No scheduler
    // step, matching event or fill waits for one of these snapshots.
    std::lock_guard lock(state_mutex_);
    return !workers_.empty();
}

std::string ResearchService::isolated_query(const HttpRequest& request) {
    std::lock_guard query_lock(query_worker_mutex_);
    ApiJsonWriter bootstrap;
    bootstrap.begin_object().key("apiExecutable").string(options_.executable_path).key("root").string(std::filesystem::canonical(options_.data_directory).string())
        .key("query").boolean(true).key("experiments").begin_array();
    {
        std::lock_guard lock(state_mutex_);
        for (const auto& [id, experiment] : experiments_) { (void)id; bootstrap.raw(render_experiment_record(*experiment)); }
        bootstrap.end_array().key("runs").begin_array();
        for (const auto& run : runs_in_creation_order_)
            if (is_terminal(run->state())) bootstrap.raw(run->worker_snapshot().first);
    }
    bootstrap.end_array().end_object();
    WorkerProcess worker(options_.executable_path, bootstrap.take(),
        postgres_ ? postgres_->artifact_lock_fd() : worker_owner_lock_->descriptor());
    const auto reply = worker.request(request);
    require_worker_success(reply);
    return reply.response.body;
}

} // namespace exsim::api
