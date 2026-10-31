#pragma once

#include "ship/resource/Resource.h"
#include "ship/resource/ResourceFactoryBinary.h"
#include "ship/resource/ResourceFactoryXML.h"

namespace Ship {
class BinaryReader;
}

namespace Fast {
class Vertex;

// Builds a Vertex resource from `count` records at the reader's position and sets its RecordSize: the
// vanilla 16-byte record (s16 positions) or the 22-byte v1 record (s32 positions, otherwise identical).
// Shared by every factory that carries vertices - the Vertex resource here and the game's generic Array
// resource - so the two encodings are defined once, and so a caller never needs the full Vtx type (whose
// libultra header defines s16/u16 as macros on some platforms).
std::shared_ptr<Vertex> ReadVertexResource(Ship::BinaryReader& reader, std::shared_ptr<Ship::ResourceInitData> initData,
                                           uint32_t count, bool s32Positions);

class ResourceFactoryBinaryVertexV0 final : public Ship::ResourceFactoryBinary {
  public:
    std::shared_ptr<Ship::IResource> ReadResource(std::shared_ptr<Ship::File> file,
                                                  std::shared_ptr<Ship::ResourceInitData> initData) override;
};

// v1: s32 positions, so a room mesh can leave the +/-32767 range. Identical to v0 otherwise.
class ResourceFactoryBinaryVertexV1 final : public Ship::ResourceFactoryBinary {
  public:
    std::shared_ptr<Ship::IResource> ReadResource(std::shared_ptr<Ship::File> file,
                                                  std::shared_ptr<Ship::ResourceInitData> initData) override;
};

class ResourceFactoryXMLVertexV0 final : public Ship::ResourceFactoryXML {
  public:
    std::shared_ptr<Ship::IResource> ReadResource(std::shared_ptr<Ship::File> file,
                                                  std::shared_ptr<Ship::ResourceInitData> initData) override;
};
} // namespace Fast
