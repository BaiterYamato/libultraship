#include "ship/resource/ResourceFactoryJson.h"
#include "spdlog/spdlog.h"

namespace Ship {
bool ResourceFactoryJson::FileHasValidFormatAndReader(std::shared_ptr<File> file,
                                                      std::shared_ptr<ResourceInitData> initData) {
    if (initData->Format != RESOURCE_FORMAT_JSON) {
        SPDLOG_ERROR("resource file format does not match factory format.");
        return false;
    }
    if (file == nullptr || file->Buffer == nullptr || file->Buffer->empty()) {
        SPDLOG_ERROR("Failed to load resource: JSON file is empty ({})", initData->Path);
        return false;
    }
    return true;
}
} // namespace Ship
