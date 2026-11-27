#pragma once

#include <SimClock.h>

#include <Disks/DiskLocal.h>
#include <Disks/IO/createReadBufferFromFileBase.h>
#include <IO/ReadBufferFromFileDecorator.h>
#include <IO/ReadPipeline.h>
#include <IO/SwapHelper.h>
#include <IO/WriteBufferFromFileDecorator.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace keeper_sim
{

/// FoundationDB AsyncFileNonDurable-inspired crash-consistent simulated disk.
///
/// Wraps ClickHouse's `DB::DiskLocal` with two directories:
///   - `live_dir`: what the running Keeper node reads and writes (OS page cache + disk).
///   - `durable_dir`: what has actually reached stable storage via `sync()` / directory sync.
///
/// On sudden power loss (`simulatePowerLoss`), every file that had unsynced dirty writes
/// since its last `sync()` deterministically undergoes one of three realistic crash outcomes:
///   1. Reverted to its last synced bytes in `durable_dir` (or deleted if newly created unsynced).
///   2. Torn / partial write: a random prefix of the unsynced bytes survives.
///   3. Corrupted unsynced tail: unsynced bytes are partially corrupted (testing CRC/SipHash recovery).
/// Files that completed `sync()` are guaranteed durable and never lost or corrupted.
class SimCrashDisk : public DB::DiskLocal
{
public:
    enum class TransientFaultKind
    {
        None,
        Open,
        PartialWrite,
        SyncError,
        RemoveError,
        ReadError,
        MidReadError,
    };

    struct Stats
    {
        uint64_t files_synced = 0;
        uint64_t power_losses = 0;
        uint64_t dirty_files_reverted = 0;
        uint64_t dirty_files_torn = 0;
        uint64_t dirty_files_corrupted_tail = 0;
        uint64_t transient_faults_triggered = 0;
    };

    SimCrashDisk(
        const std::string & name_,
        const std::filesystem::path & live_dir_,
        const std::filesystem::path & durable_dir_,
        CoverageTracker * coverage_ = nullptr)
        : DB::DiskLocal(name_, live_dir_.string() + "/")
        , live_dir(live_dir_)
        , durable_dir(durable_dir_)
        , coverage(coverage_)
    {
        std::filesystem::create_directories(live_dir);
        std::filesystem::create_directories(durable_dir);
        syncLiveFromDurable();
    }

    void armTransientFault(const std::string & prefix, TransientFaultKind kind)
    {
        armed_fault_prefix = prefix;
        armed_fault_kind = kind;
    }

    void disarmTransientFault()
    {
        armed_fault_prefix.clear();
        armed_fault_kind = TransientFaultKind::None;
    }

    bool hasArmedFault() const
    {
        return armed_fault_kind != TransientFaultKind::None;
    }

    void prepareRead(
        const String & path,
        const DB::ReadSettings & settings,
        std::optional<size_t> read_hint,
        DB::ReadPipeline & pipeline) const override
    {
        auto * self = const_cast<SimCrashDisk *>(this);
        const std::string norm = normalizeRelPath(path);
        if (self->matchesArmedFault(norm, TransientFaultKind::ReadError))
        {
            self->consumeArmedFault();
            throw std::runtime_error("sim_disk: injected read open failure on " + norm);
        }
        if (self->matchesArmedFault(norm, TransientFaultKind::MidReadError))
        {
            self->consumeArmedFault();
            const auto full_path = (live_dir / norm).string();
            DB::StoredObject obj(full_path, full_path, DB::StoredObject::UnknownSize);
            pipeline.setSource(
                [full_path, norm](const DB::StoredObject &, const DB::ReadSettings & rs, bool, bool) -> std::unique_ptr<DB::ReadBufferFromFileBase>
                {
                    auto inner = DB::createReadBufferFromFileBase(full_path, rs);
                    return std::make_unique<SimFailReadBuffer>(std::move(inner), norm);
                },
                DB::StoredObjects{obj},
                settings);
            return;
        }
        DB::DiskLocal::prepareRead(path, settings, read_hint, pipeline);
    }

    std::unique_ptr<DB::WriteBufferFromFileBase> writeFile(
        const String & path, size_t buf_size, DB::WriteMode mode, const DB::WriteSettings & settings) override
    {
        const std::string norm = normalizeRelPath(path);

        if (matchesArmedFault(norm, TransientFaultKind::Open))
        {
            consumeArmedFault();
            throw std::runtime_error("sim_disk: injected open write failure on " + norm);
        }

        const bool inject_partial = matchesArmedFault(norm, TransientFaultKind::PartialWrite);
        const bool inject_sync = matchesArmedFault(norm, TransientFaultKind::SyncError);
        if (inject_partial || inject_sync)
            consumeArmedFault();

        auto inner = DB::DiskLocal::writeFile(path, buf_size, mode, settings);
        markDirty(norm);
        return std::make_unique<SimWriteBuffer>(
            std::move(inner), *this, norm,
            inject_partial ? TransientFaultKind::PartialWrite
                           : (inject_sync ? TransientFaultKind::SyncError : TransientFaultKind::None));
    }

    void createFile(const String & path) override
    {
        DB::DiskLocal::createFile(path);
        markDirty(normalizeRelPath(path));
    }

    void truncateFile(const String & path, size_t size) override
    {
        DB::DiskLocal::truncateFile(path, size);
        markDirty(normalizeRelPath(path));
    }

    void copyFile(
        const String & from_file_path,
        IDisk & to_disk,
        const String & to_file_path,
        const DB::ReadSettings & read_settings,
        const DB::WriteSettings & write_settings = {},
        const std::function<void()> & cancellation_hook = {}) override
    {
        DB::DiskLocal::copyFile(from_file_path, to_disk, to_file_path, read_settings, write_settings, cancellation_hook);
        /// If copying within this disk (e.g. KeeperStateManager backing up `state` -> `state-OLD`),
        /// flush the destination copy to durable storage if the source was already durable so
        /// metadata backup recovery accurately reflects a durable backup before rewriting live `state`.
        if (&to_disk == this)
        {
            const std::string src = normalizeRelPath(from_file_path);
            const std::string dst = normalizeRelPath(to_file_path);
            if (!dirty_files.contains(src))
                persistFileToDurable(dst);
        }
    }

    void moveFile(const String & from_path, const String & to_path) override
    {
        const std::string src = normalizeRelPath(from_path);
        const std::string dst = normalizeRelPath(to_path);
        DB::DiskLocal::moveFile(from_path, to_path);
        const bool was_dirty = dirty_files.erase(src) > 0;
        if (was_dirty)
        {
            dirty_files.insert(dst);
        }
        else
        {
            removeFromDurable(src);
            persistFileToDurable(dst);
        }
    }

    void replaceFile(const String & from_path, const String & to_path) override
    {
        const std::string src = normalizeRelPath(from_path);
        const std::string dst = normalizeRelPath(to_path);
        DB::DiskLocal::replaceFile(from_path, to_path);
        const bool was_dirty = dirty_files.erase(src) > 0;
        if (was_dirty)
        {
            dirty_files.insert(dst);
        }
        else
        {
            removeFromDurable(src);
            persistFileToDurable(dst);
        }
    }

    void removeFile(const String & path) override
    {
        const std::string norm = normalizeRelPath(path);
        if (matchesArmedFault(norm, TransientFaultKind::RemoveError))
        {
            consumeArmedFault();
            throw std::runtime_error("sim_disk: injected remove failure on " + norm);
        }
        DB::DiskLocal::removeFile(path);
        dirty_files.erase(norm);
        removeFromDurable(norm);
    }

    void removeFileIfExists(const String & path) override
    {
        const std::string norm = normalizeRelPath(path);
        if (matchesArmedFault(norm, TransientFaultKind::RemoveError))
        {
            consumeArmedFault();
            throw std::runtime_error("sim_disk: injected removeIfExists failure on " + norm);
        }
        DB::DiskLocal::removeFileIfExists(path);
        dirty_files.erase(norm);
        removeFromDurable(norm);
    }

    void onFileSynced(const std::string & norm_path)
    {
        persistFileToDurable(norm_path);
        dirty_files.erase(norm_path);
        ++stats.files_synced;
    }

    void onFileWritten(const std::string & norm_path)
    {
        dirty_files.insert(norm_path);
    }

    /// Graceful stop: OS page cache drains cleanly to durable storage.
    void simulateGracefulStop()
    {
        for (const auto & rel : dirty_files)
        {
            const auto src = live_dir / rel;
            if (std::filesystem::exists(src))
                persistFileToDurable(rel);
            else
                removeFromDurable(rel);
        }
        dirty_files.clear();
        disarmTransientFault();
    }

    /// Sudden power loss: discards unsynced page cache and optionally tears/corrupts
    /// unsynced byte tails on files that had dirty writes since their last `sync()`.
    size_t simulatePowerLoss(SimRandom & rng, bool allow_torn_writes)
    {
        ++stats.power_losses;
        disarmTransientFault();
        size_t affected = 0;

        /// Snapshot any dirty file contents from `live_dir` before restoring `durable_dir`.
        struct DirtySnapshot
        {
            std::string rel;
            std::string live_bytes;
            bool had_durable = false;
            std::string durable_bytes;
        };
        std::vector<DirtySnapshot> snapshots;
        for (const auto & rel : dirty_files)
        {
            DirtySnapshot snap;
            snap.rel = rel;
            const auto live_path = live_dir / rel;
            const auto dur_path = durable_dir / rel;
            if (std::filesystem::exists(live_path) && std::filesystem::is_regular_file(live_path))
                snap.live_bytes = readAllBytes(live_path);
            if (std::filesystem::exists(dur_path) && std::filesystem::is_regular_file(dur_path))
            {
                snap.had_durable = true;
                snap.durable_bytes = readAllBytes(dur_path);
            }
            if (snap.live_bytes != snap.durable_bytes)
                snapshots.push_back(std::move(snap));
        }

        /// Restore `live_dir` cleanly from `durable_dir`.
        syncLiveFromDurable();
        dirty_files.clear();

        /// Now apply FDB-style unsynced crash outcomes to files that had dirty bytes.
        for (const auto & snap : snapshots)
        {
            ++affected;
            const auto live_path = live_dir / snap.rel;
            const auto dur_path = durable_dir / snap.rel;
            const size_t durable_len = snap.had_durable ? snap.durable_bytes.size() : 0;
            const size_t live_len = snap.live_bytes.size();

            if (!allow_torn_writes || live_len <= durable_len)
            {
                /// Pure lost write: reverted to `durable_dir` (already done above).
                ++stats.dirty_files_reverted;
                if (coverage)
                    coverage->hit("power_loss_reverted_dirty_file");
                continue;
            }

            const int64_t mode = rng.rangeInt(0, 2);
            if (mode == 0)
            {
                /// Mode 0: Complete loss of unsynced append/write (already reverted).
                ++stats.dirty_files_reverted;
                if (coverage)
                    coverage->hit("power_loss_reverted_dirty_file");
            }
            else if (mode == 1)
            {
                /// Mode 1: Torn partial write (durable prefix + random partial prefix of unsynced tail).
                const size_t extra = live_len - durable_len;
                const size_t keep_extra = rng.rangeSize(1, std::max<size_t>(1, extra - 1));
                std::string torn = snap.live_bytes.substr(0, durable_len + keep_extra);
                writeAllBytes(live_path, torn);
                writeAllBytes(dur_path, torn);
                ++stats.dirty_files_torn;
                if (coverage)
                    coverage->hit("power_loss_torn_write");
            }
            else
            {
                /// Mode 2: Torn + corrupted bytes strictly inside the unsynced tail.
                const size_t extra = live_len - durable_len;
                const size_t keep_extra = rng.rangeSize(1, extra);
                std::string torn = snap.live_bytes.substr(0, durable_len + keep_extra);
                const size_t corrupt_pos = durable_len + rng.rangeSize(0, keep_extra - 1);
                torn[corrupt_pos] ^= static_cast<char>(0x5A);
                writeAllBytes(live_path, torn);
                writeAllBytes(dur_path, torn);
                ++stats.dirty_files_corrupted_tail;
                if (coverage)
                    coverage->hit("power_loss_corrupted_unsynced_tail");
            }
        }
        return affected;
    }

    /// Complete disk wipe: simulates replacing a failed machine with a fresh empty disk,
    /// forcing recovery from peers via real Raft log catch-up or network snapshot installation.
    void simulateDiskWipe()
    {
        dirty_files.clear();
        disarmTransientFault();
        clearDirectoryContents(live_dir);
        clearDirectoryContents(durable_dir);
    }

    /// Corrupt or truncate the newest `snapshot_*.bin` on disk ONLY when at least 2 valid snapshots
    /// exist, testing `KeeperSnapshotManager`'s multi-snapshot fallback & recovery on startup!
    bool corruptLatestSnapshotIfMultiple(SimRandom & rng)
    {
        std::vector<std::filesystem::path> snaps;
        if (!std::filesystem::exists(durable_dir))
            return false;
        for (const auto & entry : std::filesystem::directory_iterator(durable_dir))
        {
            if (!entry.is_regular_file())
                continue;
            const auto name = entry.path().filename().string();
            if (name.starts_with("snapshot_") && name.ends_with(".bin"))
                snaps.push_back(entry.path());
        }
        if (snaps.size() < 2)
            return false;
        std::sort(snaps.begin(), snaps.end());
        const auto & newest = snaps.back();
        const auto rel = std::filesystem::relative(newest, durable_dir);
        std::string bytes = readAllBytes(newest);
        if (bytes.size() < 16)
            return false;
        if (rng.chance(0.50))
            bytes.resize(rng.rangeSize(1, bytes.size() / 2));
        else
            bytes[rng.rangeSize(0, bytes.size() - 1)] ^= static_cast<char>(0xFF);
        writeAllBytes(newest, bytes);
        writeAllBytes(live_dir / rel, bytes);
        if (coverage)
            coverage->hit("latest_snapshot_corrupted_for_fallback");
        return true;
    }

    const Stats & getStats() const { return stats; }
    size_t dirtyFileCount() const { return dirty_files.size(); }

private:
    class SimFailReadBuffer : public DB::ReadBufferFromFileDecorator
    {
    public:
        SimFailReadBuffer(std::unique_ptr<DB::ReadBufferFromFileBase> impl_, std::string rel_path_)
            : DB::ReadBufferFromFileDecorator(std::move(impl_))
            , rel_path(std::move(rel_path_))
        {
        }

        bool nextImpl() override
        {
            /// Allow the first read (e.g. 64-byte `FileEncryption::Header`), then fail mid-stream!
            if (++reads_done >= 2)
                throw std::runtime_error("sim_disk: injected mid-stream read failure on " + rel_path);
            return DB::ReadBufferFromFileDecorator::nextImpl();
        }

    private:
        std::string rel_path;
        size_t reads_done = 0;
    };

    class SimWriteBuffer : public DB::WriteBufferFromFileDecorator
    {
    public:
        SimWriteBuffer(
            std::unique_ptr<DB::WriteBuffer> impl_,
            SimCrashDisk & disk_,
            std::string rel_path_,
            TransientFaultKind fault_kind_)
            : DB::WriteBufferFromFileDecorator(std::move(impl_))
            , disk(disk_)
            , rel_path(std::move(rel_path_))
            , fault_kind(fault_kind_)
        {
        }

        void sync() override
        {
            if (fault_kind == TransientFaultKind::SyncError)
            {
                next();
                fault_kind = TransientFaultKind::None;
                throw std::runtime_error("sim_disk: injected sync failure on " + rel_path);
            }
            DB::WriteBufferFromFileDecorator::sync();
            disk.onFileSynced(rel_path);
        }

    private:
        void nextImpl() override
        {
            if (fault_kind == TransientFaultKind::PartialWrite)
            {
                const size_t prefix = std::min<size_t>(13, offset());
                {
                    DB::SwapHelper swap(*this, *impl);
                    impl->position() = impl->buffer().begin() + prefix;
                    impl->next();
                }
                disk.onFileWritten(rel_path);
                fault_kind = TransientFaultKind::None;
                throw std::runtime_error("sim_disk: injected partial write failure on " + rel_path);
            }
            DB::SwapHelper swap(*this, *impl);
            impl->next();
            disk.onFileWritten(rel_path);
        }

        SimCrashDisk & disk;
        std::string rel_path;
        TransientFaultKind fault_kind;
    };

    static std::string normalizeRelPath(const std::string & p)
    {
        std::string out = p;
        while (!out.empty() && out.front() == '/')
            out.erase(out.begin());
        return out;
    }

    bool matchesArmedFault(const std::string & norm_path, TransientFaultKind kind) const
    {
        return armed_fault_kind == kind
            && !armed_fault_prefix.empty()
            && norm_path.starts_with(armed_fault_prefix);
    }

    void consumeArmedFault()
    {
        ++stats.transient_faults_triggered;
        if (coverage)
            coverage->hit("disk_io_fault_injected");
        disarmTransientFault();
    }

    void markDirty(const std::string & norm_path)
    {
        dirty_files.insert(norm_path);
    }

    void persistFileToDurable(const std::string & norm_path)
    {
        const auto src = live_dir / norm_path;
        const auto dst = durable_dir / norm_path;
        if (!std::filesystem::exists(src) || !std::filesystem::is_regular_file(src))
            return;
        std::filesystem::create_directories(dst.parent_path());
        std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing);
    }

    void removeFromDurable(const std::string & norm_path)
    {
        std::error_code ec;
        std::filesystem::remove(durable_dir / norm_path, ec);
    }

    static void clearDirectoryContents(const std::filesystem::path & dir)
    {
        if (!std::filesystem::exists(dir))
            return;
        for (const auto & entry : std::filesystem::directory_iterator(dir))
            std::filesystem::remove_all(entry.path());
    }

    void syncLiveFromDurable()
    {
        clearDirectoryContents(live_dir);
        if (!std::filesystem::exists(durable_dir))
            return;
        for (const auto & entry : std::filesystem::recursive_directory_iterator(durable_dir))
        {
            const auto rel = std::filesystem::relative(entry.path(), durable_dir);
            const auto dst = live_dir / rel;
            if (entry.is_directory())
            {
                std::filesystem::create_directories(dst);
            }
            else if (entry.is_regular_file())
            {
                std::filesystem::create_directories(dst.parent_path());
                std::filesystem::copy_file(entry.path(), dst, std::filesystem::copy_options::overwrite_existing);
            }
        }
    }

    static std::string readAllBytes(const std::filesystem::path & path)
    {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    static void writeAllBytes(const std::filesystem::path & path, const std::string & data)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
    }

    std::filesystem::path live_dir;
    std::filesystem::path durable_dir;
    CoverageTracker * coverage = nullptr;
    std::set<std::string> dirty_files;
    std::string armed_fault_prefix;
    TransientFaultKind armed_fault_kind = TransientFaultKind::None;
    Stats stats;
};

}
