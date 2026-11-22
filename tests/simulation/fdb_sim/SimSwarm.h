#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fdb_sim
{

/// Pure C++ FoundationDB Joshua-style multi-process swarm runner.
/// Uses POSIX `fork`/`execv`/`pipe`/`waitpid` (zero threads) so each seed runs in a
/// completely isolated process with fresh ClickHouse global state, full linker clock/thread
/// instrumentation, parallel CPU utilization, cross-run `--host-delay` determinism checking,
/// negative quorum safety controls, and aggregated `SOMETIMES` coverage reporting.
class SimSwarmRunner
{
public:
    struct JobSpec
    {
        std::string name;
        uint64_t seed = 1;
        int nodes = 3;
        std::string profile = "balanced";
        std::string storage = "mem"; // "mem", "lsmt-mem", "lsmt-disk"
        int64_t duration_ms = 15000;
        bool host_delay = false;
        bool strict_raft_commit = false;
        bool unsafe_quorum_control = false;
        bool expect_failure = false;
    };

    struct JobResult
    {
        JobSpec spec;
        int exit_code = -1;
        bool signalled = false;
        int term_signal = 0;
        bool passed = false;
        double elapsed_s = 0.0;
        std::string command;
        std::string stdout_text;
        std::string stderr_text;
        std::string trace_hash;
        uint64_t executed_events = 0;
        uint64_t final_commit_index = 0;
        uint64_t final_digest = 0;
        std::map<std::string, uint64_t> coverage;
    };

    static std::vector<JobSpec> buildSuite(const std::string & suite)
    {
        std::vector<JobSpec> jobs;
        auto add_group = [&](const std::string & name, const std::string & profile, int nodes,
                             const std::string & storage, const std::vector<uint64_t> & seeds, int64_t duration_ms)
        {
            for (uint64_t s : seeds)
                jobs.push_back(JobSpec{name + "_s" + std::to_string(s), s, nodes, profile, storage, duration_ms, false, false, false, false});
        };

        if (suite == "quick")
        {
            add_group("mem-balanced-3n", "balanced", 3, "mem", {1, 2}, 12000);
            add_group("mem-power-loss-3n", "power-loss", 3, "mem", {21, 22}, 14000);
            add_group("mem-snapshot-storm-3n", "snapshot-storm", 3, "mem", {31}, 14000);
            add_group("mem-partition-5n", "partition-storm", 5, "mem", {41}, 14000);
            add_group("lsmt-disk-chaos-3n", "chaos", 3, "lsmt-disk", {61}, 14000);
        }
        else if (suite == "full")
        {
            add_group("mem-balanced-3n", "balanced", 3, "mem", {1, 2, 3, 4}, 15000);
            add_group("mem-chaos-3n", "chaos", 3, "mem", {12, 13, 14, 15}, 15000);
            add_group("mem-power-loss-3n", "power-loss", 3, "mem", {21, 22, 23, 24}, 15000);
            add_group("mem-snapshot-storm-3n", "snapshot-storm", 3, "mem", {31, 32, 33}, 15000);
            add_group("mem-partition-5n", "partition-storm", 5, "mem", {41, 42, 43}, 15000);
            add_group("lsmt-mem-chaos-3n", "chaos", 3, "lsmt-mem", {51, 52, 53}, 15000);
            add_group("lsmt-disk-chaos-3n", "chaos", 3, "lsmt-disk", {61, 62, 63}, 15000);
        }
        else
        {
            throw std::runtime_error("Unknown swarm suite: " + suite + " (expected quick or full)");
        }
        return jobs;
    }

    static int run(
        const std::string & self_exe,
        const std::string & suite,
        const std::filesystem::path & output_dir,
        size_t max_workers,
        bool strict_raft_commit)
    {
        std::filesystem::create_directories(output_dir);
        const auto work_dir = output_dir / "work";
        std::filesystem::create_directories(work_dir);

        const auto t_start = std::chrono::steady_clock::now();

        /// 1. Run instrumentation control in an isolated child process.
        {
            auto ctrl = runOneSubprocess(self_exe, {"--instrumentation-control"}, JobSpec{"instrumentation-control"}, work_dir);
            if (ctrl.exit_code != 0 || ctrl.stdout_text.find("PASS fdb-sim-instrumentation-control") == std::string::npos)
            {
                std::cerr << "[FAIL] instrumentation-control: " << ctrl.stderr_text << '\n';
                return 1;
            }
            std::cout << "[PASS] instrumentation-control\n" << std::flush;
        }

        /// 2. Run negative safety control: unsafe quorum=1 under partition-storm MUST fail!
        {
            JobSpec neg;
            neg.name = "negative-unsafe-quorum-control";
            neg.seed = 777;
            neg.nodes = 3;
            neg.profile = "partition-storm";
            neg.storage = "mem";
            neg.duration_ms = 12000;
            neg.unsafe_quorum_control = true;
            neg.expect_failure = true;
            auto res = runJob(self_exe, neg, work_dir);
            if (!res.passed)
            {
                std::cerr << "[FAIL] negative-unsafe-quorum-control unexpectedly passed!\n";
                return 1;
            }
            std::cout << "[PASS] negative-unsafe-quorum-control (oracle rejected unsafe quorum)\n" << std::flush;
        }

        /// 3. Run all campaign jobs in parallel across `max_workers` child processes.
        auto jobs = buildSuite(suite);
        for (auto & j : jobs)
            j.strict_raft_commit = strict_raft_commit;

        std::vector<JobResult> results = runParallel(self_exe, jobs, work_dir, std::max<size_t>(1, max_workers));

        /// 4. Determinism verification: pick up to 3 passing runs across distinct profiles/storages
        /// and re-run with `--host-delay`, asserting byte-identical `trace_hash` and `final_digest`.
        std::vector<JobSpec> det_jobs;
        std::vector<const JobResult *> det_originals;
        for (const auto & r : results)
        {
            if (r.passed && !r.trace_hash.empty() && det_jobs.size() < 3)
            {
                JobSpec replay = r.spec;
                replay.name = "det_" + r.spec.name;
                replay.host_delay = true;
                det_jobs.push_back(replay);
                det_originals.push_back(&r);
            }
        }
        auto det_results = runParallel(self_exe, det_jobs, work_dir, std::max<size_t>(1, max_workers));
        size_t det_passed = 0;
        for (size_t i = 0; i < det_results.size(); ++i)
        {
            const auto & orig = *det_originals[i];
            const auto & rep = det_results[i];
            const bool same = rep.passed
                && rep.trace_hash == orig.trace_hash
                && rep.executed_events == orig.executed_events
                && rep.final_commit_index == orig.final_commit_index
                && rep.final_digest == orig.final_digest;
            if (same)
                ++det_passed;
            std::cout << "[" << (same ? "PASS" : "FAIL") << "] determinism " << orig.spec.name
                      << " orig=" << orig.trace_hash << " replay=" << rep.trace_hash << '\n' << std::flush;
        }

        /// 5. Aggregate SOMETIMES coverage & totals.
        size_t passed_count = 0;
        std::map<std::string, uint64_t> total_coverage;
        uint64_t total_events = 0;
        for (const auto & r : results)
        {
            if (r.passed)
            {
                ++passed_count;
                total_events += r.executed_events;
                for (const auto & [k, v] : r.coverage)
                    total_coverage[k] += v;
            }
        }

        const double total_elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
        const bool all_ok = (passed_count == results.size()) && (det_passed == det_results.size());

        std::cout << "\n=== C++ FDB-Style Simulation Swarm Report ===\n";
        std::cout << "Verdict: " << (all_ok ? "PASS" : "FAIL")
                  << " (" << passed_count << "/" << results.size() << " seeds passed, "
                  << det_passed << "/" << det_results.size() << " determinism checks passed, "
                  << total_events << " total virtual events, "
                  << std::fixed << std::setprecision(2) << total_elapsed << "s)\n";
        std::cout << "SOMETIMES Coverage:\n";
        for (const auto & [k, v] : total_coverage)
            std::cout << "  " << std::left << std::setw(36) << k << ": " << v << '\n';

        if (!all_ok)
        {
            std::cout << "\nFailures & Repro Commands:\n";
            for (const auto & r : results)
            {
                if (!r.passed)
                {
                    std::cout << "  FAIL " << r.spec.name
                              << " (exit=" << r.exit_code << ", sig=" << r.term_signal << ")\n"
                              << "    repro: " << r.command << '\n'
                              << "    error: " << firstLine(r.stderr_text.empty() ? r.stdout_text : r.stderr_text) << '\n';
                }
            }
        }

        return all_ok ? 0 : 1;
    }

private:
    static std::string firstLine(const std::string & s)
    {
        auto pos = s.find('\n');
        return pos == std::string::npos ? s : s.substr(0, pos);
    }

    static std::string extractJsonString(const std::string & json, const std::string & key)
    {
        const std::string needle = "\"" + key + "\":\"";
        const auto pos = json.find(needle);
        if (pos == std::string::npos)
            return "";
        const auto start = pos + needle.size();
        const auto end = json.find('"', start);
        return end == std::string::npos ? "" : json.substr(start, end - start);
    }

    static uint64_t extractJsonU64(const std::string & json, const std::string & key)
    {
        const std::string needle = "\"" + key + "\":";
        const auto pos = json.find(needle);
        if (pos == std::string::npos)
            return 0;
        const auto start = pos + needle.size();
        size_t end = start;
        while (end < json.size() && json[end] >= '0' && json[end] <= '9')
            ++end;
        return end > start ? std::stoull(json.substr(start, end - start)) : 0;
    }

    static std::map<std::string, uint64_t> extractCoverageMap(const std::string & json)
    {
        std::map<std::string, uint64_t> out;
        const std::string needle = "\"coverage\":{";
        const auto pos = json.find(needle);
        if (pos == std::string::npos)
            return out;
        size_t i = pos + needle.size();
        const auto close = json.find('}', i);
        if (close == std::string::npos)
            return out;
        while (i < close)
        {
            const auto q1 = json.find('"', i);
            if (q1 == std::string::npos || q1 >= close)
                break;
            const auto q2 = json.find('"', q1 + 1);
            if (q2 == std::string::npos || q2 >= close)
                break;
            const std::string key = json.substr(q1 + 1, q2 - q1 - 1);
            const auto colon = json.find(':', q2 + 1);
            if (colon == std::string::npos || colon >= close)
                break;
            size_t num_start = colon + 1;
            size_t num_end = num_start;
            while (num_end < close && json[num_end] >= '0' && json[num_end] <= '9')
                ++num_end;
            if (num_end > num_start)
                out[key] = std::stoull(json.substr(num_start, num_end - num_start));
            i = num_end + 1;
        }
        return out;
    }

    static std::vector<std::string> buildArgs(const JobSpec & spec, const std::filesystem::path & state_dir)
    {
        std::vector<std::string> args = {
            "--seed", std::to_string(spec.seed),
            "--nodes", std::to_string(spec.nodes),
            "--profile", spec.profile,
            "--duration-ms", std::to_string(spec.duration_ms),
            "--state-dir", state_dir.string(),
            "--json-summary",
        };
        if (spec.storage == "lsmt-mem")
            args.push_back("--lsmt");
        else if (spec.storage == "lsmt-disk")
            args.push_back("--lsmt-disk");
        if (spec.host_delay)
            args.push_back("--host-delay");
        if (spec.strict_raft_commit)
            args.push_back("--strict-raft-commit");
        if (spec.unsafe_quorum_control)
            args.push_back("--unsafe-quorum-control");
        return args;
    }

    static JobResult runOneSubprocess(
        const std::string & self_exe,
        const std::vector<std::string> & extra_args,
        const JobSpec & spec,
        const std::filesystem::path & work_dir)
    {
        JobResult res;
        res.spec = spec;
        const auto t0 = std::chrono::steady_clock::now();

        std::vector<std::string> argv_storage;
        argv_storage.push_back(self_exe);
        for (const auto & a : extra_args)
            argv_storage.push_back(a);
        std::ostringstream cmd_ss;
        for (size_t i = 0; i < argv_storage.size(); ++i)
            cmd_ss << (i ? " " : "") << argv_storage[i];
        res.command = cmd_ss.str();

        int out_pipe[2]{-1, -1};
        int err_pipe[2]{-1, -1};
        if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0)
            throw std::runtime_error("SimSwarmRunner: pipe() failed");

        const pid_t pid = fork();
        if (pid < 0)
            throw std::runtime_error("SimSwarmRunner: fork() failed");

        if (pid == 0)
        {
            dup2(out_pipe[1], STDOUT_FILENO);
            dup2(err_pipe[1], STDERR_FILENO);
            close(out_pipe[0]);
            close(out_pipe[1]);
            close(err_pipe[0]);
            close(err_pipe[1]);

            std::vector<char *> c_argv;
            c_argv.reserve(argv_storage.size() + 1);
            for (auto & s : argv_storage)
                c_argv.push_back(s.data());
            c_argv.push_back(nullptr);
            execv(self_exe.c_str(), c_argv.data());
            _exit(127);
        }

        close(out_pipe[1]);
        close(err_pipe[1]);

        auto read_all = [](int fd) -> std::string
        {
            std::string data;
            char buf[4096];
            for (;;)
            {
                const ssize_t n = read(fd, buf, sizeof(buf));
                if (n <= 0)
                    break;
                data.append(buf, static_cast<size_t>(n));
            }
            close(fd);
            return data;
        };
        res.stdout_text = read_all(out_pipe[0]);
        res.stderr_text = read_all(err_pipe[0]);

        int status = 0;
        waitpid(pid, &status, 0);
        res.elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (WIFEXITED(status))
        {
            res.exit_code = WEXITSTATUS(status);
        }
        else if (WIFSIGNALED(status))
        {
            res.signalled = true;
            res.term_signal = WTERMSIG(status);
            res.exit_code = 128 + res.term_signal;
        }
        (void)work_dir;
        return res;
    }

    static JobResult runJob(
        const std::string & self_exe,
        const JobSpec & spec,
        const std::filesystem::path & work_dir)
    {
        const auto state_dir = work_dir / ("state_" + spec.name);
        auto res = runOneSubprocess(self_exe, buildArgs(spec, state_dir), spec, work_dir);
        std::error_code ec;
        std::filesystem::remove_all(state_dir, ec);

        if (spec.expect_failure)
        {
            res.passed = (res.exit_code != 0);
            return res;
        }

        if (res.exit_code == 0 && res.stdout_text.find("\"verdict\":\"PASS\"") != std::string::npos)
        {
            res.passed = true;
            res.trace_hash = extractJsonString(res.stdout_text, "trace_hash");
            res.executed_events = extractJsonU64(res.stdout_text, "executed_events");
            res.final_commit_index = extractJsonU64(res.stdout_text, "final_commit_index");
            res.final_digest = extractJsonU64(res.stdout_text, "final_digest");
            res.coverage = extractCoverageMap(res.stdout_text);
        }
        return res;
    }

    struct ActiveChild
    {
        pid_t pid = -1;
        size_t job_idx = 0;
        std::filesystem::path state_dir;
        std::filesystem::path out_file;
        std::filesystem::path err_file;
        std::string command;
        std::chrono::steady_clock::time_point started;
    };

    static std::vector<JobResult> runParallel(
        const std::string & self_exe,
        const std::vector<JobSpec> & jobs,
        const std::filesystem::path & work_dir,
        size_t max_workers)
    {
        std::vector<JobResult> results(jobs.size());
        std::map<pid_t, ActiveChild> active;
        size_t next_idx = 0;

        auto launch = [&](size_t idx)
        {
            const auto & spec = jobs[idx];
            const auto state_dir = work_dir / ("state_" + spec.name);
            const auto out_file = work_dir / ("out_" + spec.name + ".log");
            const auto err_file = work_dir / ("err_" + spec.name + ".log");
            auto args = buildArgs(spec, state_dir);

            std::vector<std::string> argv_storage;
            argv_storage.push_back(self_exe);
            for (const auto & a : args)
                argv_storage.push_back(a);
            std::ostringstream cmd_ss;
            for (size_t i = 0; i < argv_storage.size(); ++i)
                cmd_ss << (i ? " " : "") << argv_storage[i];

            const pid_t pid = fork();
            if (pid < 0)
                throw std::runtime_error("SimSwarmRunner: fork() failed");
            if (pid == 0)
            {
                FILE * fo = fopen(out_file.c_str(), "w");
                FILE * fe = fopen(err_file.c_str(), "w");
                if (fo)
                {
                    dup2(fileno(fo), STDOUT_FILENO);
                    fclose(fo);
                }
                if (fe)
                {
                    dup2(fileno(fe), STDERR_FILENO);
                    fclose(fe);
                }
                std::vector<char *> c_argv;
                c_argv.reserve(argv_storage.size() + 1);
                for (auto & s : argv_storage)
                    c_argv.push_back(s.data());
                c_argv.push_back(nullptr);
                execv(self_exe.c_str(), c_argv.data());
                _exit(127);
            }
            active[pid] = ActiveChild{pid, idx, state_dir, out_file, err_file, cmd_ss.str(), std::chrono::steady_clock::now()};
        };

        auto read_file = [](const std::filesystem::path & p) -> std::string
        {
            std::ifstream in(p);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        };

        while (next_idx < jobs.size() || !active.empty())
        {
            while (next_idx < jobs.size() && active.size() < max_workers)
                launch(next_idx++);

            int status = 0;
            const pid_t done_pid = waitpid(-1, &status, 0);
            if (done_pid <= 0)
                break;

            auto it = active.find(done_pid);
            if (it == active.end())
                continue;
            const auto child = it->second;
            active.erase(it);

            JobResult res;
            res.spec = jobs[child.job_idx];
            res.command = child.command;
            res.elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - child.started).count();
            res.stdout_text = read_file(child.out_file);
            res.stderr_text = read_file(child.err_file);
            if (WIFEXITED(status))
            {
                res.exit_code = WEXITSTATUS(status);
            }
            else if (WIFSIGNALED(status))
            {
                res.signalled = true;
                res.term_signal = WTERMSIG(status);
                res.exit_code = 128 + res.term_signal;
            }

            std::error_code ec;
            std::filesystem::remove_all(child.state_dir, ec);
            std::filesystem::remove(child.out_file, ec);
            std::filesystem::remove(child.err_file, ec);

            if (res.spec.expect_failure)
            {
                res.passed = (res.exit_code != 0);
            }
            else if (res.exit_code == 0 && res.stdout_text.find("\"verdict\":\"PASS\"") != std::string::npos)
            {
                res.passed = true;
                res.trace_hash = extractJsonString(res.stdout_text, "trace_hash");
                res.executed_events = extractJsonU64(res.stdout_text, "executed_events");
                res.final_commit_index = extractJsonU64(res.stdout_text, "final_commit_index");
                res.final_digest = extractJsonU64(res.stdout_text, "final_digest");
                res.coverage = extractCoverageMap(res.stdout_text);
            }

            std::cout << "[" << (res.passed ? "PASS" : "FAIL") << "] " << res.spec.name
                      << " (" << std::fixed << std::setprecision(2) << res.elapsed_s << "s)";
            if (res.passed)
                std::cout << " events=" << res.executed_events << " idx=" << res.final_commit_index << " hash=" << res.trace_hash;
            else
                std::cout << " exit=" << res.exit_code << " " << firstLine(res.stderr_text.empty() ? res.stdout_text : res.stderr_text);
            std::cout << '\n' << std::flush;

            results[child.job_idx] = std::move(res);
        }
        return results;
    }
};

} // namespace fdb_sim
