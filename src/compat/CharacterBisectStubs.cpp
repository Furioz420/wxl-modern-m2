// The friend's character-customization diagnostics can optionally narrow a
// model draw to individual triangles. The current v1.1 M2 draw path does not
// expose that diagnostic surface; keep the panel linkable without changing the
// production draw hook. These no-op answers do not participate in character
// rendering or customization decisions.
#include "../ExtensionApi.hpp"

namespace wxl_modern_m2
{
    uint32_t BisectTriangleCount() { return 0; }
    bool BisectArmed() { return false; }
    void SetBisectArmed(bool) {}
    void BisectRange(uint32_t& first, uint32_t& count) { first = 0; count = 0; }
    void SetBisectRange(uint32_t, uint32_t) {}
    void RequestBisectReport() {}
}
