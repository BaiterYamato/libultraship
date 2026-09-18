#include "fast/resource/factory/MatrixFactory.h"
#include "fast/resource/type/Matrix.h"
#include "spdlog/spdlog.h"

namespace Fast {
std::shared_ptr<Ship::IResource>
ResourceFactoryBinaryMatrixV0::ReadResource(std::shared_ptr<Ship::File> file,
                                            std::shared_ptr<Ship::ResourceInitData> initData) {
    if (!FileHasValidFormatAndReader(file, initData)) {
        return nullptr;
    }

    auto matrix = std::make_shared<Matrix>(initData);
    auto reader = std::get<std::shared_ptr<Ship::BinaryReader>>(file->Reader);

#if defined(GBI_FLOATS)
    for (size_t i = 0; i < 4; i++) {
        for (size_t j = 0; j < 4; j++) {
            matrix->Matrx.mf[i][j] = reader->ReadFloat();
        }
    }
#elif defined(GBI_FLOAT_MTX)
    // Stored data is the N64 s16.16 layout (16 int-part words then 16 frac-part words); unpack to float.
    int32_t words[16];
    for (size_t i = 0; i < 16; i++) {
        words[i] = reader->ReadInt32();
    }
    for (size_t i = 0; i < 4; i++) {
        for (size_t j = 0; j < 4; j += 2) {
            int32_t intPart = words[i * 2 + j / 2];
            uint32_t fracPart = (uint32_t)words[8 + i * 2 + j / 2];
            matrix->Matrx.mf[i][j] = (int32_t)((intPart & 0xffff0000) | (fracPart >> 16)) / 65536.0f;
            matrix->Matrx.mf[i][j + 1] = (int32_t)((intPart << 16) | (fracPart & 0xffff)) / 65536.0f;
        }
    }
#else
    for (size_t i = 0; i < 4; i++) {
        for (size_t j = 0; j < 4; j++) {
            matrix->Matrx.m[i][j] = reader->ReadInt32();
        }
    }
#endif

    return matrix;
}
} // namespace Fast
