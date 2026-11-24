// Tests for npk_core: murmur3 vectors, TOC parsing, decrypt, decode round trips.
#include "npk/codec.hpp"
#include "npk/hash.hpp"

int main()
{
    npk::DecodeContext context; // links lz4 / zstd / libdeflate
    (void)context;
    return 0; // TODO
}
