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
#include <cctype>
#include <cstdlib>

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
isMaterialXTexcoordIdentifier(const pxr::TfToken& identifier)
{
    const std::string idLower = toLower(identifier.GetString());
    return idLower == "mtlxtexcoord" ||
           startsWith(idLower, "nd_texcoord_") ||
           idLower.find("nd_texcoord_") != std::string::npos;
}

bool
isMaterialXGeompropIdentifier(const pxr::TfToken& identifier)
{
    const std::string idLower = toLower(identifier.GetString());
    return idLower == "mtlxgeompropvalue" ||
           startsWith(idLower, "nd_geompropvalue_") ||
           idLower.find("nd_geompropvalue_") != std::string::npos;
}

bool
isMaterialXTexcoordLikeIdentifier(const pxr::TfToken& identifier)
{
    return isMaterialXTexcoordIdentifier(identifier) ||
           isMaterialXGeompropIdentifier(identifier);
}

bool
isMaterialXPlace2dIdentifier(const pxr::TfToken& identifier)
{
    const std::string idLower = toLower(identifier.GetString());
    return idLower == "mtlxplace2d" ||
           (idLower.find("nd_") != std::string::npos &&
            idLower.find("place2d") != std::string::npos);
}

bool
looksLikeImageIdentifier(const pxr::TfToken& identifier)
{
    const std::string idLower = toLower(identifier.GetString());
    return idLower.find("image") != std::string::npos ||
           idLower.find("uvtexture") != std::string::npos;
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

void
applyMaterialXImageMapping(const pxr::TfToken& identifier,
                           std::map<pxr::TfToken, pxr::VtValue>& params,
                           const pxr::SdfPath& nodePath)
{
    static const pxr::TfToken tTexture("texture");
    static const pxr::TfToken tFile("file");
    static const pxr::TfToken tUvTiling("uvtiling");
    static const pxr::TfToken tUvOffset("uvoffset");
    static const pxr::TfToken tScale("scale");
    static const pxr::TfToken tOffset("offset");
    if (params.find(tTexture) == params.end()) {
        auto fileIt = params.find(tFile);
        if (fileIt != params.end()) {
            params[tTexture] = fileIt->second;
            Logger::debug(nodePath, ": bridged image parameter file -> texture");
        }
    }
    if (isMaterialXTiledImageIdentifier(identifier)) {
        if (params.find(tScale) == params.end()) {
            auto tilingIt = params.find(tUvTiling);
            if (tilingIt != params.end()) {
                params[tScale] = tilingIt->second;
                Logger::debug(nodePath, ": bridged tiledimage parameter uvtiling -> scale");
            }
        }
        if (params.find(tOffset) == params.end()) {
            auto offsetIt = params.find(tUvOffset);
            if (offsetIt != params.end()) {
                params[tOffset] = offsetIt->second;
                Logger::debug(nodePath, ": bridged tiledimage parameter uvoffset -> offset");
            }
        }
    }
    applyImageMapWrapAroundMapping(params, nodePath);
}

void
applyMaterialXTexcoordLikeMapping(const pxr::HdMaterialNode& node,
                                  std::map<pxr::TfToken, pxr::VtValue>& params)
{
    static const pxr::TfToken tVarName("varname");
    static const pxr::TfToken tGeomProp("geomprop");
    static const pxr::TfToken tAttribute("attribute");
    static const pxr::TfToken tDefault("default");
    static const pxr::TfToken tFallback("fallback");

    std::string varname("st");
    auto useStringParam = [&](const pxr::TfToken& key) {
        auto it = params.find(key);
        if (it == params.end()) {
            return;
        }
        std::string value;
        if (extractStringValue(it->second, value) && !value.empty()) {
            varname = value;
        }
    };
    useStringParam(tGeomProp);
    useStringParam(tVarName);
    useStringParam(tAttribute);

    params[tVarName] = pxr::VtValue(varname);
    Logger::debug(node.path, ": bridged texcoord node varname='", varname, "'");

    if (params.find(tFallback) == params.end()) {
        auto defaultIt = params.find(tDefault);
        if (defaultIt != params.end()) {
            params[tFallback] = defaultIt->second;
        }
    }
}

void
applyMaterialXPlace2dMapping(const pxr::HdMaterialNode& node,
                             std::map<pxr::TfToken, pxr::VtValue>& params)
{
    static const pxr::TfToken tIn("in");
    static const pxr::TfToken tTexcoord("texcoord");
    static const pxr::TfToken tInput("input");
    static const pxr::TfToken tCoord("coord");
    static const pxr::TfToken tRotation("rotation");
    static const pxr::TfToken tRotate("rotate");
    static const pxr::TfToken tTranslation("translation");
    static const pxr::TfToken tTranslate("translate");
    static const pxr::TfToken tOffset("offset");
    static const pxr::TfToken tPivot("pivot");
    static const pxr::TfToken tOperationOrder("operationorder");

    if (params.find(tIn) == params.end()) {
        if (params.find(tTexcoord) != params.end()) {
            params[tIn] = params[tTexcoord];
        } else if (params.find(tInput) != params.end()) {
            params[tIn] = params[tInput];
        } else if (params.find(tCoord) != params.end()) {
            params[tIn] = params[tCoord];
        }
    }
    if (params.find(tRotation) == params.end()) {
        auto rotateIt = params.find(tRotate);
        if (rotateIt != params.end()) {
            params[tRotation] = rotateIt->second;
        }
    }
    if (params.find(tTranslation) == params.end()) {
        if (params.find(tTranslate) != params.end()) {
            params[tTranslation] = params[tTranslate];
        } else if (params.find(tOffset) != params.end()) {
            params[tTranslation] = params[tOffset];
        }
    }
    if (params.find(tPivot) != params.end()) {
        Logger::debug(node.path, ": place2d pivot authored but not fully represented in v2 bridge");
    }
    if (params.find(tOperationOrder) != params.end()) {
        Logger::debug(node.path, ": place2d operationorder authored but not fully represented in v2 bridge");
    }

    Logger::debug(node.path, ": bridged place2d node params for UsdTransform2d");
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
                           const pxr::SdfPath& outputId,
                           const scene_rdl2::rdl2::SceneObject* input)
{
    static const pxr::TfToken tBaseColor("base_color");
    static const pxr::TfToken tIn("in");
    static const pxr::TfToken tOut("out");
    auto isUvLikeInput = [input]() {
        if (!input) {
            return false;
        }
        const std::string inputClassName = input->getSceneClass().getName();
        return inputClassName == "UsdTransform2d" ||
               inputClassName == "UsdPrimvarReader_float2";
    };

    if (outputName.IsEmpty()) {
        Logger::debug(outputId, ": relationship outputName is empty");
        if (output && output->getSceneClass().getName() == "ImageMap" && isUvLikeInput()) {
            Logger::debug(outputId, ": empty outputName UV fallback -> input_texture_coordinates");
            return "input_texture_coordinates";
        }
        if (output && output->getSceneClass().getName() == "UsdTransform2d" && isUvLikeInput()) {
            Logger::debug(outputId, ": empty outputName UV fallback -> in");
            return "in";
        }
    }

    if (output &&
        output->getSceneClass().getName() == "DwaBaseMaterial") {
        if (outputName == tBaseColor) {
            Logger::debug(outputId, ": remapped output attribute base_color -> albedo");
            return "albedo";
        }
    }
    if (output &&
        output->getSceneClass().getName() == "ImageMap") {
        const std::string outputNameLower = toLower(outputName.GetString());
        if (outputNameLower == "st" || outputNameLower == "texcoord" ||
            outputNameLower == "uv" || outputNameLower == "in" ||
            outputNameLower == "coord") {
            Logger::debug(outputId, ": remapped image coord input ", outputName, " -> input_texture_coordinates");
            return "input_texture_coordinates";
        }
    }
    if (output &&
        output->getSceneClass().getName() == "UsdTransform2d") {
        const std::string outputNameLower = toLower(outputName.GetString());
        if (outputName == tOut || outputNameLower == "st" || outputNameLower == "texcoord" ||
            outputNameLower == "uv" || outputNameLower == "coord") {
            Logger::debug(outputId, ": remapped place2d input ", outputName, " -> in");
            return "in";
        }
    }
    return outputName.GetString();
}

void
configureImageMapTexcoordMode(scene_rdl2::rdl2::SceneObject* output,
                              const std::string& outputAttributeName,
                              const pxr::SdfPath& outputId,
                              scene_rdl2::rdl2::SceneObject* input,
                              const pxr::SdfPath& inputId)
{
    if (!output || output->getSceneClass().getName() != "ImageMap" ||
        outputAttributeName != "input_texture_coordinates") {
        return;
    }
    if (input) {
        Logger::debug(outputId, ": binding ImageMap.input_texture_coordinates <- ",
                      inputId, " (", input->getSceneClass().getName(), ")");
    } else {
        Logger::debug(outputId, ": binding ImageMap.input_texture_coordinates <- ",
                      inputId, " (input scene object missing)");
    }
    const scene_rdl2::rdl2::Attribute* textureCoordinatesAttr =
        output->getSceneClass().getAttribute("texture_coordinates");
    if (!textureCoordinatesAttr ||
        textureCoordinatesAttr->getType() != scene_rdl2::rdl2::TYPE_INT) {
        return;
    }
    const auto textureCoordinatesKey =
        output->getSceneClass().getAttributeKey<Int>("texture_coordinates");
    const int enumValue =
        output->getSceneClass().getEnumValue(textureCoordinatesKey, "input texture coordinates");
    output->set(textureCoordinatesKey, enumValue);
    Logger::debug(outputId, ": set ImageMap.texture_coordinates = input texture coordinates");
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
                                     std::map<pxr::TfToken, pxr::VtValue>& params)
{
    static const pxr::TfToken tBaseColor("base_color");
    static const pxr::TfToken tBase("base");
    static const pxr::TfToken tSpecular("specular");
    static const pxr::TfToken tSpecularRoughness("specular_roughness");
    static const pxr::TfToken tMetalness("metalness");
    static const pxr::TfToken tTransmission("transmission");
    static const pxr::TfToken tOpacity("opacity");
    static const pxr::TfToken tIor("ior");
    static const pxr::TfToken tEmissionColor("emission_color");
    static const pxr::TfToken tEmission("emission");

    static const pxr::TfToken tAlbedo("albedo");
    static const pxr::TfToken tRoughness("roughness");
    static const pxr::TfToken tMetallic("metallic");
    static const pxr::TfToken tPresence("presence");
    static const pxr::TfToken tRefractiveIndex("refractive_index");

    setIfPresent(params, node.parameters, tSpecular, tSpecular);
    setIfPresent(params, node.parameters, tSpecularRoughness, tRoughness);
    setIfPresent(params, node.parameters, tMetalness, tMetallic);
    setIfPresent(params, node.parameters, tTransmission, tTransmission);
    setIfPresent(params, node.parameters, tIor, tRefractiveIndex);

    auto opacityIt = node.parameters.find(tOpacity);
    if (opacityIt != node.parameters.end()) {
        pxr::GfVec3f opacityRgb;
        float opacityScalar = 1.0f;
        if (getRgb(opacityIt->second, opacityRgb)) {
            opacityScalar = (opacityRgb[0] + opacityRgb[1] + opacityRgb[2]) / 3.0f;
            params[tPresence] = pxr::VtValue(opacityScalar);
        }
    }

    pxr::GfVec3f baseColor(1.0f);
    auto baseColorIt = node.parameters.find(tBaseColor);
    bool hasBaseColor = baseColorIt != node.parameters.end() && getRgb(baseColorIt->second, baseColor);
    if (hasBaseColor) {
        float baseFactor = 1.0f;
        auto baseIt = node.parameters.find(tBase);
        if (baseIt != node.parameters.end()) {
            float tmp;
            if (getScalar(baseIt->second, tmp)) {
                baseFactor = tmp;
            }
        }
        params[tAlbedo] = pxr::VtValue(baseColor * baseFactor);
    }

    pxr::GfVec3f emissionColor(0.0f);
    auto emissionColorIt = node.parameters.find(tEmissionColor);
    bool hasEmissionColor = emissionColorIt != node.parameters.end() && getRgb(emissionColorIt->second, emissionColor);
    if (hasEmissionColor) {
        float emissionFactor = 1.0f;
        auto emissionIt = node.parameters.find(tEmission);
        if (emissionIt != node.parameters.end()) {
            float tmp;
            if (getScalar(emissionIt->second, tmp)) {
                emissionFactor = tmp;
            }
        }
        params[tEmission] = pxr::VtValue(emissionColor * emissionFactor);
    }
}

pxr::TfToken
resolveSurfaceTerminal(const pxr::HdMaterialNetworkMap& networkmap,
                       const pxr::TfToken& requestedTerminal)
{
    if (requestedTerminal != pxr::HdMaterialTerminalTokens->surface) {
        return requestedTerminal;
    }
    if (networkmap.map.find(requestedTerminal) != networkmap.map.end()) {
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
    const pxr::HdRprim* geom
) {
    std::string className = node.identifier.GetString();
    const bool isMaterialXStandardSurface = isMaterialXStandardSurfaceIdentifier(node.identifier);
    const bool isMaterialXImageOrTiled = isMaterialXImageOrTiledIdentifier(node.identifier);
    const bool isMaterialXTexcoordLike = isMaterialXTexcoordLikeIdentifier(node.identifier);
    const bool isMaterialXPlace2d = isMaterialXPlace2dIdentifier(node.identifier);
    const bool isImageLikeButNotAllowed =
        looksLikeImageIdentifier(node.identifier) &&
        !isMaterialXImageOrTiled &&
        !isUsdUVTextureIdentifier(node.identifier);

    if (className == "BaseMaterial") {
        className = "DwaBaseMaterial";
        Logger::info(node.path, ": aliased BaseMaterial -> DwaBaseMaterial");
    } else if (isMaterialXStandardSurface) {
        className = "DwaBaseMaterial";
        Logger::info(node.path, ": bridged MaterialX standard_surface -> DwaBaseMaterial");
    } else if (isMaterialXImageOrTiled) {
        className = "ImageMap";
        Logger::debug(node.path, ": bridged image node '", node.identifier, "' -> ImageMap");
    } else if (isMaterialXTexcoordLike) {
        className = "UsdPrimvarReader_float2";
        Logger::debug(node.path, ": bridged texcoord node '", node.identifier, "' -> UsdPrimvarReader_float2");
    } else if (isMaterialXPlace2d) {
        className = "UsdTransform2d";
        Logger::debug(node.path, ": bridged place2d node '", node.identifier, "' -> UsdTransform2d");
    } else if (isImageLikeButNotAllowed) {
        Logger::debug(node.path, ": image-like node id not allowlisted: '", node.identifier, "'");
    }

    std::map<pxr::TfToken, pxr::VtValue> params(node.parameters.begin(), node.parameters.end());
    if (isMaterialXImageOrTiled) {
        applyMaterialXImageMapping(node.identifier, params, node.path);
    }
    if (isMaterialXTexcoordLike) {
        applyMaterialXTexcoordLikeMapping(node, params);
    }
    if (isMaterialXPlace2d) {
        applyMaterialXPlace2dMapping(node, params);
    }
    if (isMaterialXStandardSurface) {
        applyMaterialXStandardSurfaceMapping(node, params);
    }

    if (className == "ImageMap") {
        debugLogSelectedParams(node.path, "ImageMap",
                               params,
                               {pxr::TfToken("file"), pxr::TfToken("texture"),
                                pxr::TfToken("texcoord"), pxr::TfToken("uvtiling"),
                                pxr::TfToken("uvoffset"), pxr::TfToken("scale"),
                                pxr::TfToken("offset"), pxr::TfToken("wrap_around")});
    } else if (className == "UsdTransform2d") {
        debugLogSelectedParams(node.path, "UsdTransform2d",
                               params,
                               {pxr::TfToken("texcoord"), pxr::TfToken("in"),
                                pxr::TfToken("scale"), pxr::TfToken("rotate"),
                                pxr::TfToken("rotation"), pxr::TfToken("offset"),
                                pxr::TfToken("translation"), pxr::TfToken("pivot"),
                                pxr::TfToken("operationorder")});
    } else if (className == "UsdPrimvarReader_float2") {
        debugLogSelectedParams(node.path, "UsdPrimvarReader_float2",
                               params,
                               {pxr::TfToken("varname"), pxr::TfToken("geomprop"),
                                pxr::TfToken("attribute"), pxr::TfToken("default"),
                                pxr::TfToken("fallback")});
    }

    SceneObject* shaderObj = renderDelegate.createSceneObject(className, nodeName);
    if (shaderObj) {
        try {
            SceneObject::UpdateGuard guard(shaderObj);
            const SceneClass& sceneClass = shaderObj->getSceneClass();
            bool isUsdTransform2dClass = (sceneClass.getName() == "UsdTransform2d");
            const bool hasInParam = params.find(pxr::TfToken("in")) != params.end();
            const bool hasScaleParam = params.find(pxr::TfToken("scale")) != params.end();
            const bool hasTranslationParam = params.find(pxr::TfToken("translation")) != params.end();
            const bool hasRotationParam = params.find(pxr::TfToken("rotation")) != params.end();
            bool appliedIn = false;
            bool appliedScale = false;
            bool appliedTranslation = false;
            bool appliedRotation = false;
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
                        if (isUsdTransform2dClass) {
                            if (attrName == "in") {
                                appliedIn = true;
                            } else if (attrName == "scale") {
                                appliedScale = true;
                            } else if (attrName == "translation") {
                                appliedTranslation = true;
                            } else if (attrName == "rotation") {
                                appliedRotation = true;
                            }
                        }
                    } else {
                        hdMoonray::ValueConverter::setDefault(shaderObj, attribute);
                    }
                }
            }
            if (isUsdTransform2dClass) {
                Logger::debug(node.path, ": UsdTransform2d set status in(authored/applied)=",
                              hasInParam ? "Y" : "N", "/", appliedIn ? "Y" : "N",
                              " scale=", hasScaleParam ? "Y" : "N", "/", appliedScale ? "Y" : "N",
                              " translation=", hasTranslationParam ? "Y" : "N", "/", appliedTranslation ? "Y" : "N",
                              " rotation=", hasRotationParam ? "Y" : "N", "/", appliedRotation ? "Y" : "N");
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
    auto i = networkmap.map.find(terminalName);
    if (i == networkmap.map.end()) {
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
    static const pxr::TfToken tOut("out");
    for (const pxr::HdMaterialRelationship& rel : network.relationships) {
        const pxr::TfToken inputName = rel.inputName.IsEmpty() ? tOut : rel.inputName;
        if (rel.inputName.IsEmpty()) {
            Logger::debug(rel.inputId, ": relationship inputName is empty, normalizing to 'out'");
        }
        const std::string inputChannel = inputName.GetString();

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
                                         geom);
            }
        } else {
            // If the node isn't in the nodeChannelMap (i.e. materials)
            // then only one node is created using the USD path as the name.
            next = makeMoonrayShader(renderDelegate,
                                     sceneDelegate,
                                     node,
                                     moonrayNodeName,
                                     geom);
        }

        if (next) last = next; // HDM-368 : don't give up on error
    }

    // set bindings (fixme: only works for Moonray shaders)
    for (const pxr::HdMaterialRelationship& rel : network.relationships) {
        const pxr::TfToken relInputName = rel.inputName.IsEmpty() ? tOut : rel.inputName;
        if (rel.inputName.IsEmpty()) {
            Logger::debug(rel.inputId, ": relationship inputName is empty, normalizing to 'out'");
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
                                           relInputName.GetString());

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
                    const std::string channel = relInputName.GetString();
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
                        resolveOutputAttributeName(output, rel.outputName, rel.outputId, input);

                    const scene_rdl2::rdl2::Attribute* attribute(
                        output->getSceneClass().getAttribute(outputAttributeName));
                    if (!attribute) {
                        Logger::debug(rel.outputId, ": skipping connection: rel.outputName='",
                                      rel.outputName, "' resolved='", outputAttributeName,
                                      "' scene class=", output->getSceneClass().getName());
                        continue;
                    }
                    configureImageMapTexcoordMode(output, outputAttributeName, rel.outputId, input, rel.inputId);

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
                    resolveOutputAttributeName(output, rel.outputName, rel.outputId, input);

                const scene_rdl2::rdl2::Attribute* attribute(
                    output->getSceneClass().getAttribute(outputAttributeName));
                if (!attribute) {
                    Logger::debug(rel.outputId, ": skipping connection: rel.outputName='",
                                  rel.outputName, "' resolved='", outputAttributeName,
                                  "' scene class=", output->getSceneClass().getName());
                    continue;
                }
                configureImageMapTexcoordMode(output, outputAttributeName, rel.outputId, input, rel.inputId);

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

    if (not last && terminalName == pxr::HdMaterialTerminalTokens->surface) last = renderDelegate.errorMaterial();
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
