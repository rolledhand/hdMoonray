// Copyright 2023-2024 DreamWorks Animation LLC
// SPDX-License-Identifier: Apache-2.0

#include "Material.h"
#include "RenderDelegate.h"
#include "ValueConverter.h"
#include "CoordSys.h"
#include "HdmLog.h"

#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/matrix4f.h>
#include <pxr/base/gf/matrix4d.h>
#include <pxr/imaging/hd/rprim.h>

// nb, must use full name scene_rdl2::rdl2::Material...
#include <scene_rdl2/scene/rdl2/Layer.h>
#include <scene_rdl2/scene/rdl2/Material.h>
#include <scene_rdl2/scene/rdl2/Displacement.h>
#include <scene_rdl2/scene/rdl2/VolumeShader.h>

#include <scene_rdl2/render/logging/logging.h>
#include <iostream>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <unordered_set>
#include <vector>

using namespace scene_rdl2::rdl2;
using namespace scene_rdl2::math;

#ifdef __GNUC__
#define UNUSED __attribute__ ((unused))
#else
#define UNUSED
#endif


using scene_rdl2::logging::Logger;

const pxr::TfToken projectorToken("projector");
const pxr::TfToken proj_camToken("proj_cam");
const pxr::TfToken moonraySurfaceTerminalToken("moonray:surface");

bool
endsWith(const std::string& value, const std::string& suffix)
{
    return value.size() >= suffix.size() &&
        value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool
startsWith(const std::string& value, const std::string& prefix)
{
    return value.size() >= prefix.size() &&
        value.compare(0, prefix.size(), prefix) == 0;
}

std::string
toLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool
isUsdUVTextureIdentifier(const pxr::TfToken& identifier)
{
    return toLower(identifier.GetString()) == "usduvtexture";
}

bool
isMaterialXStandardSurfaceIdentifier(const pxr::TfToken& identifier)
{
    const std::string idLower = toLower(identifier.GetString());
    return idLower == "mtlxstandard_surface" ||
           idLower.find("standard_surface") != std::string::npos;
}

bool
isMaterialXImageIdentifier(const pxr::TfToken& identifier)
{
    const std::string idLower = toLower(identifier.GetString());
    return idLower == "mtlximage" ||
           startsWith(idLower, "nd_image_") ||
           idLower.find("nd_image_") != std::string::npos;
}

bool
isMaterialXTiledImageIdentifier(const pxr::TfToken& identifier)
{
    const std::string idLower = toLower(identifier.GetString());
    return idLower == "mtlxtiledimage" ||
           startsWith(idLower, "nd_tiledimage_") ||
           idLower.find("nd_tiledimage_") != std::string::npos;
}

bool
isMaterialXImageOrTiledIdentifier(const pxr::TfToken& identifier)
{
    return isMaterialXImageIdentifier(identifier) ||
           isMaterialXTiledImageIdentifier(identifier);
}

bool
isUsdTransform2dIdentifier(const pxr::TfToken& identifier)
{
    const std::string idLower = toLower(identifier.GetString());
    return idLower == "usdtransform2d" ||
           idLower.find("transform2d") != std::string::npos;
}

bool
isMaterialXPrimvarUtilityIdentifier(const pxr::TfToken& identifier)
{
    const std::string idLower = toLower(identifier.GetString());
    return idLower == "usdprimvarreader_float2" ||
           idLower.find("primvarreader_float2") != std::string::npos ||
           idLower.find("texcoord") != std::string::npos ||
           idLower.find("geompropvalue") != std::string::npos ||
           idLower.find("geomprop") != std::string::npos;
}

bool
looksLikeImageIdentifier(const pxr::TfToken& identifier)
{
    const std::string idLower = toLower(identifier.GetString());
    return idLower.find("image") != std::string::npos ||
           idLower.find("uvtexture") != std::string::npos;
}

bool
looksLikeUnsupportedMaterialXUtilityIdentifier(const pxr::TfToken& identifier)
{
    const std::string idLower = toLower(identifier.GetString());
    return idLower.find("texcoord") != std::string::npos ||
           idLower.find("geomprop") != std::string::npos ||
           idLower.find("place2d") != std::string::npos ||
           idLower.find("transform2d") != std::string::npos;
}

enum class MappingQuality
{
    Exact,
    Approximate,
    Unsupported
};

enum class MappingMode
{
    Copy,
    BaseColorWithFactor,
    EmissionWithFactor,
    OpacityToPresence,
    BoolToThinGeometry,
    DispersionToUseDispersion,
    Unsupported
};

struct StandardSurfaceMappingRule
{
    const char* inputName;
    const char* outputName;
    MappingQuality quality;
    MappingMode mode;
    const char* note;
};

struct MappingStats
{
    int exact = 0;
    int approximate = 0;
    int unsupported = 0;
};

void
warnMaterialXLossyOnce(const pxr::SdfPath& nodePath,
                       const std::string& key,
                       const std::string& message)
{
    static std::unordered_set<std::string> seen;
    const std::string fullKey = nodePath.GetString() + "|" + key;
    if (seen.insert(fullKey).second) {
        Logger::warn(nodePath, ": ", message);
    }
}

const std::array<StandardSurfaceMappingRule, 42>&
getStandardSurfaceMappingRules()
{
    static const std::array<StandardSurfaceMappingRule, 42> kRules = {{
        {"base", nullptr, MappingQuality::Exact, MappingMode::BaseColorWithFactor, "multiplies base_color into albedo"},
        {"base_color", nullptr, MappingQuality::Exact, MappingMode::BaseColorWithFactor, "multiplies base factor into albedo"},
        {"diffuse_roughness", "diffuse_roughness", MappingQuality::Exact, MappingMode::Copy, ""},
        {"metalness", "metallic", MappingQuality::Exact, MappingMode::Copy, ""},
        {"specular", "specular", MappingQuality::Exact, MappingMode::Copy, ""},
        {"specular_color", "primary_specular_tint", MappingQuality::Approximate, MappingMode::Copy, "mapped to primary specular tint"},
        {"specular_roughness", "roughness", MappingQuality::Approximate, MappingMode::Copy, "mapped to global roughness"},
        {"specular_IOR", "refractive_index", MappingQuality::Approximate, MappingMode::Copy, "mapped to refractive_index"},
        {"specular_anisotropy", "anisotropy", MappingQuality::Approximate, MappingMode::Copy, "mapped to Dwa anisotropy"},
        {"specular_rotation", nullptr, MappingQuality::Unsupported, MappingMode::Unsupported, "no direct DwaBase equivalent"},
        {"transmission", "transmission", MappingQuality::Exact, MappingMode::Copy, ""},
        {"transmission_color", "transmission_color", MappingQuality::Exact, MappingMode::Copy, ""},
        {"transmission_depth", nullptr, MappingQuality::Unsupported, MappingMode::Unsupported, "no direct DwaBase equivalent"},
        {"transmission_scatter", nullptr, MappingQuality::Unsupported, MappingMode::Unsupported, "no direct DwaBase equivalent"},
        {"transmission_scatter_anisotropy", nullptr, MappingQuality::Unsupported, MappingMode::Unsupported, "no direct DwaBase equivalent"},
        {"transmission_dispersion", "use_dispersion", MappingQuality::Approximate, MappingMode::DispersionToUseDispersion, "mapped to bool use_dispersion"},
        {"transmission_extra_roughness", "transmission_azimuthal_roughness", MappingQuality::Approximate, MappingMode::Copy, "mapped to transmission_azimuthal_roughness"},
        {"subsurface", "subsurface_blend", MappingQuality::Approximate, MappingMode::Copy, "mapped to subsurface_blend"},
        {"subsurface_color", "scattering_color", MappingQuality::Approximate, MappingMode::Copy, "mapped to scattering_color"},
        {"subsurface_radius", "scattering_radius", MappingQuality::Approximate, MappingMode::Copy, "mapped to scattering_radius"},
        {"subsurface_scale", nullptr, MappingQuality::Unsupported, MappingMode::Unsupported, "no direct DwaBase equivalent"},
        {"subsurface_anisotropy", nullptr, MappingQuality::Unsupported, MappingMode::Unsupported, "no direct DwaBase equivalent"},
        {"sheen", "fuzz", MappingQuality::Approximate, MappingMode::Copy, "mapped to fuzz"},
        {"sheen_color", "fuzz_albedo", MappingQuality::Approximate, MappingMode::Copy, "mapped to fuzz_albedo"},
        {"sheen_roughness", "fuzz_roughness", MappingQuality::Approximate, MappingMode::Copy, "mapped to fuzz_roughness"},
        {"coat", "clearcoat", MappingQuality::Approximate, MappingMode::Copy, "mapped to clearcoat"},
        {"coat_color", "clearcoat_attenuation_color", MappingQuality::Approximate, MappingMode::Copy, "mapped to clearcoat attenuation"},
        {"coat_roughness", "clearcoat_roughness", MappingQuality::Approximate, MappingMode::Copy, "mapped to clearcoat_roughness"},
        {"coat_anisotropy", nullptr, MappingQuality::Unsupported, MappingMode::Unsupported, "no direct DwaBase equivalent"},
        {"coat_rotation", nullptr, MappingQuality::Unsupported, MappingMode::Unsupported, "no direct DwaBase equivalent"},
        {"coat_IOR", "clearcoat_refractive_index", MappingQuality::Approximate, MappingMode::Copy, "mapped to clearcoat_refractive_index"},
        {"coat_normal", nullptr, MappingQuality::Unsupported, MappingMode::Unsupported, "no direct DwaBase equivalent"},
        {"coat_affect_color", nullptr, MappingQuality::Unsupported, MappingMode::Unsupported, "no direct DwaBase equivalent"},
        {"coat_affect_roughness", nullptr, MappingQuality::Unsupported, MappingMode::Unsupported, "no direct DwaBase equivalent"},
        {"thin_film_thickness", "iridescence_thickness", MappingQuality::Approximate, MappingMode::Copy, "mapped to iridescence thickness"},
        {"thin_film_IOR", nullptr, MappingQuality::Unsupported, MappingMode::Unsupported, "no direct DwaBase equivalent"},
        {"emission", nullptr, MappingQuality::Exact, MappingMode::EmissionWithFactor, "multiplies emission_color into emission"},
        {"emission_color", nullptr, MappingQuality::Exact, MappingMode::EmissionWithFactor, "multiplies emission factor into emission"},
        {"opacity", "presence", MappingQuality::Approximate, MappingMode::OpacityToPresence, "averaged to scalar presence"},
        {"thin_walled", "thin_geometry", MappingQuality::Approximate, MappingMode::BoolToThinGeometry, "mapped to thin_geometry"},
        {"normal", "input_normal", MappingQuality::Approximate, MappingMode::Copy, "mapped to input_normal"},
        {"tangent", "shading_tangent", MappingQuality::Approximate, MappingMode::Copy, "mapped to shading_tangent"},
    }};
    return kRules;
}

bool
isNativeMoonrayTextureMaterialIdentifier(const pxr::TfToken& identifier)
{
    const std::string id = identifier.GetString();
    return id == "DwaBaseMaterial" || id == "ImageMap";
}

std::string
normalizeNativeTextureMaterialClassName(const pxr::TfToken& identifier)
{
    const std::string raw = identifier.GetString();
    const std::string lower = toLower(raw);

    auto matchAny = [&lower](const std::initializer_list<const char*>& names) {
        for (const char* name : names) {
            if (lower == name) {
                return true;
            }
        }
        return false;
    };

    if (matchAny({"dwabasematerial", "moonray:dwabasematerial", "moonray_dwabasematerial",
                  "moonraydwabasematerial"})) {
        return "DwaBaseMaterial";
    }
    if (matchAny({"imagemap", "moonray:imagemap", "moonray_imagemap", "moonrayimagemap"})) {
        return "ImageMap";
    }
    return raw;
}

bool
isTruthEnvEnabled(const char* name)
{
    const char* value = std::getenv(name);
    if (!value) {
        return false;
    }
    const std::string lower = toLower(std::string(value));
    return lower == "1" || lower == "true" || lower == "yes" || lower == "on";
}

void
debugLogSelectedParams(const pxr::SdfPath& nodePath,
                       const char* label,
                       const std::map<pxr::TfToken, pxr::VtValue>& params,
                       const std::initializer_list<pxr::TfToken>& keys)
{
    std::string summary;
    bool first = true;
    for (const pxr::TfToken& key : keys) {
        if (!first) {
            summary += ", ";
        }
        first = false;
        summary += key.GetString();
        summary += '=';
        summary += (params.find(key) != params.end()) ? "Y" : "N";
    }
    Logger::debug(nodePath, ": ", label, " authored params: ", summary);
}

bool
shouldDumpMaterialNetworkMap()
{
    return isTruthEnvEnabled("HDMOONRAY_MTLX_DUMP_NETWORK");
}

bool
shouldTraceNativePath()
{
    return isTruthEnvEnabled("HDMOONRAY_NATIVE_TRACE");
}

bool
shouldStrictMoonraySurface()
{
    return isTruthEnvEnabled("HDMOONRAY_STRICT_MOONRAY_SURFACE");
}

bool
extractStringValue(const pxr::VtValue& value, std::string& out)
{
    if (value.IsHolding<std::string>()) {
        out = value.UncheckedGet<std::string>();
        return true;
    }
    if (value.IsHolding<pxr::TfToken>()) {
        out = value.UncheckedGet<pxr::TfToken>().GetString();
        return true;
    }
    if (value.IsHolding<pxr::SdfAssetPath>()) {
        const pxr::SdfAssetPath& assetPath = value.UncheckedGet<pxr::SdfAssetPath>();
        out = assetPath.GetResolvedPath().empty() ? assetPath.GetAssetPath() : assetPath.GetResolvedPath();
        return true;
    }
    return false;
}

bool
isRepeatWrapModeString(const std::string& s)
{
    const std::string lower = toLower(s);
    return lower == "repeat" || lower == "periodic";
}

bool
parseWrapModeRepeat(const pxr::VtValue& value, bool& isRepeat)
{
    std::string wrap;
    if (extractStringValue(value, wrap)) {
        isRepeat = isRepeatWrapModeString(wrap);
        return true;
    }
    if (value.IsHolding<int>()) {
        // UsdUVTexture wrap enum: repeat == 2
        isRepeat = (value.UncheckedGet<int>() == 2);
        return true;
    }
    if (value.IsHolding<long>()) {
        isRepeat = (value.UncheckedGet<long>() == 2);
        return true;
    }
    if (value.IsHolding<long long>()) {
        isRepeat = (value.UncheckedGet<long long>() == 2);
        return true;
    }
    return false;
}

void
applyImageMapWrapAroundMapping(std::map<pxr::TfToken, pxr::VtValue>& params,
                               const pxr::SdfPath& nodePath)
{
    static const pxr::TfToken tWrapAround("wrap_around");
    static const pxr::TfToken tWrapS("wrapS");
    static const pxr::TfToken tWrapT("wrapT");
    static const pxr::TfToken tUAddressMode("uaddressmode");
    static const pxr::TfToken tVAddressMode("vaddressmode");
    static const pxr::TfToken tUAddress("uaddress");
    static const pxr::TfToken tVAddress("vaddress");

    bool hasWrapSignal = false;
    bool wrapAround = true;
    auto applyWrapToken = [&](const pxr::TfToken& key) {
        auto it = params.find(key);
        if (it == params.end()) {
            return;
        }
        bool isRepeat = false;
        if (parseWrapModeRepeat(it->second, isRepeat)) {
            hasWrapSignal = true;
            wrapAround = wrapAround && isRepeat;
        }
    };

    applyWrapToken(tWrapS);
    applyWrapToken(tWrapT);
    applyWrapToken(tUAddressMode);
    applyWrapToken(tVAddressMode);
    applyWrapToken(tUAddress);
    applyWrapToken(tVAddress);

    if (hasWrapSignal) {
        params[tWrapAround] = pxr::VtValue(wrapAround);
        Logger::debug(nodePath, ": bridged wrap modes -> wrap_around=", wrapAround ? "true" : "false");
    }
}

bool getScalar(const pxr::VtValue& v, float& out);
bool getBool(const pxr::VtValue& v, bool& out);
bool getVec2f(const pxr::VtValue& v, pxr::GfVec2f& out);
bool getRgb(const pxr::VtValue& v, pxr::GfVec3f& out);

void
applyMaterialXImageMapping(const pxr::TfToken& identifier,
                           std::map<pxr::TfToken, pxr::VtValue>& params,
                           const pxr::SdfPath& nodePath,
                           MappingStats& stats)
{
    (void)identifier;
    static const pxr::TfToken tTexture("texture");
    static const pxr::TfToken tFile("file");
    static const pxr::TfToken tDefault("default");
    static const pxr::TfToken tUvTiling("uvtiling");
    static const pxr::TfToken tUvOffset("uvoffset");
    static const pxr::TfToken tScale("scale");
    static const pxr::TfToken tOffset("offset");
    static const pxr::TfToken tFilterType("filtertype");
    static const pxr::TfToken tFrameRange("framerange");
    static const pxr::TfToken tFrameOffset("frameoffset");
    static const pxr::TfToken tFrameEndAction("frameendaction");

    if (params.find(tTexture) == params.end()) {
        auto fileIt = params.find(tFile);
        if (fileIt != params.end()) {
            params[tTexture] = fileIt->second;
            Logger::debug(nodePath, ": bridged image parameter file -> texture");
            ++stats.exact;
        }
    }

    if (params.find(tDefault) != params.end()) {
        ++stats.unsupported;
        warnMaterialXLossyOnce(nodePath, "image:default",
                               "MaterialX default/fallback color is currently unsupported in native ImageMap bridge");
    }

    if (params.find(tScale) == params.end()) {
        auto tilingIt = params.find(tUvTiling);
        if (tilingIt != params.end()) {
            params[tScale] = tilingIt->second;
            Logger::debug(nodePath, ": bridged image parameter uvtiling -> scale");
            ++stats.exact;
        }
    }
    if (params.find(tOffset) == params.end()) {
        auto offsetIt = params.find(tUvOffset);
        if (offsetIt != params.end()) {
            params[tOffset] = offsetIt->second;
            Logger::debug(nodePath, ": bridged image parameter uvoffset -> offset");
            ++stats.exact;
        }
    }

    if (params.find(tFilterType) != params.end()) {
        ++stats.unsupported;
        warnMaterialXLossyOnce(nodePath, "image:filtertype",
                               "MaterialX filtertype has no direct ImageMap equivalent and is ignored");
    }
    if (params.find(tFrameRange) != params.end() ||
        params.find(tFrameOffset) != params.end() ||
        params.find(tFrameEndAction) != params.end()) {
        ++stats.unsupported;
        warnMaterialXLossyOnce(nodePath, "image:frame_controls",
                               "MaterialX frame controls are currently unsupported in ImageMap");
    }

    applyImageMapWrapAroundMapping(params, nodePath);
}

bool
hasNonEmptyImagePath(const std::map<pxr::TfToken, pxr::VtValue>& params)
{
    static const pxr::TfToken tTexture("texture");
    static const pxr::TfToken tFile("file");
    const pxr::VtValue* pathValue = nullptr;
    auto textureIt = params.find(tTexture);
    if (textureIt != params.end()) {
        pathValue = &(textureIt->second);
    } else {
        auto fileIt = params.find(tFile);
        if (fileIt != params.end()) {
            pathValue = &(fileIt->second);
        }
    }

    if (!pathValue) {
        return false;
    }

    if (pathValue->IsHolding<pxr::SdfAssetPath>()) {
        const pxr::SdfAssetPath& assetPath = pathValue->UncheckedGet<pxr::SdfAssetPath>();
        return !(assetPath.GetResolvedPath().empty() && assetPath.GetAssetPath().empty());
    }
    if (pathValue->IsHolding<std::string>()) {
        return !pathValue->UncheckedGet<std::string>().empty();
    }
    if (pathValue->IsHolding<pxr::TfToken>()) {
        return !pathValue->UncheckedGet<pxr::TfToken>().IsEmpty();
    }
    return true;
}

std::string
resolveOutputAttributeName(const scene_rdl2::rdl2::SceneObject* output,
                           const pxr::TfToken& outputName,
                           const pxr::SdfPath& outputId)
{
    static const pxr::TfToken tBaseColor("base_color");
    static const pxr::TfToken tSpecularRoughness("specular_roughness");
    static const pxr::TfToken tMetalness("metalness");
    static const pxr::TfToken tOpacity("opacity");
    static const pxr::TfToken tIor("ior");
    static const pxr::TfToken tEmissionColor("emission_color");
    static const pxr::TfToken tTexcoord("texcoord");
    static const pxr::TfToken tSt("st");
    static const pxr::TfToken tIn("in");
    static const pxr::TfToken tFile("file");
    static const pxr::TfToken tDefault("default");

    static const pxr::TfToken tAlbedo("albedo");
    static const pxr::TfToken tRoughness("roughness");
    static const pxr::TfToken tMetallic("metallic");
    static const pxr::TfToken tPresence("presence");
    static const pxr::TfToken tRefractiveIndex("refractive_index");
    static const pxr::TfToken tEmission("emission");
    static const pxr::TfToken tInputTextureCoordinates("input_texture_coordinates");
    static const pxr::TfToken tTexture("texture");
    static const pxr::TfToken tDefaultColor("default_color");

    if (output &&
        output->getSceneClass().getName() == "DwaBaseMaterial") {
        if (outputName == tBaseColor) {
            Logger::debug(outputId, ": remapped output attribute base_color -> albedo");
            return tAlbedo.GetString();
        }
        if (outputName == tSpecularRoughness) {
            Logger::debug(outputId, ": remapped output attribute specular_roughness -> roughness");
            return tRoughness.GetString();
        }
        if (outputName == tMetalness) {
            Logger::debug(outputId, ": remapped output attribute metalness -> metallic");
            return tMetallic.GetString();
        }
        if (outputName == tOpacity) {
            Logger::debug(outputId, ": remapped output attribute opacity -> presence");
            return tPresence.GetString();
        }
        if (outputName == tIor) {
            Logger::debug(outputId, ": remapped output attribute ior -> refractive_index");
            return tRefractiveIndex.GetString();
        }
        if (outputName == tEmissionColor) {
            Logger::debug(outputId, ": remapped output attribute emission_color -> emission");
            return tEmission.GetString();
        }
    }
    if (output &&
        output->getSceneClass().getName() == "ImageMap") {
        if (outputName == tTexcoord || outputName == tSt || outputName == tIn) {
            Logger::debug(outputId, ": remapped output attribute ", outputName,
                          " -> input_texture_coordinates");
            return tInputTextureCoordinates.GetString();
        }
        if (outputName == tFile) {
            Logger::debug(outputId, ": remapped output attribute file -> texture");
            return tTexture.GetString();
        }
        if (outputName == tDefault) {
            Logger::debug(outputId, ": remapped output attribute default -> default_color");
            return tDefaultColor.GetString();
        }
    }
    return outputName.GetString();
}

bool
getScalar(const pxr::VtValue& v, float& out)
{
    if (v.IsHolding<float>()) {
        out = v.UncheckedGet<float>();
        return true;
    }
    if (v.IsHolding<double>()) {
        out = static_cast<float>(v.UncheckedGet<double>());
        return true;
    }
    if (v.IsHolding<int>()) {
        out = static_cast<float>(v.UncheckedGet<int>());
        return true;
    }
    if (v.IsHolding<long>()) {
        out = static_cast<float>(v.UncheckedGet<long>());
        return true;
    }
    if (v.IsHolding<long long>()) {
        out = static_cast<float>(v.UncheckedGet<long long>());
        return true;
    }
    return false;
}

bool
getBool(const pxr::VtValue& v, bool& out)
{
    if (v.IsHolding<bool>()) {
        out = v.UncheckedGet<bool>();
        return true;
    }
    float scalar = 0.0f;
    if (getScalar(v, scalar)) {
        out = (scalar > 0.5f);
        return true;
    }
    return false;
}

bool
getVec2f(const pxr::VtValue& v, pxr::GfVec2f& out)
{
    if (v.IsHolding<pxr::GfVec2f>()) {
        out = v.UncheckedGet<pxr::GfVec2f>();
        return true;
    }
    if (v.IsHolding<pxr::GfVec2d>()) {
        auto d = v.UncheckedGet<pxr::GfVec2d>();
        out = pxr::GfVec2f(static_cast<float>(d[0]), static_cast<float>(d[1]));
        return true;
    }
    return false;
}

bool
getRgb(const pxr::VtValue& v, pxr::GfVec3f& out)
{
    if (v.IsHolding<pxr::GfVec3f>()) {
        out = v.UncheckedGet<pxr::GfVec3f>();
        return true;
    }
    if (v.IsHolding<pxr::GfVec3d>()) {
        auto d = v.UncheckedGet<pxr::GfVec3d>();
        out = pxr::GfVec3f(static_cast<float>(d[0]), static_cast<float>(d[1]), static_cast<float>(d[2]));
        return true;
    }
    if (v.IsHolding<pxr::GfVec4f>()) {
        auto c = v.UncheckedGet<pxr::GfVec4f>();
        out = pxr::GfVec3f(c[0], c[1], c[2]);
        return true;
    }
    if (v.IsHolding<pxr::GfVec4d>()) {
        auto c = v.UncheckedGet<pxr::GfVec4d>();
        out = pxr::GfVec3f(static_cast<float>(c[0]), static_cast<float>(c[1]), static_cast<float>(c[2]));
        return true;
    }
    float scalar;
    if (getScalar(v, scalar)) {
        out = pxr::GfVec3f(scalar);
        return true;
    }
    return false;
}

void
setIfPresent(std::map<pxr::TfToken, pxr::VtValue>& outParams,
             const std::map<pxr::TfToken, pxr::VtValue>& inParams,
             const pxr::TfToken& inToken,
             const pxr::TfToken& outToken)
{
    auto it = inParams.find(inToken);
    if (it != inParams.end()) {
        outParams[outToken] = it->second;
    }
}

void
applyMaterialXStandardSurfaceMapping(const pxr::HdMaterialNode& node,
                                     std::map<pxr::TfToken, pxr::VtValue>& params,
                                     MappingStats& stats)
{
    const auto& rules = getStandardSurfaceMappingRules();
    for (const StandardSurfaceMappingRule& rule : rules) {
        auto inIt = node.parameters.find(pxr::TfToken(rule.inputName));
        if (inIt == node.parameters.end()) {
            continue;
        }
        if (rule.quality == MappingQuality::Unsupported) {
            ++stats.unsupported;
            warnMaterialXLossyOnce(
                node.path,
                std::string("unsupported:") + rule.inputName,
                std::string("MaterialX input '") + rule.inputName + "' is currently unsupported (" + rule.note + ")");
            continue;
        }
        if (rule.quality == MappingQuality::Exact) {
            ++stats.exact;
        } else {
            ++stats.approximate;
            warnMaterialXLossyOnce(
                node.path,
                std::string("approx:") + rule.inputName,
                std::string("MaterialX input '") + rule.inputName + "' uses approximate mapping (" + rule.note + ")");
        }

        const pxr::TfToken outToken(rule.outputName ? rule.outputName : "");
        switch (rule.mode) {
        case MappingMode::Copy:
            if (!outToken.IsEmpty()) {
                params[outToken] = inIt->second;
            }
            break;
        case MappingMode::OpacityToPresence: {
            pxr::GfVec3f opacityRgb(1.0f);
            if (getRgb(inIt->second, opacityRgb)) {
                const float opacityScalar = (opacityRgb[0] + opacityRgb[1] + opacityRgb[2]) / 3.0f;
                params[pxr::TfToken("presence")] = pxr::VtValue(opacityScalar);
            }
            break;
        }
        case MappingMode::BoolToThinGeometry: {
            bool thin = false;
            if (getBool(inIt->second, thin)) {
                params[pxr::TfToken("thin_geometry")] = pxr::VtValue(thin);
            }
            break;
        }
        case MappingMode::DispersionToUseDispersion: {
            float dispersion = 0.0f;
            if (getScalar(inIt->second, dispersion)) {
                params[pxr::TfToken("use_dispersion")] = pxr::VtValue(dispersion > 0.0f);
            }
            break;
        }
        case MappingMode::BaseColorWithFactor:
        case MappingMode::EmissionWithFactor:
        case MappingMode::Unsupported:
            // Handled after the table loop to ensure both factors/inputs are available.
            break;
        }
    }

    pxr::GfVec3f baseColor(1.0f);
    const auto baseColorIt = node.parameters.find(pxr::TfToken("base_color"));
    const bool hasBaseColor = baseColorIt != node.parameters.end() && getRgb(baseColorIt->second, baseColor);
    if (hasBaseColor) {
        float baseFactor = 1.0f;
        const auto baseIt = node.parameters.find(pxr::TfToken("base"));
        if (baseIt != node.parameters.end()) {
            getScalar(baseIt->second, baseFactor);
        }
        params[pxr::TfToken("albedo")] = pxr::VtValue(baseColor * baseFactor);
    }

    pxr::GfVec3f emissionColor(0.0f);
    const auto emissionColorIt = node.parameters.find(pxr::TfToken("emission_color"));
    const bool hasEmissionColor =
        emissionColorIt != node.parameters.end() && getRgb(emissionColorIt->second, emissionColor);
    if (hasEmissionColor) {
        float emissionFactor = 1.0f;
        const auto emissionIt = node.parameters.find(pxr::TfToken("emission"));
        if (emissionIt != node.parameters.end()) {
            getScalar(emissionIt->second, emissionFactor);
        }
        params[pxr::TfToken("emission")] = pxr::VtValue(emissionColor * emissionFactor);
    }
}

void
applyMaterialXPrimvarUtilityMapping(std::map<pxr::TfToken, pxr::VtValue>& params,
                                    const pxr::SdfPath& nodePath)
{
    static const pxr::TfToken tVarname("varname");
    static const pxr::TfToken tGeomprop("geomprop");
    static const pxr::TfToken tDefaultGeomprop("defaultgeomprop");
    static const pxr::TfToken tFallback("fallback");
    static const pxr::TfToken tDefault("default");

    if (params.find(tVarname) == params.end()) {
        auto gpIt = params.find(tGeomprop);
        if (gpIt != params.end()) {
            params[tVarname] = gpIt->second;
        } else {
            auto dgpIt = params.find(tDefaultGeomprop);
            if (dgpIt != params.end()) {
                params[tVarname] = dgpIt->second;
            } else {
                params[tVarname] = pxr::VtValue(std::string("UV0"));
            }
        }
    }

    if (params.find(tFallback) == params.end()) {
        auto dIt = params.find(tDefault);
        if (dIt != params.end()) {
            params[tFallback] = dIt->second;
        }
    }

    Logger::debug(nodePath, ": bridged MaterialX primvar utility -> UsdPrimvarReader_float2");
}

void
applyMaterialXTransform2dMapping(std::map<pxr::TfToken, pxr::VtValue>& params,
                                 const pxr::SdfPath& nodePath)
{
    static const pxr::TfToken tTexcoord("texcoord");
    static const pxr::TfToken tIn("in");
    static const pxr::TfToken tUvOffset("uvoffset");
    static const pxr::TfToken tTranslation("translation");
    static const pxr::TfToken tUvTiling("uvtiling");
    static const pxr::TfToken tScale("scale");

    if (params.find(tIn) == params.end()) {
        auto texIt = params.find(tTexcoord);
        if (texIt != params.end()) {
            pxr::GfVec2f st(0.0f, 0.0f);
            if (getVec2f(texIt->second, st)) {
                params[tIn] = pxr::VtValue(pxr::GfVec3f(st[0], st[1], 0.0f));
            }
        }
    }
    if (params.find(tTranslation) == params.end()) {
        auto uvOffsetIt = params.find(tUvOffset);
        if (uvOffsetIt != params.end()) {
            params[tTranslation] = uvOffsetIt->second;
        }
    }
    if (params.find(tScale) == params.end()) {
        auto uvTilingIt = params.find(tUvTiling);
        if (uvTilingIt != params.end()) {
            params[tScale] = uvTilingIt->second;
        }
    }
    Logger::debug(nodePath, ": bridged MaterialX transform2d -> UsdTransform2d");
}

pxr::TfToken
resolveSurfaceTerminal(const pxr::HdMaterialNetworkMap& networkmap,
                       const pxr::TfToken& requestedTerminal)
{
    if (requestedTerminal != pxr::HdMaterialTerminalTokens->surface) {
        if (shouldTraceNativePath()) {
            Logger::info("hdMoonray native trace terminal: requested='", requestedTerminal,
                         "' (non-surface passthrough)");
        }
        return requestedTerminal;
    }
    if (shouldStrictMoonraySurface()) {
        if (networkmap.map.find(moonraySurfaceTerminalToken) != networkmap.map.end()) {
            Logger::info("hdMoonray material terminal strict mode: using 'moonray:surface'");
            return moonraySurfaceTerminalToken;
        }
        Logger::info("hdMoonray material terminal strict mode: missing 'moonray:surface' terminal");
        return moonraySurfaceTerminalToken;
    }
    if (networkmap.map.find(requestedTerminal) != networkmap.map.end()) {
        if (shouldTraceNativePath()) {
            Logger::info("hdMoonray native trace terminal: using requested 'surface'");
        }
        return requestedTerminal;
    }
    if (networkmap.map.find(moonraySurfaceTerminalToken) != networkmap.map.end()) {
        Logger::info("hdMoonray material terminal fallback: using 'moonray:surface'");
        return moonraySurfaceTerminalToken;
    }
    for (const auto& entry : networkmap.map) {
        const std::string terminal = entry.first.GetString();
        if (endsWith(terminal, ":surface")) {
            Logger::info("hdMoonray material terminal fallback: using '", terminal, "'");
            return entry.first;
        }
    }
    if (shouldTraceNativePath()) {
        Logger::info("hdMoonray native trace terminal: no matching surface terminal found");
    }
    return requestedTerminal;
}

UNUSED
void
dumpMaterialNetworkMap(const pxr::HdMaterialNetworkMap& networkmap)
{
    // struct HdMaterialNetworkMap {
    //    std::map<TfToken, HdMaterialNetwork> map;
    //    std::vector<SdfPath> terminals;
    std::cout << "=== Material Networks ===" << std::endl;
    for (auto const& iter : networkmap.map) {
        const pxr::TfToken & terminalName = iter.first;
        const pxr::HdMaterialNetwork & network = iter.second;

        std::cout << "Terminal '" << terminalName << "':" << std::endl;

        // struct HdMaterialNetwork {
        //    std::vector<HdMaterialRelationship> relationships;
        //    std::vector<HdMaterialNode> nodes;
        //    TfTokenVector primvars;

        std::cout << "  primvars:";
        for (const pxr::TfToken& primvarName : network.primvars) {
            std::cout << " " << primvarName;
        }
        std::cout << std::endl;

        unsigned i = 0;
        for (const pxr::HdMaterialNode & node : network.nodes) {
            // struct HdMaterialNode {
            //    SdfPath path;
            //    TfToken identifier;
            //    std::map<TfToken, VtValue> parameters;
            std::cout << "  node " << i++ << ": " << node.identifier << " " << node.path << std::endl;
            for (auto const& jter : node.parameters) {
                std::cout << "    " << jter.first << " = " << jter.second
                          << " [" << jter.second.GetTypeName() << "] " << std::endl;
            }
        }

        for (const pxr::HdMaterialRelationship& rel : network.relationships) {
            // struct HdMaterialRelationship {
            //    SdfPath inputId;
            //    TfToken inputName;
            //    SdfPath outputId;
            //    TfToken outputName;
            std::cout << "  " << rel.outputId << "." << rel.outputName
                      << " bound to "<< rel.inputId << "." << rel.inputName
                      <<  std::endl;
        }
        std::cout << "---" << std::endl;
    }
    // This is redundant, it is always the last node in each network. I have no idea why they
    // store this redundant information
    // std::cout << "terminals: " << std::endl;
    // for (const pxr::SdfPath& terminal : networkmap.terminals) {
    //     std::cout << "    " << terminal << std::endl;
    // }

    std::cout << "=========================" << std::endl;
}

SceneObject*
getCoordSysBinding(
    hdMoonray::RenderDelegate& renderDelegate,
    pxr::HdSceneDelegate *sceneDelegate,
    const pxr::HdMaterialNode& node, pxr::TfToken key,
    const pxr::HdRprim* geom
) {
    auto valIt = node.parameters.find(key);
    if (valIt != node.parameters.end()) {
        const pxr::VtValue& val = valIt->second;
        if (val.IsHolding<pxr::TfToken>()) {
            pxr::TfToken coordSysName = val.UncheckedGet<pxr::TfToken>();
            // empty token is used to specify a nullptr SceneObject*
            if (coordSysName.IsEmpty()) return nullptr;
            scene_rdl2::rdl2::SceneObject* sceneObject =
                hdMoonray::CoordSys::getBinding(sceneDelegate, renderDelegate, geom->GetId(), coordSysName);
            if (not sceneObject) {
                Logger::error(node.path, ".", key, ": failed to find binding for coordSys ", coordSysName);
            }
            return sceneObject;
        } else {
            Logger::error(node.path, ".", key, ": invalid type '",val.GetTypeName(), "', should be 'token'");
        }
    } else if (key == projectorToken) {
        // fix sq9010 s2 and other shots where Mpaint assets are missing the coordSys binding
        scene_rdl2::rdl2::SceneObject* sceneObject =
            hdMoonray::CoordSys::getBinding(sceneDelegate, renderDelegate, geom->GetId(), proj_camToken);
        // no error if not found, as this would be triggered if bindings to shaders are used
        return sceneObject;
    } 
    return nullptr;
}

SceneObject*
makeMoonrayShader(
    hdMoonray::RenderDelegate& renderDelegate,
    pxr::HdSceneDelegate *sceneDelegate,
    const pxr::HdMaterialNode& node,
    const std::string& nodeName,
    const std::string& requestedOutputChannel,
    const pxr::HdRprim* geom
) {
    std::string className = normalizeNativeTextureMaterialClassName(node.identifier);
    const bool isNativeTextureMaterial = (className == "DwaBaseMaterial" || className == "ImageMap");
    const bool isMaterialXStandardSurface = isMaterialXStandardSurfaceIdentifier(node.identifier);
    const bool isMaterialXImageOrTiled = isMaterialXImageOrTiledIdentifier(node.identifier);
    const bool isMaterialXTransform2d = isUsdTransform2dIdentifier(node.identifier);
    const bool isMaterialXPrimvarUtility = isMaterialXPrimvarUtilityIdentifier(node.identifier);
    const bool isUnsupportedMaterialXUtility =
        looksLikeUnsupportedMaterialXUtilityIdentifier(node.identifier);
    const bool isImageLikeButNotAllowed =
        looksLikeImageIdentifier(node.identifier) &&
        !isMaterialXImageOrTiled &&
        !isUsdUVTextureIdentifier(node.identifier);

    if (isNativeTextureMaterial) {
        Logger::debug(node.path, ": native Moonray node passthrough id='", node.identifier,
                      "' class='", className, "'");
    } else if (className == "BaseMaterial") {
        className = "DwaBaseMaterial";
        Logger::info(node.path, ": aliased BaseMaterial -> DwaBaseMaterial");
    } else if (isMaterialXStandardSurface) {
        className = "DwaBaseMaterial";
        Logger::info(node.path, ": bridged MaterialX standard_surface -> DwaBaseMaterial");
    } else if (isMaterialXImageOrTiled) {
        className = "ImageMap";
        Logger::debug(node.path, ": bridged image node '", node.identifier, "' -> ImageMap");
    } else if (isMaterialXTransform2d) {
        className = "UsdTransform2d";
        Logger::debug(node.path, ": bridged transform2d node '", node.identifier, "' -> UsdTransform2d");
    } else if (isMaterialXPrimvarUtility) {
        className = "UsdPrimvarReader_float2";
        Logger::debug(node.path, ": bridged primvar utility node '", node.identifier, "' -> UsdPrimvarReader_float2");
    } else if (isUnsupportedMaterialXUtility) {
        Logger::debug(node.path, ": unsupported MaterialX utility node id '",
                      node.identifier, "' (native ImageMap-only bridge)");
    } else if (isImageLikeButNotAllowed) {
        Logger::debug(node.path, ": image-like node id not allowlisted: '", node.identifier, "'");
    }
    if (shouldTraceNativePath()) {
        Logger::info("hdMoonray native trace node: path='", node.path,
                     "' id='", node.identifier, "' resolvedClass='", className, "'");
    }

    std::map<pxr::TfToken, pxr::VtValue> params(node.parameters.begin(), node.parameters.end());
    MappingStats stats;
    if (isMaterialXImageOrTiled) {
        applyMaterialXImageMapping(node.identifier, params, node.path, stats);
    }
    if (isMaterialXStandardSurface) {
        applyMaterialXStandardSurfaceMapping(node, params, stats);
    }
    if (isMaterialXTransform2d) {
        applyMaterialXTransform2dMapping(params, node.path);
    }
    if (isMaterialXPrimvarUtility) {
        applyMaterialXPrimvarUtilityMapping(params, node.path);
    }
    if (className == "ImageMap" && requestedOutputChannel == "a") {
        params[pxr::TfToken("alpha_only")] = pxr::VtValue(true);
        ++stats.approximate;
        warnMaterialXLossyOnce(node.path, "image:alpha_only",
                               "Using ImageMap alpha_only for channel 'a' extraction");
    } else if (className == "ImageMap" &&
               (requestedOutputChannel == "r" ||
                requestedOutputChannel == "g" ||
                requestedOutputChannel == "b")) {
        ++stats.unsupported;
        warnMaterialXLossyOnce(node.path, "image:rgb_channel_extract",
                               "ImageMap does not support isolated r/g/b extraction directly; using rgb output");
    }
    if (isMaterialXStandardSurface || isMaterialXImageOrTiled || isMaterialXTransform2d || isMaterialXPrimvarUtility) {
        Logger::debug(node.path, ": MaterialX mapping summary exact=", stats.exact,
                      " approximate=", stats.approximate, " unsupported=", stats.unsupported);
    }

    if (className == "ImageMap") {
        debugLogSelectedParams(node.path, "ImageMap",
                               params,
                               {pxr::TfToken("file"), pxr::TfToken("texture"),
                                pxr::TfToken("uvtiling"),
                                pxr::TfToken("uvoffset"), pxr::TfToken("scale"),
                                pxr::TfToken("offset"), pxr::TfToken("wrap_around")});
    }

    SceneObject* shaderObj = renderDelegate.createSceneObject(className, nodeName);
    if (shaderObj) {
        try {
            SceneObject::UpdateGuard guard(shaderObj);
            const SceneClass& sceneClass = shaderObj->getSceneClass();
            if (sceneClass.getName() == "ImageMap") {
                auto hasAttr = [&sceneClass](const char* name) {
                    return sceneClass.getAttribute(name) ? "Y" : "N";
                };
                Logger::debug(node.path, ": ImageMap native attrs texture=", hasAttr("texture"),
                              " scale=", hasAttr("scale"),
                              " offset=", hasAttr("offset"),
                              " wrap_around=", hasAttr("wrap_around"),
                              " input_texture_coordinates=", hasAttr("input_texture_coordinates"),
                              " texture_coordinates=", hasAttr("texture_coordinates"));
            }
            for (auto it = sceneClass.beginAttributes(); it != sceneClass.endAttributes(); ++it) {
                const Attribute* attribute = *it;
                const std::string& attrName = attribute->getName();
                if (geom!=nullptr && attribute->getType() == scene_rdl2::rdl2::TYPE_SCENE_OBJECT) {
                    SceneObject* binding = getCoordSysBinding(
                        renderDelegate, sceneDelegate, node, pxr::TfToken(attrName), geom);
                    shaderObj->set(AttributeKey<SceneObject*>(*attribute), binding);
                } else {
                    auto valIt = params.find(pxr::TfToken(attrName));
                    if (valIt != params.end()) {
                        hdMoonray::ValueConverter::setAttribute(shaderObj, attribute, valIt->second);
                    } else {
                        hdMoonray::ValueConverter::setDefault(shaderObj, attribute);
                    }
                }
            }
        } catch (const std::exception& e) {
            Logger::error(node.path, ": ", e.what());
            return nullptr;
        }
    }
    return shaderObj;
}


std::string
getNodeWithChannelName(const std::string& nodePath,
                       const std::string& channelName)
{
    return nodePath + std::string(".") + channelName;
}

//} // namespace {

namespace hdMoonray {

pxr::HdDirtyBits
Material::GetInitialDirtyBitsMask() const
{
    return AllDirty; // as of 20.5 this is DirtyParams | DirtyResource
}

void
Material::Sync(pxr::HdSceneDelegate *sceneDelegate,
               pxr::HdRenderParam   *renderParam,
               pxr::HdDirtyBits     *dirtyBits)
{
    auto& renderDelegate(RenderDelegate::get(renderParam));

    const pxr::SdfPath& id = GetId();
    hdmLogSyncStart("Material", id, dirtyBits);
    
    if (*dirtyBits & AllDirty) {
        mResource = sceneDelegate->GetMaterialResource(id);
        mMaterialDirty = mDisplacementDirty = mVolumeShaderDirty = true;
        // update any material that has been created
        if (mMaterial) getMaterial(renderDelegate, sceneDelegate, mGeom);
        if (mDisplacement) getDisplacement(renderDelegate, sceneDelegate, mGeom);
        if (mVolumeShader) getVolumeShader(renderDelegate, sceneDelegate, mGeom);
    }
    *dirtyBits &= ~AllDirty;
    hdmLogSyncEnd(id);
}

bool Material::isEnabled() const
{
    return mResource.IsHolding<pxr::HdMaterialNetworkMap>();
}

// Build the tree of shaders, return the final output
// Returns null if the terninal does not exist, returns error shader on other errors
// The geom is for finding coordSys bindings
scene_rdl2::rdl2::SceneObject*
Material::updateTerminal(pxr::TfToken terminalName,
                         RenderDelegate& renderDelegate,
                         pxr::HdSceneDelegate *sceneDelegate,
                         const pxr::HdRprim* geom)
{
    if (not isEnabled()) {
        return nullptr;
    }

    const pxr::HdMaterialNetworkMap& networkmap = mResource.UncheckedGet<pxr::HdMaterialNetworkMap>();
    terminalName = resolveSurfaceTerminal(networkmap, terminalName);
    if (shouldTraceNativePath()) {
        Logger::info(GetId(), ": hdMoonray native trace selected terminal='", terminalName, "'");
    }
    auto i = networkmap.map.find(terminalName);
    if (i == networkmap.map.end()) {
        if (shouldTraceNativePath()) {
            Logger::info(GetId(), ": hdMoonray native trace terminal missing from network map");
        }
        return nullptr;
    }

    if (shouldDumpMaterialNetworkMap()) {
        dumpMaterialNetworkMap(networkmap);
    }

    const pxr::HdMaterialNetwork& network = i->second;

    // Get a mapping of nodes to their output channels.
    // When creating the nodes below, create one node
    // per channel.   The map only stores nodes with output
    // connections so won't contain the material at the end
    // which has no output connections.
    std::unordered_map< std::string, std::set<std::string> > nodeChannelMap;
    for (const pxr::HdMaterialRelationship& rel : network.relationships) {
        const std::string inputChannel = rel.inputName.GetString();

        // Find the input node
        for (const pxr::HdMaterialNode& node : network.nodes) {
            if (node.path == rel.inputId) {
                // Search for node.path string in map
                auto it = nodeChannelMap.find(node.path.GetString());
                if (it == nodeChannelMap.end()) {
                    // Not found. Create channel set with input name as channel
                    std::set<std::string> channelSet = { inputChannel };
                    std::pair<std::string, std::set<std::string>> entry = { node.path.GetString(), channelSet };
                    nodeChannelMap.insert(entry);
                } else {
                    // Found.  Insert channel name into channelSet
                    it->second.insert(inputChannel);
                }
            }
        }
    }

    // Create the nodes
    scene_rdl2::rdl2::SceneObject* last = nullptr;
    for (const pxr::HdMaterialNode& node : network.nodes) {
        scene_rdl2::rdl2::SceneObject* next = nullptr;
        // Skip image map creation when texture/file path is empty.
        if ((isUsdUVTextureIdentifier(node.identifier) ||
             isMaterialXImageOrTiledIdentifier(node.identifier)) &&
            !hasNonEmptyImagePath(node.parameters)) {
            Logger::debug(node.path, ": skipping image node '", node.identifier, "' with empty texture/file path");
            continue;
        }

        // Create a node for each channel entry
        // Moonray name must be absolute so that it is unique in the scene.
        // (HDM-401 : USD 0.25.5 returns relative node paths)
        std::string moonrayNodeName = node.path.MakeAbsolutePath(GetId()).GetString();

        auto it = nodeChannelMap.find(node.path.GetString());
        if (it != nodeChannelMap.end()) {
            std::set<std::string>& channelSet = it->second;
            for (const std::string& channel : channelSet) {

                const std::string nodeName =
                    getNodeWithChannelName(moonrayNodeName,
                                           channel);

                next = makeMoonrayShader(renderDelegate,
                                         sceneDelegate,
                                         node,
                                         nodeName,
                                         channel,
                                         geom);
            }
        } else {
            // If the node isn't in the nodeChannelMap (i.e. materials)
            // then only one node is created using the USD path as the name.
            next = makeMoonrayShader(renderDelegate,
                                     sceneDelegate,
                                     node,
                                     moonrayNodeName,
                                     std::string(),
                                     geom);
        }

        if (next) last = next; // HDM-368 : don't give up on error
    }

    // set bindings (fixme: only works for Moonray shaders)
    for (const pxr::HdMaterialRelationship& rel : network.relationships) {
        if (shouldTraceNativePath()) {
            Logger::info("hdMoonray native trace rel: output=", rel.outputId, ".", rel.outputName,
                         " <- input=", rel.inputId, ".", rel.inputName);
        }
        // Input connection
        SceneObject* input;

        // Moonray name is absolute path (HDM-401)
        std::string moonrayInputName = rel.inputId.MakeAbsolutePath(GetId()).GetString();
        bool foundNodeWithChannel = false;
        for (const pxr::HdMaterialNode& node : network.nodes) {
            if (node.path == rel.inputId) {

                const std::string nodeWithChannel =
                    getNodeWithChannelName(moonrayInputName,
                                           rel.inputName.GetString());

                input = renderDelegate.getSceneObject(nodeWithChannel);

                if (input) {
                    foundNodeWithChannel = true;
                }
            }
        }

        // If we didn't find the node with the channel name
        // extension then it must be a node with no outputs
        // (i.e. material) so just use the node name without
        // a channel extension to get it from the render delegate.
        if (!foundNodeWithChannel) {
            input = renderDelegate.getSceneObject(moonrayInputName);
        }

        if (!input) {
            continue;
        }

        // Special handling for specific nodes

        // Skip IOR maps
        if (rel.outputName.GetString().find("ior") != std::string::npos) {
            continue;
        }

        // UsdUVTexture map special handling
        if (input->getSceneClass().getName() == "UsdUVTexture") {
            try {
                UpdateGuard guard(input);

                // Decode normal maps
                if (renderDelegate.getDecodeNormals() &&
                    rel.outputName.GetString().find("normal") != std::string::npos) {
                    const scene_rdl2::rdl2::Attribute* scaleAttr =
                        input->getSceneClass().getAttribute("scale");
                    const scene_rdl2::rdl2::Attribute* biasAttr =
                        input->getSceneClass().getAttribute("bias");
                    const scene_rdl2::rdl2::Attribute* sourceColorSpaceAttr =
                        input->getSceneClass().getAttribute("sourceColorSpace");
                    if (scaleAttr && scaleAttr->getType() == scene_rdl2::rdl2::TYPE_RGB) {
                        input->set(AttributeKey<Rgb>(*scaleAttr), Rgb(2.0f));
                    }
                    if (biasAttr && biasAttr->getType() == scene_rdl2::rdl2::TYPE_RGB) {
                        input->set(AttributeKey<Rgb>(*biasAttr), Rgb(-1.0f));
                    }
                    if (sourceColorSpaceAttr &&
                        sourceColorSpaceAttr->getType() == scene_rdl2::rdl2::TYPE_INT) {
                        input->set(AttributeKey<Int>(*sourceColorSpaceAttr), 0);
                    }
                }

                // Channel binding
                const scene_rdl2::rdl2::Attribute* outputModeAttr =
                    input->getSceneClass().getAttribute("output_mode");
                if (outputModeAttr &&
                    outputModeAttr->getType() == scene_rdl2::rdl2::TYPE_INT) {
                    const std::string channel = rel.inputName.GetString();
                    const auto outputModeKey = input->getSceneClass().getAttributeKey<Int>("output_mode");
                    int enumValue = input->getSceneClass().getEnumValue(outputModeKey, channel);
                    input->set(outputModeKey, enumValue);
                }
            } catch (const std::exception& e) {
                Logger::error(rel.outputId, ": ", e.what());
                last = nullptr;
            }
        }

        // Output
        SceneObject* output;
        // Check if output is in nodeChannelMap
        // Moonray name is absolute path (HDM-401)
        std::string moonrayOutputName = rel.outputId.MakeAbsolutePath(GetId()).GetString();
        auto it = nodeChannelMap.find(rel.outputId.GetString());
        if (it != nodeChannelMap.end()) {
            // Create a binding for each channel's node
            for (const std::string& channel : it->second) {
                const std::string outputNodeName =
                    getNodeWithChannelName(moonrayOutputName,
                                           channel);

                output = renderDelegate.getSceneObject(outputNodeName);
                if (!output) {
                    continue;
                }

                try {
                    UpdateGuard guard(output);
                    const std::string outputAttributeName =
                        resolveOutputAttributeName(output, rel.outputName, rel.outputId);

                    if (output->getSceneClass().getName() == "ImageMap" &&
                        outputAttributeName == "input_texture_coordinates") {
                        const scene_rdl2::rdl2::Attribute* textureCoordinatesAttr =
                            output->getSceneClass().getAttribute("texture_coordinates");
                        if (textureCoordinatesAttr &&
                            textureCoordinatesAttr->getType() == scene_rdl2::rdl2::TYPE_INT) {
                            const auto textureCoordinatesKey =
                                output->getSceneClass().getAttributeKey<Int>("texture_coordinates");
                            output->set(textureCoordinatesKey, 2);
                        }
                    }

                    const scene_rdl2::rdl2::Attribute* attribute(
                        output->getSceneClass().getAttribute(outputAttributeName));
                    if (!attribute) {
                        Logger::debug(rel.outputId, ": skipping connection: rel.outputName='",
                                      rel.outputName, "' resolved='", outputAttributeName,
                                      "' scene class=", output->getSceneClass().getName());
                        continue;
                    }
                    if (output->getSceneClass().getName() == "DwaBaseMaterial" &&
                        outputAttributeName == "albedo" &&
                        input->getSceneClass().getName() == "ImageMap") {
                        Logger::debug(rel.outputId, ": binding DwaBaseMaterial.albedo <- ImageMap ", rel.inputId);
                    }

                    if (attribute->getType() == scene_rdl2::rdl2::TYPE_SCENE_OBJECT) {
                        output->set(AttributeKey<SceneObject*>(*attribute), input);
                    } else {
                        ValueConverter::setBinding(output, attribute, input);
                    }
                } catch (const std::exception& e) {
                    Logger::error(rel.outputId, ": ", e.what());
                    last = nullptr;
                }
            }
        } else {
            output = renderDelegate.getSceneObject(moonrayOutputName);
            if (!output) {
                continue;
            }

            try {
                UpdateGuard guard(output);
                const std::string outputAttributeName =
                    resolveOutputAttributeName(output, rel.outputName, rel.outputId);

                if (output->getSceneClass().getName() == "ImageMap" &&
                    outputAttributeName == "input_texture_coordinates") {
                    const scene_rdl2::rdl2::Attribute* textureCoordinatesAttr =
                        output->getSceneClass().getAttribute("texture_coordinates");
                    if (textureCoordinatesAttr &&
                        textureCoordinatesAttr->getType() == scene_rdl2::rdl2::TYPE_INT) {
                        const auto textureCoordinatesKey =
                            output->getSceneClass().getAttributeKey<Int>("texture_coordinates");
                        output->set(textureCoordinatesKey, 2);
                    }
                }

                const scene_rdl2::rdl2::Attribute* attribute(
                    output->getSceneClass().getAttribute(outputAttributeName));
                if (!attribute) {
                    Logger::debug(rel.outputId, ": skipping connection: rel.outputName='",
                                  rel.outputName, "' resolved='", outputAttributeName,
                                  "' scene class=", output->getSceneClass().getName());
                    continue;
                }
                if (output->getSceneClass().getName() == "DwaBaseMaterial" &&
                    outputAttributeName == "albedo" &&
                    input->getSceneClass().getName() == "ImageMap") {
                    Logger::debug(rel.outputId, ": binding DwaBaseMaterial.albedo <- ImageMap ", rel.inputId);
                }

                if (attribute->getType() == scene_rdl2::rdl2::TYPE_SCENE_OBJECT) {
                    output->set(AttributeKey<SceneObject*>(*attribute), input);
                } else {
                    ValueConverter::setBinding(output, attribute, input);
                }
            } catch (const std::exception& e) {
                Logger::error(rel.outputId, ": ", e.what());
                last = nullptr;
            }
        }
    }

    if (not last &&
        (terminalName == pxr::HdMaterialTerminalTokens->surface ||
         terminalName == moonraySurfaceTerminalToken)) {
        last = renderDelegate.errorMaterial();
    }
    return last;
}

scene_rdl2::rdl2::Material*
Material::getMaterial(
    RenderDelegate& renderDelegate,
    pxr::HdSceneDelegate *sceneDelegate,
    const pxr::HdRprim* geom)
{
    if (mMaterialDirty || renderDelegate.getDecodeNormalsChanged()) {
        std::lock_guard<std::mutex> lock(mCreateMutex);
        mGeom = geom;
        scene_rdl2::rdl2::SceneObject* s = updateTerminal(
            pxr::HdMaterialTerminalTokens->surface, renderDelegate, sceneDelegate, geom);
        mMaterial = s ? s->asA<scene_rdl2::rdl2::Material>() : nullptr;
        mMaterialDirty = false;
    }
    return mMaterial;
}

scene_rdl2::rdl2::Displacement*
Material::getDisplacement(
    RenderDelegate& renderDelegate,
    pxr::HdSceneDelegate *sceneDelegate,
    const pxr::HdRprim* geom)
{
    if (mDisplacementDirty) {
        std::lock_guard<std::mutex> lock(mCreateMutex);
        if (mDisplacementDirty) {
            mGeom = geom;
            scene_rdl2::rdl2::SceneObject* s = updateTerminal(
                pxr::HdMaterialTerminalTokens->displacement, renderDelegate, sceneDelegate, geom);
            mDisplacement = s ? s->asA<scene_rdl2::rdl2::Displacement>() : nullptr;
            mDisplacementDirty = false;
        }
    }
    return mDisplacement;
}


scene_rdl2::rdl2::VolumeShader*
Material::getVolumeShader(
    RenderDelegate& renderDelegate,
    pxr::HdSceneDelegate *sceneDelegate,
    const pxr::HdRprim* geom)
{
    if (mVolumeShaderDirty) {
        std::lock_guard<std::mutex> lock(mCreateMutex);
        if (mVolumeShaderDirty) {
            mGeom = geom;
            scene_rdl2::rdl2::SceneObject* s = updateTerminal(
                pxr::HdMaterialTerminalTokens->volume, renderDelegate, sceneDelegate, geom);
            mVolumeShader = s ? s->asA<scene_rdl2::rdl2::VolumeShader>() : nullptr;
            mVolumeShaderDirty = false;
        }
    }
    return mVolumeShader;
}

void
Material::get(
    scene_rdl2::rdl2::LayerAssignment& layerAssignment,
    const pxr::SdfPath& materialId,
    RenderDelegate& renderDelegate,
    pxr::HdSceneDelegate* sceneDelegate,
    const pxr::HdRprim* geom,
    bool volume)
{
    if (not materialId.IsEmpty()) {
        Material* mtlPrim = static_cast<Material*>(
            sceneDelegate->GetRenderIndex().GetSprim(pxr::HdPrimTypeTokens->material, materialId));
        if (not mtlPrim) {
            Logger::error(geom->GetId(), ".material: ", materialId, " has no moonray or fallback shaders");
            layerAssignment.mMaterial = volume ? nullptr : renderDelegate.errorMaterial();
            layerAssignment.mDisplacement = nullptr;
            layerAssignment.mVolumeShader = volume ? renderDelegate.defaultVolumeShader() : nullptr;
            return;
        }
        if (mtlPrim->isEnabled()) {
            layerAssignment.mMaterial = mtlPrim->getMaterial(renderDelegate, sceneDelegate, geom);
            layerAssignment.mDisplacement = mtlPrim->getDisplacement(renderDelegate, sceneDelegate, geom);
            layerAssignment.mVolumeShader = mtlPrim->getVolumeShader(renderDelegate, sceneDelegate, geom);
            return;
        }
    }
    // return default material
    layerAssignment.mMaterial = volume ? nullptr : renderDelegate.defaultMaterial();
    layerAssignment.mDisplacement = nullptr;
    layerAssignment.mVolumeShader = volume ? renderDelegate.defaultVolumeShader() : nullptr;
}

}
