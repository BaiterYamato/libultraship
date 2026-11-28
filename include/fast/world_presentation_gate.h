#pragma once

#include <cstdint>

namespace Fast {

// Prova fornecida pelo produtor da lista, sem percorrer recursos/branches do F3D.
// Não possui estado "consumido" por replay: a lista é a mesma na interpolação.
template <typename Command> class WorldPresentationGate {
  public:
    void Begin(const void* root) {
        mRoot = root;
        mMarker = nullptr;
        mInvalid = false;
    }

    void Publish(const void* root, const void* marker) {
        if (!mRoot || root != mRoot || !marker || mMarker) {
            mInvalid = true;
            return;
        }
        mMarker = static_cast<const Command*>(marker);
    }

    bool Authorize(const void* root, bool debugging, std::uintptr_t expectedW0) const {
        // Não ler o ponteiro antigo quando o lote terminou, mudou ou foi invalidado.
        return !debugging && !mInvalid && mRoot && root == mRoot && mMarker && mMarker->words.w0 == expectedW0 &&
               mMarker->words.w1 == 0;
    }

    void End() {
        Begin(nullptr);
    }

  private:
    const void* mRoot = nullptr;
    const Command* mMarker = nullptr;
    bool mInvalid = false;
};

} // namespace Fast
