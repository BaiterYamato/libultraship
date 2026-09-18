#include "fast/resource/factory/VertexFactory.h"
#include "fast/resource/type/Vertex.h"
#include "spdlog/spdlog.h"
#include "libultraship/libultra/gbi.h"
#include <tinyxml2.h>

namespace Fast {
static void ReadVertexRecords(Ship::BinaryReader& reader, Vertex& vertex, uint32_t count, bool s32Positions) {
    vertex.VertexList.reserve(count);
    // 3 * (s16 | s32) pos + u16 flag + 2 * s16 tc + 4 * u8 colour
    vertex.RecordSize = s32Positions ? 22 : 16;

    for (uint32_t i = 0; i < count; i++) {
        Vtx data;
        if (s32Positions) {
            data.v.ob[0] = reader.ReadInt32();
            data.v.ob[1] = reader.ReadInt32();
            data.v.ob[2] = reader.ReadInt32();
        } else {
            data.v.ob[0] = reader.ReadInt16();
            data.v.ob[1] = reader.ReadInt16();
            data.v.ob[2] = reader.ReadInt16();
        }
        data.v.flag = reader.ReadUInt16();
        data.v.tc[0] = reader.ReadInt16();
        data.v.tc[1] = reader.ReadInt16();
        data.v.cn[0] = reader.ReadUByte();
        data.v.cn[1] = reader.ReadUByte();
        data.v.cn[2] = reader.ReadUByte();
        data.v.cn[3] = reader.ReadUByte();
        vertex.VertexList.push_back(data);
    }
}

std::shared_ptr<Vertex> ReadVertexResource(Ship::BinaryReader& reader, std::shared_ptr<Ship::ResourceInitData> initData,
                                           uint32_t count, bool s32Positions) {
    auto vertex = std::make_shared<Vertex>(initData);
    ReadVertexRecords(reader, *vertex, count, s32Positions);
    return vertex;
}

static std::shared_ptr<Ship::IResource> ReadVertexFile(std::shared_ptr<Ship::File> file,
                                                       std::shared_ptr<Ship::ResourceInitData> initData,
                                                       bool s32Positions) {
    auto reader = std::get<std::shared_ptr<Ship::BinaryReader>>(file->Reader);
    uint32_t count = reader->ReadUInt32();
    return ReadVertexResource(*reader, initData, count, s32Positions);
}

std::shared_ptr<Ship::IResource>
ResourceFactoryBinaryVertexV0::ReadResource(std::shared_ptr<Ship::File> file,
                                            std::shared_ptr<Ship::ResourceInitData> initData) {
    if (!FileHasValidFormatAndReader(file, initData)) {
        return nullptr;
    }
    return ReadVertexFile(file, initData, false);
}

std::shared_ptr<Ship::IResource>
ResourceFactoryBinaryVertexV1::ReadResource(std::shared_ptr<Ship::File> file,
                                            std::shared_ptr<Ship::ResourceInitData> initData) {
    if (!FileHasValidFormatAndReader(file, initData)) {
        return nullptr;
    }
    return ReadVertexFile(file, initData, true);
}

std::shared_ptr<Ship::IResource>
ResourceFactoryXMLVertexV0::ReadResource(std::shared_ptr<Ship::File> file,
                                         std::shared_ptr<Ship::ResourceInitData> initData) {
    if (!FileHasValidFormatAndReader(file, initData)) {
        return nullptr;
    }

    auto vertex = std::make_shared<Vertex>(initData);

    auto child =
        std::get<std::shared_ptr<tinyxml2::XMLDocument>>(file->Reader)->FirstChildElement()->FirstChildElement();

    while (child != nullptr) {
        std::string childName = child->Name();

        if (childName == "Vtx") {
            Vtx data;
            data.v.ob[0] = child->IntAttribute("X");
            data.v.ob[1] = child->IntAttribute("Y");
            data.v.ob[2] = child->IntAttribute("Z");
            data.v.flag = 0;
            data.v.tc[0] = child->IntAttribute("S");
            data.v.tc[1] = child->IntAttribute("T");
            data.v.cn[0] = child->IntAttribute("R");
            data.v.cn[1] = child->IntAttribute("G");
            data.v.cn[2] = child->IntAttribute("B");
            data.v.cn[3] = child->IntAttribute("A");

            vertex->VertexList.push_back(data);
        }

        child = child->NextSiblingElement();
    }

    return vertex;
}
} // namespace Fast
