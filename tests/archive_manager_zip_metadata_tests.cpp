#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ship/resource/File.h"
#include "ship/resource/archive/ArchiveManager.h"
#include "ship/resource/archive/O2rArchive.h"
#include "ship/utils/StrHash64.h"

namespace {

class ZipTestDirectory {
  public:
    ZipTestDirectory() {
        static std::atomic<unsigned> next{ 0 };
        const auto root = std::filesystem::temp_directory_path();
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned i = 0; i < 100; ++i) {
            auto candidate = root / ("lus-zip-metadata-" + std::to_string(stamp) + "-" + std::to_string(next++));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                mPath = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error("Could not create synthetic ZIP fixture directory");
    }
    ~ZipTestDirectory() {
        std::error_code error;
        if (!mPath.empty())
            std::filesystem::remove_all(mPath, error);
    }
    const std::filesystem::path& Path() const {
        return mPath;
    }
    ZipTestDirectory(const ZipTestDirectory&) = delete;
    ZipTestDirectory& operator=(const ZipTestDirectory&) = delete;

  private:
    std::filesystem::path mPath;
};

constexpr const char* kManifest = R"({"name":"SyntheticZip","version":"9.1","code_version":7})";
constexpr const char* kPayload = "ZIP-PAYLOAD";
// These tests check literal sidecar transport/indexing, not ResourceLoader alias decoding.
constexpr const char* kSidecar = R"({"path":"assets/payload.bin"})";

class SyntheticZip {
  public:
    SyntheticZip() : mPath(mDirectory.Path() / "synthetic.o2r") {
        const std::vector<std::pair<std::string, std::string>> entries{ { "manifest.json", kManifest },
                                                                        { "assets/payload.bin", kPayload },
                                                                        { "assets/payload.bin.meta", kSidecar } };
        zip_t* zip = zip_open(mPath.generic_string().c_str(), ZIP_CREATE | ZIP_TRUNCATE, nullptr);
        if (!zip)
            throw std::runtime_error("Could not create synthetic ZIP");
        try {
            for (const auto& [name, bytes] : entries) {
                auto* source = zip_source_buffer(zip, bytes.data(), bytes.size(), 0);
                if (!source)
                    throw std::runtime_error("Could not create ZIP source");
                const auto index = zip_file_add(zip, name.c_str(), source, ZIP_FL_ENC_UTF_8);
                if (index < 0) {
                    zip_source_free(source);
                    throw std::runtime_error("Could not add ZIP entry");
                }
                if (zip_set_file_compression(zip, static_cast<zip_uint64_t>(index), ZIP_CM_STORE, 0) != 0)
                    throw std::runtime_error("Could not store ZIP entry uncompressed");
            }
            if (zip_close(zip) != 0)
                throw std::runtime_error("Could not finish ZIP fixture");
            zip = nullptr;
        } catch (...) {
            if (zip)
                zip_discard(zip);
            throw;
        }
    }
    const std::filesystem::path& Path() const {
        return mPath;
    }

  private:
    ZipTestDirectory mDirectory;
    std::filesystem::path mPath;
};

// Baseline O2r::LoadFile(hash) requires global Context; these fixtures deliberately do not.
// Do not call it by accident before the manager's snapshot implementation is present.
bool RestoreWithSnapshotOwnership(Ship::ArchiveManager& manager, const std::shared_ptr<Ship::Archive>& archive) {
    auto retained = manager.GetArchives();
    if (retained->size() != 1)
        return false;
    if (manager.RemoveArchive(archive) != 1 || !archive->IsLoaded())
        return false;
    return manager.AddArchive(archive) != nullptr;
}

void ExpectBytesForPathAndHash(Ship::ArchiveManager& manager, const char* path, const char* expected) {
    auto byPath = manager.LoadFile(path);
    auto byHash = manager.LoadFile(CRC64(path));
    ASSERT_NE(byPath, nullptr);
    ASSERT_NE(byHash, nullptr);
    ASSERT_TRUE(byPath->IsLoaded);
    ASSERT_TRUE(byHash->IsLoaded);
    ASSERT_NE(byPath->Buffer, nullptr);
    ASSERT_NE(byHash->Buffer, nullptr);
    EXPECT_EQ(*byPath->Buffer, *byHash->Buffer);
    EXPECT_EQ(std::string(byHash->Buffer->begin(), byHash->Buffer->end()), expected);
}

} // namespace

TEST(ArchiveManagerLifetime, RealZipManifestAndPayloadMatchForPathAndHash) {
    SyntheticZip fixture;
    Ship::ArchiveManager manager;
    auto archive = std::make_shared<Ship::O2rArchive>(fixture.Path().generic_string());
    archive->Load();
    ASSERT_TRUE(archive->IsLoaded());
    EXPECT_EQ(archive->GetManifest().Name, "SyntheticZip");
    EXPECT_EQ(archive->GetManifest().Version, "9.1");
    EXPECT_EQ(archive->GetManifest().CodeVersion, 7u);
    EXPECT_FALSE(archive->HasGameVersion());
    ASSERT_NE(manager.AddArchive(archive), nullptr);
    ASSERT_TRUE(RestoreWithSnapshotOwnership(manager, archive))
        << "Snapshot ownership must retain the archive before hash loading";
    ExpectBytesForPathAndHash(manager, "manifest.json", kManifest);
    ExpectBytesForPathAndHash(manager, "assets/payload.bin", kPayload);
}

TEST(ArchiveManagerLifetime, RealZipSidecarAndPayloadKeepDistinctLiteralEntries) {
    SyntheticZip fixture;
    Ship::ArchiveManager manager;
    auto archive = std::make_shared<Ship::O2rArchive>(fixture.Path().generic_string());
    archive->Load();
    ASSERT_TRUE(archive->IsLoaded());
    ASSERT_NE(manager.AddArchive(archive), nullptr);
    ASSERT_TRUE(RestoreWithSnapshotOwnership(manager, archive))
        << "Snapshot ownership must retain the archive before hash loading";
    EXPECT_TRUE(manager.HasFile("assets/payload.bin"));
    EXPECT_TRUE(manager.HasFile("assets/payload.bin.meta"));
    EXPECT_EQ(manager.ListFiles()->size(), 3u);
    ExpectBytesForPathAndHash(manager, "assets/payload.bin", kPayload);
    ExpectBytesForPathAndHash(manager, "assets/payload.bin.meta", kSidecar);
}
