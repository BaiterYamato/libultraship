#include "ship/resource/archive/ArchiveManager.h"

#include <filesystem>
#include <algorithm>
#include <atomic>
#include <utility>
#include "spdlog/spdlog.h"

#include "ship/resource/archive/Archive.h"
#ifdef INCLUDE_MPQ_SUPPORT
#include "ship/resource/archive/OtrArchive.h"
#endif
#include "ship/resource/archive/O2rArchive.h"
#include "ship/resource/archive/FolderArchive.h"
#include "ship/utils/StringHelper.h"
#include "ship/utils/glob.h"
#include "ship/utils/StrHash64.h"

namespace Ship {

// Manager snapshots retain this block. Unload runs only after the last snapshot,
// outside the VFS mutex; retain the original shared_ptr and its typed deleter.
struct ArchiveManager::ManagedArchiveState {
    std::weak_ptr<Archive> snapshot;
    std::atomic<bool> unloaded{ false };
};

struct ArchiveManager::ManagedArchive {
    explicit ManagedArchive(std::shared_ptr<Archive> value)
        : archive(std::move(value)), state(std::make_shared<ManagedArchiveState>()) {
    }
    ~ManagedArchive() {
        if (managed) {
            try {
                archive->Unload();
            } catch (...) { SPDLOG_WARN("Archive unload threw during deferred retirement"); }
        }
        archive.reset();
        state->unloaded.store(true, std::memory_order_release);
    }
    std::shared_ptr<Archive> archive;
    std::shared_ptr<ManagedArchiveState> state;
    bool managed = false;
};

std::shared_ptr<Archive> ArchiveManager::PrepareArchiveSnapshot(const std::shared_ptr<ManagedArchive>& candidate) {
    // Allocate the control block before acquiring mMutex. Its final release may run Close.
    auto snapshot = std::shared_ptr<Archive>(candidate, candidate->archive.get());
    candidate->state->snapshot = snapshot;
    return snapshot;
}

std::shared_ptr<Archive> ArchiveManager::ManagedArchiveLocked(const std::shared_ptr<Archive>& prepared,
                                                              const std::shared_ptr<ManagedArchive>& candidate) {
    for (auto it = mManagedArchives.begin(); it != mManagedArchives.end();) {
        // unloaded is published only after original ownership was released outside mMutex.
        if (it->second->unloaded.load(std::memory_order_acquire) && it->second->snapshot.expired())
            it = mManagedArchives.erase(it);
        else
            ++it;
    }
    const auto& archive = candidate->archive;
    if (const auto found = mManagedArchives.find(archive.get()); found != mManagedArchives.end()) {
        if (auto retained = found->second->snapshot.lock())
            return retained;
        // An expired weak pointer may still be executing its deleter. Never resurrect it.
        return nullptr;
    }
    if (!archive->IsLoaded() ||
        (!mValidGameVersions.empty() && !mValidGameVersions.contains(archive->GetGameVersion())))
        return nullptr;
    mManagedArchives[archive.get()] = candidate->state;
    candidate->managed = true;
    return prepared;
}

void ArchiveManager::RebuildVirtualFileSystemLocked() {
    mGameVersions.clear();
    mHashes.clear();
    mFileToArchive.clear();
    mDirectories.clear();
    for (size_t i = 0; i < mArchives.size(); ++i) {
        const auto& archive = mArchives[i];
        archive->SetPriority(static_cast<int32_t>(i));
        if (archive->HasGameVersion())
            mGameVersions.push_back(archive->GetGameVersion());
        for (const auto& [hash, filename] : *archive->ListFiles()) {
            mHashes[hash] = filename;
            mFileToArchive[hash] = archive;
            const auto slash = filename.find_last_of('/');
            if (slash != std::string::npos)
                mDirectories.insert(filename.substr(0, slash));
        }
    }
}
ArchiveManager::ArchiveManager() {
}

void ArchiveManager::Init(const std::vector<std::string>& archivePaths) {
    Init(archivePaths, {});
}

void ArchiveManager::Init(const std::vector<std::string>& archivePaths,
                          const std::unordered_set<uint32_t>& validGameVersions) {
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        mValidGameVersions = validGameVersions;
    }
    auto archives = GetArchiveListInPaths(archivePaths);
    for (const auto& archive : archives) {
        AddArchive(archive);
    }
}

ArchiveManager::~ArchiveManager() {
    SPDLOG_TRACE("destruct archive manager");
    SetArchives(nullptr);
}

bool ArchiveManager::IsLoaded() {
    const std::lock_guard<std::mutex> lock(mMutex);
    return !mArchives.empty();
}

std::shared_ptr<File> ArchiveManager::LoadFile(const std::string& filePath) {
    if (filePath == "") {
        return nullptr;
    }

    return LoadFile(CRC64(filePath.c_str()));
}

std::vector<std::shared_ptr<File>> ArchiveManager::LoadFileFromAllLayers(const std::string& filePath) {
    std::vector<std::shared_ptr<File>> layers;
    if (filePath.empty()) {
        return layers;
    }
    const auto archives = GetArchives();
    for (const auto& archive : *archives) {
        if (!archive || !archive->HasFile(filePath)) {
            continue;
        }
        auto file = archive->LoadFile(filePath);
        if (file && file->IsLoaded && file->Buffer) {
            layers.push_back(std::move(file));
        }
    }
    return layers;
}

std::shared_ptr<File> ArchiveManager::LoadFile(uint64_t hash) {
    std::shared_ptr<Archive> archive;
    std::string path;
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        const auto found = mFileToArchive.find(hash);
        const auto name = mHashes.find(hash);
        if (found == mFileToArchive.end() || name == mHashes.end())
            return nullptr;
        archive = found->second;
        path = name->second;
    }
    // Never ask the backend to resolve this hash again after a concurrent removal.
    return archive ? archive->LoadFile(path) : nullptr;
}

bool ArchiveManager::HasFile(const std::string& filePath) {
    return HasFile(CRC64(filePath.c_str()));
}

bool ArchiveManager::HasFile(uint64_t hash) {
    const std::lock_guard<std::mutex> lock(mMutex);
    return mFileToArchive.count(hash) > 0;
}

std::shared_ptr<Archive> ArchiveManager::GetArchiveFromFile(const std::string& filePath) {
    const std::lock_guard<std::mutex> lock(mMutex);
    const auto found = mFileToArchive.find(CRC64(filePath.c_str()));
    return found == mFileToArchive.end() ? nullptr : found->second;
}

int32_t ArchiveManager::GetFilePriority(const std::string& filePath) {
    const std::lock_guard<std::mutex> lock(mMutex);
    auto it = mFileToArchive.find(CRC64(filePath.c_str()));
    if (it == mFileToArchive.end() || it->second == nullptr) {
        return -1;
    }
    return it->second->GetPriority();
}

std::shared_ptr<std::vector<std::string>> ArchiveManager::ListFiles(const std::string& searchMask) {
    std::list<std::string> includes = {};
    if (!searchMask.empty()) {
        includes.push_back(searchMask);
    }
    return ListFiles(includes, {});
}

std::shared_ptr<std::vector<std::string>> ArchiveManager::ListFiles(const std::list<std::string>& includes,
                                                                    const std::list<std::string>& excludes) {
    const std::lock_guard<std::mutex> lock(mMutex);
    auto list = std::make_shared<std::vector<std::string>>();
    for (const auto& [hash, path] : mHashes) {
        if (includes.empty() && excludes.empty()) {
            list->push_back(path);
            continue;
        }
        bool includeMatch = includes.empty();
        if (!includes.empty()) {
            for (std::string filter : includes) {
                if (glob_match(filter.c_str(), path.c_str())) {
                    includeMatch = true;
                    break;
                }
            }
        }
        bool excludeMatch = false;
        if (!excludes.empty()) {
            for (std::string filter : excludes) {
                if (glob_match(filter.c_str(), path.c_str())) {
                    excludeMatch = true;
                    break;
                }
            }
        }
        if (includeMatch && !excludeMatch) {
            list->push_back(path);
        }
    }
    return list;
}

std::shared_ptr<std::vector<std::string>> ArchiveManager::ListDirectories(const std::string& searchMask) {
    const std::lock_guard<std::mutex> lock(mMutex);
    auto list = std::make_shared<std::vector<std::string>>();
    for (const std::string& dir : mDirectories) {
        if (glob_match(searchMask.c_str(), dir.c_str())) {
            list->push_back(dir);
        }
    }
    return list;
}

std::vector<uint32_t> ArchiveManager::GetGameVersions() {
    const std::lock_guard<std::mutex> lock(mMutex);
    return mGameVersions;
}

void ArchiveManager::AddGameVersion(uint32_t newGameVersion) {
    const std::lock_guard<std::mutex> lock(mMutex);
    mGameVersions.push_back(newGameVersion);
}

std::shared_ptr<std::vector<std::shared_ptr<Archive>>> ArchiveManager::GetArchives() {
    const std::lock_guard<std::mutex> lock(mMutex);
    auto archives = std::make_shared<std::vector<std::shared_ptr<Archive>>>();
    for (const auto& archive : mArchives) {
        archives->push_back(archive);
    }
    return archives;
}

void ArchiveManager::ResetVirtualFileSystem() {
    const std::lock_guard<std::mutex> lock(mMutex);
    RebuildVirtualFileSystemLocked();
}

bool ArchiveManager::WriteFile(std::shared_ptr<Archive> archive, const std::string& filePath,
                               const std::vector<uint8_t>& data) {
    if (!archive)
        return false;
    std::shared_ptr<Archive> retained = archive;
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        const auto found = mManagedArchives.find(archive.get());
        if (found != mManagedArchives.end()) {
            if (auto managed = found->second->snapshot.lock())
                retained = std::move(managed);
            else
                return false; // Retiring: do not race its Close with a write.
        }
    }
    if (!retained->WriteFile(filePath, data))
        return false;
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        // Do not republish names if the archive was removed during the write.
        if (std::find(mArchives.begin(), mArchives.end(), retained) != mArchives.end()) {
            retained->IndexFile(filePath);
            const auto hash = CRC64(filePath.c_str());
            mHashes[hash] = filePath;
            mFileToArchive[hash] = retained;
        }
    }
    return true;
}

size_t ArchiveManager::RemoveArchive(const std::string& path) {
    std::shared_ptr<Archive> removed;
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        const auto found = std::find_if(mArchives.begin(), mArchives.end(),
                                        [&](const auto& archive) { return archive->GetPath() == path; });
        if (found == mArchives.end())
            return 0;
        removed = *found;
        mArchives.erase(found);
        RebuildVirtualFileSystemLocked();
    }
    // The last manager snapshot performs Unload, never while holding mMutex.
    return 1;
}

size_t ArchiveManager::RemoveArchive(std::shared_ptr<Archive> archive) {
    return RemoveArchive(archive->GetPath());
}

void ArchiveManager::SetArchives(std::shared_ptr<std::vector<std::shared_ptr<Archive>>> archives) {
    std::vector<std::shared_ptr<ManagedArchive>> candidates;
    std::vector<std::shared_ptr<Archive>> prepared;
    if (archives) {
        candidates.reserve(archives->size());
        prepared.reserve(archives->size());
        for (const auto& archive : *archives) {
            candidates.push_back(archive ? std::make_shared<ManagedArchive>(archive) : nullptr);
            prepared.push_back(archive ? PrepareArchiveSnapshot(candidates.back()) : nullptr);
        }
    }
    // Keep old references and candidate ownership outside the lock, including errors.
    std::vector<std::shared_ptr<Archive>> previous;
    std::vector<std::shared_ptr<Archive>> next;
    std::vector<std::shared_ptr<Archive>> rejected;
    next.reserve(candidates.size());
    rejected.reserve(candidates.size());
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (!candidates[i])
                continue;
            auto retained = ManagedArchiveLocked(prepared[i], candidates[i]);
            if (retained && (!retained->IsLoaded() || (!mValidGameVersions.empty() &&
                                                       !mValidGameVersions.contains(retained->GetGameVersion())))) {
                rejected.push_back(std::move(retained));
                continue;
            }
            if (retained)
                next.push_back(std::move(retained));
        }
        previous.swap(mArchives);
        mArchives.swap(next);
        RebuildVirtualFileSystemLocked();
    }
}

const std::string* ArchiveManager::HashToString(uint64_t hash) const {
    const std::lock_guard<std::mutex> lock(mMutex);
    const auto found = mHashes.find(hash);
    if (found == mHashes.end())
        return nullptr;
    return &*mPathStrings.emplace(found->second).first;
}

const char* ArchiveManager::HashToCString(uint64_t hash) const {
    const std::string* hashStr = HashToString(hash);
    return hashStr != nullptr ? hashStr->c_str() : nullptr;
}

std::vector<std::string> ArchiveManager::GetArchiveListInPaths(const std::vector<std::string>& archivePaths) {
    std::vector<std::string> fileList = {};

    for (const auto& archivePath : archivePaths) {
        if (archivePath.length() > 0) {
            if (std::filesystem::is_directory(archivePath)) {
                bool hasAssetFiles = false;
                for (const auto& p : std::filesystem::directory_iterator(archivePath)) {
                    if (StringHelper::IEquals(p.path().extension().string(), ".otr") ||
                        StringHelper::IEquals(p.path().extension().string(), ".zip") ||
                        StringHelper::IEquals(p.path().extension().string(), ".mpq") ||
                        StringHelper::IEquals(p.path().extension().string(), ".o2r")) {
                        fileList.push_back(std::filesystem::absolute(p).string());
                        hasAssetFiles = true;
                    }
                }

                if (!hasAssetFiles) {
                    fileList.push_back(std::filesystem::absolute(archivePath).string());
                }
            } else if (std::filesystem::is_regular_file(archivePath)) {
                fileList.push_back(std::filesystem::absolute(archivePath).string());
            } else {
                SPDLOG_WARN("The archive at path {} does not exist", std::filesystem::absolute(archivePath).string());
            }
        } else {
            SPDLOG_WARN("No archive path supplied");
        }
    }

    return fileList;
}

std::shared_ptr<Archive> ArchiveManager::AddArchive(const std::string& archivePath) {
    const std::filesystem::path path = archivePath;
    const std::string extension = path.extension().string();
    std::shared_ptr<Archive> archive = nullptr;

    SPDLOG_INFO("Reading archive: {}", path.string());

    if (StringHelper::IEquals(extension, ".o2r") || StringHelper::IEquals(extension, ".zip")) {
        archive = dynamic_pointer_cast<Archive>(std::make_shared<O2rArchive>(archivePath));
#ifdef INCLUDE_MPQ_SUPPORT
    } else if (StringHelper::IEquals(extension, ".otr") || StringHelper::IEquals(extension, ".mpq")) {
        archive = dynamic_pointer_cast<Archive>(std::make_shared<OtrArchive>(archivePath));
#endif
    } else if (StringHelper::IEquals(extension, "")) {
        archive = dynamic_pointer_cast<Archive>(std::make_shared<FolderArchive>(archivePath));
    } else {
        // Not recognized file extension, trying with o2r
        SPDLOG_WARN("File extension \"{}\" not recognized, trying to create an o2r archive.", extension);
        archive = std::make_shared<O2rArchive>(archivePath);
    }

    archive->Load();
    return AddArchive(archive);
}

std::shared_ptr<Archive> ArchiveManager::AddArchive(std::shared_ptr<Archive> archive) {
    if (!archive)
        return nullptr;
    auto candidate = std::make_shared<ManagedArchive>(archive);
    auto prepared = PrepareArchiveSnapshot(candidate);
    std::shared_ptr<Archive> retained;
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        retained = ManagedArchiveLocked(prepared, candidate);
        if (!retained || !retained->IsLoaded() ||
            (!mValidGameVersions.empty() && !mValidGameVersions.contains(retained->GetGameVersion())))
            return nullptr;
        mArchives.push_back(retained);
        RebuildVirtualFileSystemLocked();
    }
    return retained;
}

bool ArchiveManager::IsGameVersionValid(uint32_t gameVersion) {
    const std::lock_guard<std::mutex> lock(mMutex);
    return mValidGameVersions.empty() || mValidGameVersions.contains(gameVersion);
}

#ifdef ENABLE_SCRIPTING
void ArchiveManager::SetUntrustedArchiveHandler(const UntrustedArchiveHandler& handler) {
    const std::lock_guard<std::mutex> lock(mMutex);
    mUntrustedArchiveHandler = handler;
}

UntrustedArchiveHandler ArchiveManager::GetUntrustedArchiveHandler() const {
    const std::lock_guard<std::mutex> lock(mMutex);
    return mUntrustedArchiveHandler;
}
#endif

} // namespace Ship
