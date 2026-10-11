#pragma once
#include <mfobjects.h>
#include <mferror.h>

namespace glasswall::media::interlace
{
    struct Flag
    {
        HRESULT status{ MF_E_ATTRIBUTENOTFOUND };
        UINT32 value{};
        constexpr bool Present() const noexcept { return status == S_OK; }
        constexpr bool Set() const noexcept { return Present() && value != 0; }
        constexpr bool Valid() const noexcept { return Present() || status == MF_E_ATTRIBUTENOTFOUND; }
    };
    struct SampleFlags
    {
        Flag interlaced, singleField, repeatFirstField, bottomFieldFirst;
    };
    enum class Decision
    {
        Progressive, UnsupportedType, MissingProgressiveProof, Interlaced,
        SingleField, RepeatedField, AttributeError, FormatChanged
    };
    constexpr bool SupportedDeclaration(UINT32 mode) noexcept
    {
        return mode == MFVideoInterlace_Progressive || mode == MFVideoInterlace_MixedInterlaceOrProgressive;
    }
    constexpr Decision Classify(UINT32 decodedMode, SampleFlags const& flags, bool formatChanged = false) noexcept
    {
        // Never apply cached metadata to an unsupported mid-stream type change.
        if (formatChanged) return Decision::FormatChanged;
        if (!flags.interlaced.Valid() || !flags.singleField.Valid() ||
            !flags.repeatFirstField.Valid() || !flags.bottomFieldFirst.Valid()) return Decision::AttributeError;
        if (!SupportedDeclaration(decodedMode) && decodedMode != MFVideoInterlace_Unknown)
            return Decision::UnsupportedType;
        if (flags.singleField.Set()) return Decision::SingleField;
        if (flags.repeatFirstField.Set()) return Decision::RepeatedField;
        if (flags.interlaced.Set()) return Decision::Interlaced;
        // An explicit FALSE describes this decoded frame. Otherwise only an
        // explicitly progressive decoded type supplies the missing flag's value.
        // Mixed/unknown output without the flag is not proof of progressiveness.
        if (flags.interlaced.Present() || decodedMode == MFVideoInterlace_Progressive)
            return Decision::Progressive;
        return Decision::MissingProgressiveProof;
    }
    constexpr wchar_t const* Name(Decision decision) noexcept
    {
        switch (decision)
        {
        case Decision::Progressive: return L"accepted-progressive";
        case Decision::UnsupportedType: return L"rejected-interlaced-or-unsupported-output-type";
        case Decision::MissingProgressiveProof: return L"rejected-missing-progressive-sample-proof";
        case Decision::Interlaced: return L"rejected-interlaced-sample";
        case Decision::SingleField: return L"rejected-single-field-sample";
        case Decision::RepeatedField: return L"rejected-field-repeat-pulldown";
        case Decision::AttributeError: return L"rejected-unreadable-sample-attribute";
        case Decision::FormatChanged: return L"rejected-media-type-change";
        }
        return L"rejected-unknown-decision";
    }
}
