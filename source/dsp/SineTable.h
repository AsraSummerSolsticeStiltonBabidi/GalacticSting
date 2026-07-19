#pragma once

//==============================================================================
// Shared, read-only sine lookup table with linear interpolation. Additive
// synthesis evaluates a sine per partial per sample, so a table is much cheaper
// than std::sin. Phase is kept normalised in [0, 1).
//==============================================================================

#include <array>
#include <cmath>

namespace spectrum
{
    class SineTable
    {
    public:
        static constexpr int size = 8192;

        SineTable()
        {
            for (int i = 0; i <= size; ++i)
                table[(size_t) i] = (float) std::sin (6.283185307179586 * i / size);
        }

        // p must be in [0, 1)
        inline float lookup (double p) const noexcept
        {
            const double x = p * size;
            const int    i = (int) x;
            const float  f = (float) (x - i);
            return table[(size_t) i] + f * (table[(size_t) i + 1] - table[(size_t) i]);
        }

    private:
        std::array<float, size + 1> table {};
    };

    inline const SineTable& sineTable()
    {
        static const SineTable t;
        return t;
    }
}
