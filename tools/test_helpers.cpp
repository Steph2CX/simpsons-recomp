// Verifie les helpers SIMDe ajoutes a ppc_context.h contre une reference scalaire.
#include <cstdio>
#include <cmath>
#include <climits>
#include <ppc_context.h>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf(__VA_ARGS__); } } while (0)

int main()
{
    const int32_t s[] = { 0, 1, -1, INT_MAX, INT_MIN, INT_MAX - 5, INT_MIN + 5, 123456789, -987654321, 65535, 65536, 70000 };
    const int n = sizeof(s) / sizeof(s[0]);

    for (int i = 0; i < n; i++)
    for (int j = 0; j < n; j++)
    {
        alignas(16) int32_t a[4] = { s[i], s[j], s[(i + j) % n], s[(i * 3 + j) % n] };
        alignas(16) int32_t b[4] = { s[j], s[i], s[(i * 7 + j) % n], s[(i + j * 5) % n] };
        alignas(16) int32_t r[4];
        alignas(16) uint16_t r16[8];
        simde__m128i va = simde_mm_load_si128((simde__m128i*)a), vb = simde_mm_load_si128((simde__m128i*)b);

        simde_mm_store_si128((simde__m128i*)r, simde_mm_adds_epi32(va, vb));
        for (int k = 0; k < 4; k++)
        {
            int64_t e = int64_t(a[k]) + b[k];
            e = e > INT_MAX ? INT_MAX : e < INT_MIN ? INT_MIN : e;
            CHECK(r[k] == e, "adds_epi32 %d+%d -> %d (attendu %lld)\n", a[k], b[k], r[k], (long long)e);
        }

        simde_mm_store_si128((simde__m128i*)r, simde_mm_cmpgt_epu32(va, vb));
        for (int k = 0; k < 4; k++)
            CHECK(r[k] == (uint32_t(a[k]) > uint32_t(b[k]) ? -1 : 0), "cmpgt_epu32 %u>%u\n", a[k], b[k]);

        simde_mm_store_si128((simde__m128i*)r16, simde_mm_packus_epu32(va, vb));
        for (int k = 0; k < 8; k++)
        {
            uint32_t x = uint32_t(k < 4 ? a[k] : b[k - 4]);
            CHECK(r16[k] == (x > 0xFFFF ? 0xFFFF : x), "packus_epu32 %u -> %u\n", x, r16[k]);
        }
    }

    const float f[] = { 0.0f, -0.0f, 1.5f, -1.5f, 2147483520.0f, 2147483648.0f, 3000000000.0f, 4294967040.0f, 4294967296.0f, 1e20f, -1e20f, INFINITY, -INFINITY, NAN, 65535.9f };
    const int nf = sizeof(f) / sizeof(f[0]);
    for (int i = 0; i < nf; i += 4)
    {
        alignas(16) float in[4];
        alignas(16) uint32_t out[4];
        for (int k = 0; k < 4; k++)
            in[k] = f[(i + k) % nf];
        simde_mm_store_si128((simde__m128i*)out, simde_mm_vctuxs(simde_mm_load_ps(in)));
        for (int k = 0; k < 4; k++)
        {
            double x = in[k];
            uint32_t e = std::isnan(x) || x <= 0 ? 0 : x >= 4294967296.0 ? 0xFFFFFFFF : uint32_t(x);
            CHECK(out[k] == e, "vctuxs %g -> %u (attendu %u)\n", x, out[k], e);
        }
    }

    printf(failures ? "%d ECHECS\n" : "tous les tests passent\n", failures);
    return failures != 0;
}
