#pragma once

#include <nvsdk_ngx_params.h>
#include <algorithm>
#include <cstring>

class DlssgParameterSanitizer : public NVSDK_NGX_Parameter
{
  private:
    NVSDK_NGX_Parameter* _inner = nullptr;
    bool _filterOutputReal = false;
    unsigned int _forcedWidth = 0;
    unsigned int _forcedHeight = 0;

    static bool IsOutputRealKey(const char* name)
    {
        return name != nullptr && std::strcmp(name, "DLSSG.OutputReal") == 0;
    }

    static bool IsWidthKey(const char* name)
    {
        return name != nullptr && (std::strcmp(name, "Width") == 0 || std::strcmp(name, "OutWidth") == 0);
    }

    static bool IsHeightKey(const char* name)
    {
        return name != nullptr && (std::strcmp(name, "Height") == 0 || std::strcmp(name, "OutHeight") == 0);
    }

  public:
    DlssgParameterSanitizer(NVSDK_NGX_Parameter* inner, bool filterOutputReal, unsigned int forcedWidth = 0,
                            unsigned int forcedHeight = 0)
        : _inner(inner), _filterOutputReal(filterOutputReal), _forcedWidth(forcedWidth), _forcedHeight(forcedHeight)
    {
    }

    bool IsFilteringOutputReal() const { return _filterOutputReal; }
    unsigned int GetForcedWidth() const { return _forcedWidth; }
    unsigned int GetForcedHeight() const { return _forcedHeight; }
    NVSDK_NGX_Parameter* GetInner() const { return _inner; }

    void Set(const char* InName, unsigned long long InValue) override
    {
        if (_inner != nullptr)
            _inner->Set(InName, InValue);
    }

    void Set(const char* InName, float InValue) override
    {
        if (_inner != nullptr)
            _inner->Set(InName, InValue);
    }

    void Set(const char* InName, double InValue) override
    {
        if (_inner != nullptr)
            _inner->Set(InName, InValue);
    }

    void Set(const char* InName, unsigned int InValue) override
    {
        if (_inner != nullptr)
            _inner->Set(InName, InValue);
    }

    void Set(const char* InName, int InValue) override
    {
        if (_inner != nullptr)
            _inner->Set(InName, InValue);
    }

    void Set(const char* InName, ID3D11Resource* InValue) override
    {
        if (_inner != nullptr)
            _inner->Set(InName, InValue);
    }

    void Set(const char* InName, ID3D12Resource* InValue) override
    {
        if (_inner != nullptr)
            _inner->Set(InName, InValue);
    }

    void Set(const char* InName, void* InValue) override
    {
        if (_inner != nullptr)
            _inner->Set(InName, InValue);
    }

    NVSDK_NGX_Result Get(const char* InName, unsigned long long* OutValue) const override
    {
        if (_inner == nullptr)
            return NVSDK_NGX_Result_Fail;

        if (_filterOutputReal && IsOutputRealKey(InName))
            return NVSDK_NGX_Result_FAIL_FeatureNotFound;

        if (_forcedWidth > 0 && IsWidthKey(InName))
        {
            if (OutValue != nullptr)
                *OutValue = static_cast<unsigned long long>(_forcedWidth);
            return NVSDK_NGX_Result_Success;
        }

        if (_forcedHeight > 0 && IsHeightKey(InName))
        {
            if (OutValue != nullptr)
                *OutValue = static_cast<unsigned long long>(_forcedHeight);
            return NVSDK_NGX_Result_Success;
        }

        return _inner->Get(InName, OutValue);
    }

    NVSDK_NGX_Result Get(const char* InName, float* OutValue) const override
    {
        if (_inner == nullptr)
            return NVSDK_NGX_Result_Fail;

        if (_forcedWidth > 0 && IsWidthKey(InName))
        {
            if (OutValue != nullptr)
                *OutValue = static_cast<float>(_forcedWidth);
            return NVSDK_NGX_Result_Success;
        }

        if (_forcedHeight > 0 && IsHeightKey(InName))
        {
            if (OutValue != nullptr)
                *OutValue = static_cast<float>(_forcedHeight);
            return NVSDK_NGX_Result_Success;
        }

        return _inner->Get(InName, OutValue);
    }

    NVSDK_NGX_Result Get(const char* InName, double* OutValue) const override
    {
        if (_inner == nullptr)
            return NVSDK_NGX_Result_Fail;

        if (_forcedWidth > 0 && IsWidthKey(InName))
        {
            if (OutValue != nullptr)
                *OutValue = static_cast<double>(_forcedWidth);
            return NVSDK_NGX_Result_Success;
        }

        if (_forcedHeight > 0 && IsHeightKey(InName))
        {
            if (OutValue != nullptr)
                *OutValue = static_cast<double>(_forcedHeight);
            return NVSDK_NGX_Result_Success;
        }

        return _inner->Get(InName, OutValue);
    }

    NVSDK_NGX_Result Get(const char* InName, unsigned int* OutValue) const override
    {
        if (_inner == nullptr)
            return NVSDK_NGX_Result_Fail;

        if (_forcedWidth > 0 && IsWidthKey(InName))
        {
            if (OutValue != nullptr)
                *OutValue = _forcedWidth;
            return NVSDK_NGX_Result_Success;
        }

        if (_forcedHeight > 0 && IsHeightKey(InName))
        {
            if (OutValue != nullptr)
                *OutValue = _forcedHeight;
            return NVSDK_NGX_Result_Success;
        }

        return _inner->Get(InName, OutValue);
    }

    NVSDK_NGX_Result Get(const char* InName, int* OutValue) const override
    {
        if (_inner == nullptr)
            return NVSDK_NGX_Result_Fail;

        if (_forcedWidth > 0 && IsWidthKey(InName))
        {
            if (OutValue != nullptr)
                *OutValue = static_cast<int>(_forcedWidth);
            return NVSDK_NGX_Result_Success;
        }

        if (_forcedHeight > 0 && IsHeightKey(InName))
        {
            if (OutValue != nullptr)
                *OutValue = static_cast<int>(_forcedHeight);
            return NVSDK_NGX_Result_Success;
        }

        return _inner->Get(InName, OutValue);
    }

    NVSDK_NGX_Result Get(const char* InName, ID3D11Resource** OutValue) const override
    {
        if (_inner == nullptr)
            return NVSDK_NGX_Result_Fail;

        return _inner->Get(InName, OutValue);
    }

    NVSDK_NGX_Result Get(const char* InName, ID3D12Resource** OutValue) const override
    {
        if (_inner == nullptr)
            return NVSDK_NGX_Result_Fail;

        if (_filterOutputReal && IsOutputRealKey(InName))
            return NVSDK_NGX_Result_FAIL_FeatureNotFound;

        return _inner->Get(InName, OutValue);
    }

    NVSDK_NGX_Result Get(const char* InName, void** OutValue) const override
    {
        if (_inner == nullptr)
            return NVSDK_NGX_Result_Fail;

        if (_filterOutputReal && IsOutputRealKey(InName))
            return NVSDK_NGX_Result_FAIL_FeatureNotFound;

        return _inner->Get(InName, OutValue);
    }

    void Reset() override
    {
        if (_inner != nullptr)
            _inner->Reset();
    }
};

inline bool ShouldFilterDlssgOutputReal(NVSDK_NGX_Parameter* InParameters)
{
    if (InParameters == nullptr)
        return false;

    ID3D12Resource* d3d12Resource = nullptr;
    if (InParameters->Get("DLSSG.OutputReal", &d3d12Resource) == NVSDK_NGX_Result_Success)
        return d3d12Resource == nullptr;

    void* voidPtr = nullptr;
    if (InParameters->Get("DLSSG.OutputReal", &voidPtr) == NVSDK_NGX_Result_Success)
        return voidPtr == nullptr;

    return false;
}

struct DlssgSanitizationParams
{
    bool filterOutputReal = false;
    unsigned int forcedWidth = 0;
    unsigned int forcedHeight = 0;

    bool NeedsSanitization() const { return filterOutputReal || forcedWidth > 0 || forcedHeight > 0; }
};

inline DlssgSanitizationParams ResolveDlssgSanitization(NVSDK_NGX_Parameter* InParameters,
                                                        unsigned int fallbackDisplayWidth = 0,
                                                        unsigned int fallbackDisplayHeight = 0)
{
    DlssgSanitizationParams result {};
    if (InParameters == nullptr)
        return result;

    result.filterOutputReal = ShouldFilterDlssgOutputReal(InParameters);

    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int outWidth = 0;
    unsigned int outHeight = 0;

    InParameters->Get("Width", &width);
    InParameters->Get("Height", &height);
    InParameters->Get("OutWidth", &outWidth);
    InParameters->Get("OutHeight", &outHeight);

    const unsigned int targetWidth = std::max(fallbackDisplayWidth, outWidth);
    const unsigned int targetHeight = std::max(fallbackDisplayHeight, outHeight);

    if (targetWidth > 0 && targetHeight > 0)
    {
        // If current Width/Height are smaller than target display dimensions,
        // it indicates upscaler render resolution leaked into the shared parameter map.
        if (width == 0 || width < targetWidth)
            result.forcedWidth = targetWidth;
        if (height == 0 || height < targetHeight)
            result.forcedHeight = targetHeight;
    }

    return result;
}
