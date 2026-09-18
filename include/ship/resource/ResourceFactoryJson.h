#pragma once

#include "ResourceFactory.h"

namespace Ship {
// Base for factories that read RESOURCE_FORMAT_JSON resources. The loader leaves File::Reader untouched;
// implementations parse file->Buffer (or every archive layer's copy of the path) themselves.
class ResourceFactoryJson : public ResourceFactory {
  protected:
    bool FileHasValidFormatAndReader(std::shared_ptr<File> file, std::shared_ptr<ResourceInitData> initData) override;
};
} // namespace Ship
