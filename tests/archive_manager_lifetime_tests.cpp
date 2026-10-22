#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ship/resource/File.h"
#include "ship/resource/archive/Archive.h"
#include "ship/resource/archive/ArchiveManager.h"
#include "ship/resource/archive/FolderArchive.h"
#include "ship/utils/StrHash64.h"

namespace {

using namespace std::chrono_literals;
using Files = std::unordered_map<std::string, std::string>;

struct LifecycleCounts {
    std::atomic<int> opens{ 0 }, closes{ 0 }, activeReads{ 0 }, closesDuringRead{ 0 }, hashReads{ 0 };
};

// Uses the real Archive::Load/Unload and the manifest pattern from TestRamArchive.
// Close only records lifecycle: the baseline RED must not free memory used by a reader.
class LifetimeRamArchive final : public Ship::Archive {
  public:
    LifetimeRamArchive(const std::string& path, Files files) : Ship::Archive(path), mFiles(std::move(files)) {
    }

    std::shared_ptr<LifecycleCounts> counts = std::make_shared<LifecycleCounts>();
    std::function<void()> onRead, onClose;

    bool Open() override {
        ++counts->opens;
        for (const auto& [path, content] : mFiles) {
            (void)content;
            IndexFile(path);
        }
        return true;
    }

    bool Close() override {
        ++counts->closes;
        if (counts->activeReads.load() != 0)
            ++counts->closesDuringRead;
        if (onClose)
            onClose();
        return true;
    }

    bool WriteFile(const std::string& path, const std::vector<uint8_t>& data) override {
        mFiles[path] = std::string(data.begin(), data.end());
        // Like FolderArchive, this backend leaves index publication to the manager.
        return true;
    }

    std::shared_ptr<Ship::File> LoadFile(const std::string& path) override {
        if (path == "manifest.json") {
            return MakeFile(R"({"name":"LifetimeRamArchive","code_version":1})");
        }
        const auto found = mFiles.find(path);
        if (found == mFiles.end())
            return nullptr;
        const std::string content = found->second;
        ++counts->activeReads;
        struct Exit {
            LifecycleCounts& counts;
            ~Exit() {
                --counts.activeReads;
            }
        } exit{ *counts };
        {
            const std::lock_guard<std::mutex> lock(mPathMutex);
            mLastPath = path;
        }
        if (onRead)
            onRead();
        return MakeFile(content);
    }

    std::shared_ptr<Ship::File> LoadFile(uint64_t hash) override {
        ++counts->hashReads;
        // Resolve locally, avoiding Context even when testing the unpatched baseline.
        for (const auto& [path, content] : mFiles) {
            (void)content;
            if (CRC64(path.c_str()) == hash)
                return LoadFile(path);
        }
        return nullptr;
    }

    std::string LastPath() const {
        const std::lock_guard<std::mutex> lock(mPathMutex);
        return mLastPath;
    }

  private:
    Files mFiles;
    mutable std::mutex mPathMutex;
    std::string mLastPath;

    static std::shared_ptr<Ship::File> MakeFile(const std::string& content) {
        auto file = std::make_shared<Ship::File>();
        file->Buffer = std::make_shared<std::vector<char>>(content.begin(), content.end());
        file->IsLoaded = true;
        return file;
    }
};

std::shared_ptr<LifetimeRamArchive> LoadedRamArchive(const std::string& path, Files files) {
    auto archive = std::make_shared<LifetimeRamArchive>(path, std::move(files));
    archive->Load();
    EXPECT_TRUE(archive->IsLoaded());
    EXPECT_EQ(archive->GetManifest().Name, "LifetimeRamArchive");
    return archive;
}

// Clears stack-capturing callbacks before the manager can close a remaining archive.
class ResetHooks {
  public:
    explicit ResetHooks(const std::shared_ptr<LifetimeRamArchive>& archive) : mArchive(archive) {
    }
    ~ResetHooks() {
        if (auto archive = mArchive.lock())
            archive->onRead = archive->onClose = {};
    }

  private:
    std::weak_ptr<LifetimeRamArchive> mArchive;
};

class ReadGate {
  public:
    void EnterAndWait() {
        std::unique_lock<std::mutex> lock(mMutex);
        mEntered = true;
        mChanged.notify_all();
        if (!mChanged.wait_for(lock, 2s, [&] { return mReleased; }))
            mTimedOut = true;
    }
    bool WaitUntilEntered() {
        std::unique_lock<std::mutex> lock(mMutex);
        return mChanged.wait_for(lock, 2s, [&] { return mEntered; });
    }
    void Release() {
        const std::lock_guard<std::mutex> lock(mMutex);
        mReleased = true;
        mChanged.notify_all();
    }
    bool TimedOut() const {
        return mTimedOut.load();
    }

  private:
    std::mutex mMutex;
    std::condition_variable mChanged;
    bool mEntered = false, mReleased = false;
    std::atomic<bool> mTimedOut{ false };
};

// Fatal assertions still release the barrier and join before destroying the manager/gate.
class ScopedReader {
  public:
    ScopedReader(ReadGate& gate, std::function<std::shared_ptr<Ship::File>()> read)
        : mGate(gate), mResult(mPromise.get_future()), mThread([this, read = std::move(read)] {
              try {
                  mPromise.set_value(read());
              } catch (...) { mPromise.set_exception(std::current_exception()); }
          }) {
    }
    ~ScopedReader() {
        mGate.Release();
        if (mThread.joinable())
            mThread.join();
    }
    std::shared_ptr<Ship::File> Finish() {
        mGate.Release();
        auto result = mResult.get();
        mThread.join();
        return result;
    }
    ScopedReader(const ScopedReader&) = delete;
    ScopedReader& operator=(const ScopedReader&) = delete;

  private:
    ReadGate& mGate;
    std::promise<std::shared_ptr<Ship::File>> mPromise;
    std::future<std::shared_ptr<Ship::File>> mResult;
    std::thread mThread;
};

// Safely gate candidate-only cases: never start a deliberate map race or dereference
// a removed borrowed string when running the unpatched baseline for RED.
bool SupportsSnapshotRetirement() {
    Ship::ArchiveManager manager;
    auto archive = LoadedRamArchive("ram://capability", { { "capability/a", "A" } });
    if (!manager.AddArchive(archive))
        return false;
    auto view = manager.GetArchives();
    if (manager.RemoveArchive(archive) != 1)
        return false;
    const bool retained = archive->counts->closes.load() == 0;
    view.reset();
    return retained && archive->counts->closes.load() == 1;
}

class TemporaryDirectory {
  public:
    TemporaryDirectory() {
        static std::atomic<unsigned> sequence{ 0 };
        const auto root = std::filesystem::temp_directory_path();
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            auto path = root / ("lus-archive-lifetime-" + std::to_string(stamp) + "-" + std::to_string(sequence++));
            std::error_code error;
            if (std::filesystem::create_directory(path, error)) {
                mPath = std::move(path);
                return;
            }
        }
        throw std::runtime_error("Could not create isolated archive test directory");
    }
    ~TemporaryDirectory() {
        std::error_code error;
        if (!mPath.empty())
            std::filesystem::remove_all(mPath, error);
    }
    const std::filesystem::path& Path() const {
        return mPath;
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  private:
    std::filesystem::path mPath;
};

} // namespace

TEST(ArchiveManagerLifetime, SurvivorRemainsOpenDuringOtherRemoval) {
    Ship::ArchiveManager manager;
    auto keep = LoadedRamArchive("ram://keep", { { "keep/a", "A" } });
    auto drop = LoadedRamArchive("ram://drop", { { "drop/a", "B" } });
    ResetHooks reset(keep);
    ASSERT_NE(manager.AddArchive(keep), nullptr);
    ASSERT_NE(manager.AddArchive(drop), nullptr);
    ReadGate gate;
    keep->onRead = [&] { gate.EnterAndWait(); };
    ScopedReader reader(gate, [&] { return manager.LoadFile("keep/a"); });
    ASSERT_TRUE(gate.WaitUntilEntered());
    EXPECT_EQ(manager.RemoveArchive(drop), 1u);
    EXPECT_EQ(keep->counts->opens.load(), 1);
    EXPECT_EQ(keep->counts->closes.load(), 0);
    EXPECT_EQ(keep->counts->closesDuringRead.load(), 0);
    EXPECT_NE(reader.Finish(), nullptr);
    EXPECT_FALSE(gate.TimedOut());
}

TEST(ArchiveManagerLifetime, RemovedArchiveClosesAfterReaderAndExportedSnapshot) {
    Ship::ArchiveManager manager;
    auto archive = LoadedRamArchive("ram://target", { { "target/a", "A" } });
    ResetHooks reset(archive);
    ASSERT_NE(manager.AddArchive(archive), nullptr);
    auto view = manager.GetArchives();
    ReadGate gate;
    archive->onRead = [&] { gate.EnterAndWait(); };
    ScopedReader reader(gate, [&] { return manager.LoadFile("target/a"); });
    ASSERT_TRUE(gate.WaitUntilEntered());
    EXPECT_EQ(manager.RemoveArchive(archive), 1u);
    EXPECT_FALSE(manager.HasFile("target/a"));
    EXPECT_EQ(archive->counts->closes.load(), 0);
    EXPECT_NE(reader.Finish(), nullptr);
    EXPECT_FALSE(gate.TimedOut());
    EXPECT_EQ(archive->counts->closes.load(), 0);
    view.reset();
    EXPECT_EQ(archive->counts->closes.load(), 1);
    EXPECT_EQ(archive->counts->closesDuringRead.load(), 0);
}

TEST(ArchiveManagerLifetime, ReadAndCloseCallbacksCanReenterManager) {
    Ship::ArchiveManager manager;
    auto archive = LoadedRamArchive("ram://self", { { "self/a", "A" } });
    ResetHooks reset(archive);
    ASSERT_NE(manager.AddArchive(archive), nullptr);
    archive->onRead = [&] {
        EXPECT_TRUE(manager.HasFile("self/a"));
        EXPECT_EQ(manager.RemoveArchive(archive), 1u);
        EXPECT_EQ(archive->counts->closes.load(), 0);
    };
    archive->onClose = [&] {
        EXPECT_FALSE(manager.HasFile("self/a"));
        EXPECT_TRUE(manager.GetArchives()->empty());
    };
    EXPECT_NE(manager.LoadFile("self/a"), nullptr);
    EXPECT_EQ(archive->counts->closes.load(), 1);
    EXPECT_EQ(archive->counts->closesDuringRead.load(), 0);
}

TEST(ArchiveManagerLifetime, CopiedPathWriteIndexAndBorrowedNamesSurviveRebuild) {
    Ship::ArchiveManager manager;
    auto archive = LoadedRamArchive("ram://paths", { { "paths/a", "A" } });
    auto other = LoadedRamArchive("ram://other", { { "other/a", "B" } });
    ASSERT_NE(manager.AddArchive(archive), nullptr);
    ASSERT_NE(manager.AddArchive(other), nullptr);
    ASSERT_NE(manager.LoadFile(CRC64("paths/a")), nullptr);
    EXPECT_EQ(archive->LastPath(), "paths/a");
    EXPECT_EQ(archive->counts->hashReads.load(), 0);
    EXPECT_TRUE(manager.WriteFile(archive, "paths/new", { 'n', 'e', 'w' }));
    EXPECT_EQ(manager.RemoveArchive(other), 1u);
    EXPECT_TRUE(manager.HasFile("paths/new"));
    auto file = manager.LoadFile("paths/new");
    ASSERT_NE(file, nullptr);
    ASSERT_NE(file->Buffer, nullptr);
    EXPECT_EQ(std::string(file->Buffer->begin(), file->Buffer->end()), "new");
    if (!SupportsSnapshotRetirement())
        return;
    const auto* name = manager.HashToString(CRC64("paths/a"));
    const auto* cName = manager.HashToCString(CRC64("paths/a"));
    ASSERT_NE(name, nullptr);
    ASSERT_NE(cName, nullptr);
    EXPECT_EQ(manager.RemoveArchive(archive), 1u);
    EXPECT_EQ(manager.HashToString(CRC64("paths/a")), nullptr);
    EXPECT_EQ(*name, "paths/a");
    EXPECT_STREQ(cName, "paths/a");
}

TEST(ArchiveManagerLifetime, SameObjectAndSetArchivesRetainSingleOwnership) {
    Ship::ArchiveManager manager;
    auto archive = LoadedRamArchive("ram://same", { { "same/a", "A" } });
    ASSERT_NE(manager.AddArchive(archive), nullptr);
    ASSERT_NE(manager.AddArchive(archive), nullptr);
    EXPECT_EQ(manager.RemoveArchive(archive), 1u);
    EXPECT_EQ(archive->counts->closes.load(), 0);
    auto view = manager.GetArchives();
    manager.SetArchives(view);
    EXPECT_EQ(archive->counts->opens.load(), 1);
    EXPECT_EQ(archive->counts->closes.load(), 0);
    EXPECT_EQ(manager.RemoveArchive(archive), 1u);
    EXPECT_EQ(archive->counts->closes.load(), 0);
    view.reset();
    EXPECT_EQ(archive->counts->closes.load(), 1);
}

TEST(ArchiveManagerLifetime, RejectsReinsertDuringCloseAndAllowsRecovery) {
    if (!SupportsSnapshotRetirement())
        GTEST_SKIP() << "Requires deferred retirement; baseline RED is covered above";
    Ship::ArchiveManager manager;
    auto archive = LoadedRamArchive("ram://closing", { { "closing/a", "A" } });
    ResetHooks reset(archive);
    ASSERT_NE(manager.AddArchive(archive), nullptr);
    archive->onClose = [&] {
        EXPECT_EQ(manager.AddArchive(archive), nullptr);
        EXPECT_FALSE(manager.HasFile("closing/a"));
    };
    EXPECT_EQ(manager.RemoveArchive(archive), 1u);
    EXPECT_EQ(archive->counts->closes.load(), 1);
    archive->Load();
    ASSERT_NE(manager.AddArchive(archive), nullptr);
    EXPECT_EQ(manager.RemoveArchive(archive), 1u);
    EXPECT_EQ(archive->counts->closes.load(), 2);
    std::weak_ptr<LifetimeRamArchive> original = archive;
    archive.reset();
    EXPECT_TRUE(original.expired());
}

TEST(ArchiveManagerLifetime, ConcurrentSnapshotsUseFreshArchiveObjects) {
    if (!SupportsSnapshotRetirement())
        GTEST_SKIP() << "Never run deliberate map races on the baseline";
    Ship::ArchiveManager manager;
    std::promise<void> started;
    auto ready = started.get_future();
    std::atomic<bool> failed{ false };
    std::jthread reader([&](std::stop_token stop) {
        bool announced = false;
        try {
            while (!stop.stop_requested()) {
                manager.LoadFile("race/a");
                manager.HasFile("race/a");
                manager.HashToString(CRC64("race/a"));
                manager.GetArchives();
                if (!announced) {
                    started.set_value();
                    announced = true;
                }
            }
        } catch (...) {
            failed = true;
            if (!announced)
                started.set_value();
        }
    });
    ASSERT_EQ(ready.wait_for(2s), std::future_status::ready);
    for (int i = 0; i < 200; ++i) {
        auto archive = LoadedRamArchive("ram://race", { { "race/a", "A" } });
        ASSERT_NE(manager.AddArchive(archive), nullptr);
        EXPECT_EQ(manager.RemoveArchive(archive), 1u);
    }
    reader.request_stop();
    reader.join();
    EXPECT_FALSE(failed.load());
}

TEST(ArchiveManagerLifetime, RealFolderWriteRemainsVisibleAfterOtherRemoval) {
    TemporaryDirectory temporary;
    const auto root = temporary.Path() / "folder";
    ASSERT_TRUE(std::filesystem::create_directory(root));
    {
        std::ofstream file(root / "initial.bin", std::ios::binary);
        file << "initial";
        ASSERT_TRUE(file.good());
    }
    Ship::ArchiveManager manager;
    auto folder = std::make_shared<Ship::FolderArchive>(root.generic_string());
    folder->Load();
    ASSERT_TRUE(folder->IsLoaded());
    ASSERT_NE(manager.AddArchive(folder), nullptr);
    auto other = LoadedRamArchive("ram://folder-other", { { "other/a", "B" } });
    ASSERT_NE(manager.AddArchive(other), nullptr);
    ASSERT_TRUE(manager.WriteFile(folder, "created.bin", { 'n', 'e', 'w' }));
    EXPECT_EQ(manager.RemoveArchive(other), 1u);
    EXPECT_TRUE(manager.HasFile("created.bin"));
    auto file = manager.LoadFile("created.bin");
    ASSERT_NE(file, nullptr);
    ASSERT_NE(file->Buffer, nullptr);
    EXPECT_EQ(std::string(file->Buffer->begin(), file->Buffer->end()), "new");
}
