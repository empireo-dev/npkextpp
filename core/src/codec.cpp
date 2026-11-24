#include "npk/codec.hpp"

#include <libdeflate.h>
#include <lz4.h>
#include <zstd.h>

namespace npk {

struct DecodeContext::Impl {
    ZSTD_DCtx* zstd = nullptr;
    libdeflate_decompressor* deflate = nullptr;

    Impl()
        : zstd(ZSTD_createDCtx())
        , deflate(libdeflate_alloc_decompressor())
    {
    }
    ~Impl()
    {
        libdeflate_free_decompressor(deflate);
        ZSTD_freeDCtx(zstd);
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
};

DecodeContext::DecodeContext() : impl_(std::make_unique<Impl>()) {}
DecodeContext::~DecodeContext() = default;
DecodeContext::DecodeContext(DecodeContext&&) noexcept = default;
DecodeContext& DecodeContext::operator=(DecodeContext&&) noexcept = default;

void decrypt(std::span<std::byte> stored) noexcept
{
    (void)stored; // TODO
}

Result<void> decode(std::span<std::byte> stored, const Entry& entry, DecodeContext& context,
                    std::span<std::byte> out, bool verify)
{
    (void)stored;
    (void)entry;
    (void)context;
    (void)out;
    (void)verify;
    return make_error(Errc::not_implemented, "decode");
}

Result<std::vector<std::byte>> read_entry(const Archive& archive, const Entry& entry,
                                          DecodeContext& context, bool verify)
{
    (void)archive;
    (void)entry;
    (void)context;
    (void)verify;
    return make_error(Errc::not_implemented, "read_entry");
}

} // namespace npk
