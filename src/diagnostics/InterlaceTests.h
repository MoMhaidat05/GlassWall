#pragma once
#include "../media/mf/InterlacePolicy.h"
#include <array>
#include <ostream>

namespace glasswall::diagnostics
{
    // Test the engine's actual decision policy using decoded-sample attribute
    // results, including absent/wrong-type flags. No MF decoder or GPU is run.
    inline bool RunInterlaceTests(std::wostream& output)
    {
        using namespace media::interlace;
        constexpr Flag absent{};
        constexpr Flag no{ S_OK, FALSE }, yes{ S_OK, TRUE };
        struct Case
        {
            wchar_t const* name;
            UINT32 decodedMode;
            SampleFlags flags;
            Decision expected;
            bool formatChanged{};
        };
        constexpr std::array cases{
            Case{ L"progressive type, implicit progressive sample", MFVideoInterlace_Progressive, {}, Decision::Progressive },
            Case{ L"progressive type, explicit FALSE", MFVideoInterlace_Progressive, { no, absent, absent, absent }, Decision::Progressive },
            Case{ L"mixed type, progressive sample", MFVideoInterlace_MixedInterlaceOrProgressive, { no, no, no, no }, Decision::Progressive },
            Case{ L"mixed type, progressive sample, optional fields absent", MFVideoInterlace_MixedInterlaceOrProgressive, { no, absent, absent, absent }, Decision::Progressive },
            Case{ L"mixed type, progressive sample, bottom-first metadata", MFVideoInterlace_MixedInterlaceOrProgressive, { no, no, no, yes }, Decision::Progressive },
            Case{ L"mixed type, missing interlace flag", MFVideoInterlace_MixedInterlaceOrProgressive, {}, Decision::MissingProgressiveProof },
            Case{ L"mixed type, actual interlaced sample", MFVideoInterlace_MixedInterlaceOrProgressive, { yes, no, no, no }, Decision::Interlaced },
            Case{ L"progressive type, contradictory interlaced sample", MFVideoInterlace_Progressive, { yes, absent, absent, absent }, Decision::Interlaced },
            Case{ L"nonzero BOOL interlace value", MFVideoInterlace_MixedInterlaceOrProgressive, { { S_OK, 2 }, no, no, no }, Decision::Interlaced },
            Case{ L"single-field sample contradicts progressive flag", MFVideoInterlace_MixedInterlaceOrProgressive, { no, yes, no, no }, Decision::SingleField },
            Case{ L"field repeat/pulldown requires unsupported timing", MFVideoInterlace_MixedInterlaceOrProgressive, { no, no, yes, no }, Decision::RepeatedField },
            Case{ L"unknown output with explicit progressive proof", MFVideoInterlace_Unknown, { no, no, no, absent }, Decision::Progressive },
            Case{ L"unknown output without proof", MFVideoInterlace_Unknown, {}, Decision::MissingProgressiveProof },
            Case{ L"wrong-type interlace attribute", MFVideoInterlace_MixedInterlaceOrProgressive, { { MF_E_INVALIDTYPE, 0 }, no, no, no }, Decision::AttributeError },
            Case{ L"unreadable single-field attribute", MFVideoInterlace_Progressive, { no, { E_FAIL, 0 }, absent, absent }, Decision::AttributeError },
            Case{ L"unreadable repeat attribute", MFVideoInterlace_Progressive, { no, no, { MF_E_INVALIDTYPE, 0 }, absent }, Decision::AttributeError },
            Case{ L"unreadable bottom-first attribute", MFVideoInterlace_MixedInterlaceOrProgressive, { no, no, no, { E_FAIL, 0 } }, Decision::AttributeError },
            Case{ L"mid-stream type change cannot use cached progressive type", MFVideoInterlace_Progressive, { no, no, no, absent }, Decision::FormatChanged, true }
        };
        for (auto const& test : cases)
        {
            auto actual = Classify(test.decodedMode, test.flags, test.formatChanged);
            if (actual != test.expected)
            {
                output << L"Interlace policy FAILED: " << test.name << L"; expected " << Name(test.expected)
                    << L", got " << Name(actual) << L"\n";
                return false;
            }
        }
        constexpr std::array fixedModes{ MFVideoInterlace_FieldInterleavedUpperFirst,
            MFVideoInterlace_FieldInterleavedLowerFirst, MFVideoInterlace_FieldSingleUpper, MFVideoInterlace_FieldSingleLower };
        for (auto mode : fixedModes)
        {
            auto decodedMode = static_cast<UINT32>(mode);
            if (SupportedDeclaration(decodedMode) || Classify(decodedMode, {}) != Decision::UnsupportedType ||
                Classify(decodedMode, { no, no, no, no }) != Decision::UnsupportedType)
            { output << L"Interlace policy FAILED: fixed interlaced/single-field output was accepted.\n"; return false; }
        }
        // A mixed compressed declaration must not contaminate a progressive
        // decoded type; neither a first good sample nor field-order metadata
        // can authorize a later interlaced sample.
        if (!SupportedDeclaration(MFVideoInterlace_Progressive) ||
            !SupportedDeclaration(MFVideoInterlace_MixedInterlaceOrProgressive) ||
            SupportedDeclaration(MFVideoInterlace_Unknown) || SupportedDeclaration(999) ||
            Classify(999, { no, no, no, no }) != Decision::UnsupportedType ||
            Classify(MFVideoInterlace_Progressive, {}) != Decision::Progressive ||
            Classify(MFVideoInterlace_MixedInterlaceOrProgressive, { no, no, no, no }) != Decision::Progressive ||
            Classify(MFVideoInterlace_MixedInterlaceOrProgressive, { yes, no, no, yes }) != Decision::Interlaced)
        { output << L"Interlace policy FAILED: compressed/decoded declaration or sample transition regression.\n"; return false; }
        output << L"Interlace policy regression checks passed. Decoder, hardware acceleration and Windows playback were not tested.\n";
        return true;
    }
}
