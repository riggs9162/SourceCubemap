#include <scm/common.hpp>
#include <compressonator.h>
#include <codec.h>

namespace scm {
void compressDxt1(std::span<const std::byte> rgba, uint32_t size, std::byte* destination) {
    std::unique_ptr<CCodec> codec(CreateCodec(CT_DXT1));
    require(codec != nullptr, "Cannot create the DXT1 codec.");
    codec->SetParameter("UseAdaptiveWeighting", CMP_DWORD(0)); codec->SetParameter("DXT1UseAlpha", CMP_DWORD(0)); codec->SetParameter("AlphaThreshold", CMP_DWORD(0));
    codec->SetParameter("RefineSteps", CMP_DWORD(0)); codec->SetParameter("Quality", CODECFLOAT(1)); codec->SetParameter("CompressionSpeed", CMP_DWORD(CMP_Speed_Normal));
    auto* source = const_cast<CMP_BYTE*>(reinterpret_cast<const CMP_BYTE*>(rgba.data()));
    std::unique_ptr<CCodecBuffer> input(CreateCodecBuffer(CBT_RGBA8888, 0, 0, 0, size, size, 0, source, static_cast<CMP_DWORD>(rgba.size())));
    std::unique_ptr<CCodecBuffer> output(codec->CreateBuffer(0, 0, 0, size, size, 0, reinterpret_cast<CMP_BYTE*>(destination), (size / 4) * (size / 4) * 8));
    require(input != nullptr && output != nullptr, "Cannot create the DXT1 compression buffers.");
    input->SetFormat(CMP_FORMAT_RGBA_8888); output->SetFormat(CMP_FORMAT_DXT1); input->m_bSwizzle = true;
    require(codec->Compress(*input, *output) == CE_OK, "DXT1 compression failed.");
}
}
