#include "Max.h"
#include "impexp.h"
#include "istdplug.h"
#include "stdmat.h"
#include "triobj.h"
#include "dummy.h"
#include "modstack.h"
#include "resource.h"
#include "MorpherApi.h"
#include "imorpher.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "vendor/tiny_gltf.h"

HINSTANCE hInstance;

namespace {

namespace fs = std::filesystem;

constexpr int kPositionCtrlClass = LININTERP_POSITION_CLASS_ID;
constexpr int kRotationCtrlClass = LININTERP_ROTATION_CLASS_ID;
constexpr int kScaleCtrlClass = LININTERP_SCALE_CLASS_ID;

const Class_ID kImporterClassId(0x3d90b4b1, 0x6549af2);
const float kDefaultScale = 1.0f;

struct LocalTransform {
    Point3 translation = Point3::Origin;
    Quat rotation = IdentQuat();
    Point3 scale = Point3(1.0f, 1.0f, 1.0f);
};

struct Vec3KeyTrack {
    bool present = false;
    std::vector<TimeValue> times;
    std::vector<Point3> values;
};

struct QuatKeyTrack {
    bool present = false;
    std::vector<TimeValue> times;
    std::vector<Quat> values;
};

struct WeightKeyTrack {
    bool present = false;
    std::vector<TimeValue> times;
    std::vector<std::vector<float>> values;
};

struct NodeAnimation {
    Vec3KeyTrack translation;
    Vec3KeyTrack scale;
    QuatKeyTrack rotation;
    WeightKeyTrack weights;
};

struct MorphBinding {
    INode* baseNode = nullptr;
    Modifier* morpher = nullptr;
    size_t channelCount = 0;
    std::vector<float> defaultWeights;
};

struct NodeRecord {
    INode* anchor = nullptr;
    std::vector<MorphBinding> morphBindings;
};

TCHAR* GetString(int id) {
    static TCHAR buf[256];
    if (hInstance) {
        return LoadString(hInstance, id, buf, _countof(buf)) ? buf : nullptr;
    }
    return nullptr;
}

std::string SanitizeFileName(const std::string& value, const std::string& fallback) {
    std::string result = value.empty() ? fallback : value;
    for (char& ch : result) {
        if (ch == '\\' || ch == '/' || ch == ':' || ch == '*' || ch == '?' || ch == '"' ||
            ch == '<' || ch == '>' || ch == '|') {
            ch = '_';
        }
    }
    return result;
}

TSTR ToTSTR(const std::string& value) {
    return MSTR::FromUTF8(value.c_str());
}

TimeValue SecondsToTicks(double seconds) {
    return static_cast<TimeValue>(seconds * GetFrameRate() * GetTicksPerFrame());
}

Point3 ConvertPoint(double x, double y, double z) {
    return Point3(
        static_cast<float>(x * kDefaultScale),
        static_cast<float>(z * kDefaultScale),
        static_cast<float>(-y * kDefaultScale));
}

Point3 ConvertScale(double x, double y, double z) {
    return Point3(static_cast<float>(x), static_cast<float>(z), static_cast<float>(y));
}

Quat ConvertQuat(double x, double y, double z, double w) {
    Quat q(static_cast<float>(x), static_cast<float>(z), static_cast<float>(-y), static_cast<float>(w));
    q.Normalize();
    return q;
}

double VectorLength3(double x, double y, double z) {
    return std::sqrt((x * x) + (y * y) + (z * z));
}

Quat RotationMatrixToQuat(
    double m00, double m01, double m02,
    double m10, double m11, double m12,
    double m20, double m21, double m22) {
    double trace = m00 + m11 + m22;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 1.0;

    if (trace > 0.0) {
        const double s = std::sqrt(trace + 1.0) * 2.0;
        w = 0.25 * s;
        x = (m21 - m12) / s;
        y = (m02 - m20) / s;
        z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
        w = (m21 - m12) / s;
        x = 0.25 * s;
        y = (m01 + m10) / s;
        z = (m02 + m20) / s;
    } else if (m11 > m22) {
        const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
        w = (m02 - m20) / s;
        x = (m01 + m10) / s;
        y = 0.25 * s;
        z = (m12 + m21) / s;
    } else {
        const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
        w = (m10 - m01) / s;
        x = (m02 + m20) / s;
        y = (m12 + m21) / s;
        z = 0.25 * s;
    }

    Quat q(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(w));
    q.Normalize();
    return q;
}

LocalTransform DecomposeNodeMatrix(const std::vector<double>& matrix) {
    LocalTransform result;
    if (matrix.size() != 16) {
        return result;
    }

    const double tx = matrix[12];
    const double ty = matrix[13];
    const double tz = matrix[14];

    double x0 = matrix[0];
    double x1 = matrix[1];
    double x2 = matrix[2];
    double y0 = matrix[4];
    double y1 = matrix[5];
    double y2 = matrix[6];
    double z0 = matrix[8];
    double z1 = matrix[9];
    double z2 = matrix[10];

    double sx = VectorLength3(x0, x1, x2);
    double sy = VectorLength3(y0, y1, y2);
    double sz = VectorLength3(z0, z1, z2);

    if (sx <= 1e-8) {
        sx = 1.0;
    }
    if (sy <= 1e-8) {
        sy = 1.0;
    }
    if (sz <= 1e-8) {
        sz = 1.0;
    }

    x0 /= sx; x1 /= sx; x2 /= sx;
    y0 /= sy; y1 /= sy; y2 /= sy;
    z0 /= sz; z1 /= sz; z2 /= sz;

    const Quat gltfRotation = RotationMatrixToQuat(
        x0, y0, z0,
        x1, y1, z1,
        x2, y2, z2);

    result.translation = ConvertPoint(tx, ty, tz);
    result.rotation = ConvertQuat(gltfRotation.x, gltfRotation.y, gltfRotation.z, gltfRotation.w);
    result.scale = ConvertScale(sx, sy, sz);
    return result;
}

LocalTransform GetNodeTransform(const tinygltf::Node& node) {
    if (node.matrix.size() == 16) {
        return DecomposeNodeMatrix(node.matrix);
    }

    LocalTransform transform;
    if (node.translation.size() == 3) {
        transform.translation = ConvertPoint(
            node.translation[0], node.translation[1], node.translation[2]);
    }
    if (node.rotation.size() == 4) {
        transform.rotation = ConvertQuat(
            node.rotation[0], node.rotation[1], node.rotation[2], node.rotation[3]);
    }
    if (node.scale.size() == 3) {
        transform.scale = ConvertScale(node.scale[0], node.scale[1], node.scale[2]);
    }
    return transform;
}

bool IsImageUriExternal(const std::string& uri) {
    return !uri.empty() && uri.rfind("data:", 0) != 0;
}

double ReadNumericComponent(const unsigned char* ptr, int componentType) {
    switch (componentType) {
    case TINYGLTF_COMPONENT_TYPE_BYTE:
        return static_cast<double>(*reinterpret_cast<const int8_t*>(ptr));
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        return static_cast<double>(*reinterpret_cast<const uint8_t*>(ptr));
    case TINYGLTF_COMPONENT_TYPE_SHORT:
        return static_cast<double>(*reinterpret_cast<const int16_t*>(ptr));
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        return static_cast<double>(*reinterpret_cast<const uint16_t*>(ptr));
    case TINYGLTF_COMPONENT_TYPE_INT:
        return static_cast<double>(*reinterpret_cast<const int32_t*>(ptr));
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        return static_cast<double>(*reinterpret_cast<const uint32_t*>(ptr));
    case TINYGLTF_COMPONENT_TYPE_FLOAT:
        return static_cast<double>(*reinterpret_cast<const float*>(ptr));
    case TINYGLTF_COMPONENT_TYPE_DOUBLE:
        return *reinterpret_cast<const double*>(ptr);
    default:
        return 0.0;
    }
}

bool ReadAccessorRaw(
    const tinygltf::Model& model,
    int accessorIndex,
    int expectedType,
    std::vector<double>& outValues) {
    if (accessorIndex < 0 || accessorIndex >= static_cast<int>(model.accessors.size())) {
        return false;
    }

    const tinygltf::Accessor& accessor = model.accessors[accessorIndex];
    if (accessor.type != expectedType) {
        return false;
    }
    if (accessor.bufferView < 0 || accessor.bufferView >= static_cast<int>(model.bufferViews.size())) {
        return false;
    }

    const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
    if (bufferView.buffer < 0 || bufferView.buffer >= static_cast<int>(model.buffers.size())) {
        return false;
    }

    const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];
    const int componentCount = tinygltf::GetNumComponentsInType(accessor.type);
    const int componentSize = tinygltf::GetComponentSizeInBytes(accessor.componentType);
    const size_t stride = accessor.ByteStride(bufferView) != 0
        ? accessor.ByteStride(bufferView)
        : static_cast<size_t>(componentCount * componentSize);
    const size_t baseOffset = bufferView.byteOffset + accessor.byteOffset;

    if (baseOffset >= buffer.data.size()) {
        return false;
    }

    outValues.clear();
    outValues.reserve(static_cast<size_t>(accessor.count) * componentCount);

    for (size_t elementIndex = 0; elementIndex < accessor.count; ++elementIndex) {
        const unsigned char* element = buffer.data.data() + baseOffset + (elementIndex * stride);
        for (int componentIndex = 0; componentIndex < componentCount; ++componentIndex) {
            outValues.push_back(
                ReadNumericComponent(element + (componentIndex * componentSize), accessor.componentType));
        }
    }

    return true;
}

std::vector<int> ReadIndices(const tinygltf::Model& model, const tinygltf::Primitive& primitive, size_t vertexCount) {
    std::vector<int> indices;
    if (primitive.indices < 0) {
        indices.reserve(vertexCount);
        for (size_t i = 0; i < vertexCount; ++i) {
            indices.push_back(static_cast<int>(i));
        }
        return indices;
    }

    std::vector<double> raw;
    if (!ReadAccessorRaw(model, primitive.indices, TINYGLTF_TYPE_SCALAR, raw)) {
        return indices;
    }

    indices.reserve(raw.size());
    for (double value : raw) {
        indices.push_back(static_cast<int>(value));
    }
    return indices;
}

std::vector<int> ConvertPrimitiveToTriangles(const tinygltf::Primitive& primitive, const std::vector<int>& indices) {
    std::vector<int> triangles;
    const int mode = primitive.mode == -1 ? TINYGLTF_MODE_TRIANGLES : primitive.mode;

    if (mode == TINYGLTF_MODE_TRIANGLES) {
        triangles = indices;
        return triangles;
    }

    if (mode == TINYGLTF_MODE_TRIANGLE_STRIP) {
        for (size_t i = 0; i + 2 < indices.size(); ++i) {
            int a = indices[i];
            int b = indices[i + 1];
            int c = indices[i + 2];
            if (a == b || b == c || a == c) {
                continue;
            }
            if ((i % 2) == 0) {
                triangles.push_back(a);
                triangles.push_back(b);
                triangles.push_back(c);
            } else {
                triangles.push_back(a);
                triangles.push_back(c);
                triangles.push_back(b);
            }
        }
        return triangles;
    }

    if (mode == TINYGLTF_MODE_TRIANGLE_FAN) {
        for (size_t i = 1; i + 1 < indices.size(); ++i) {
            triangles.push_back(indices[0]);
            triangles.push_back(indices[i]);
            triangles.push_back(indices[i + 1]);
        }
        return triangles;
    }

    return triangles;
}

template <typename TValue>
void SetLinearKeys(INode* node, SClass_ID superClass, Class_ID classId, const std::vector<TValue>& values);

void AttachNodeToParent(INode* node, INode* parent) {
    if (node != nullptr && parent != nullptr && node != parent) {
        parent->AttachChild(node, 1);
    }
}

void ApplyPositionKeys(INode* node, const std::vector<TimeValue>& times, const std::vector<Point3>& values) {
    if (node == nullptr || times.empty() || times.size() != values.size()) {
        return;
    }

    Control* controller = static_cast<Control*>(
        CreateInstance(CTRL_POSITION_CLASS_ID, Class_ID(kPositionCtrlClass, 0)));
    if (controller == nullptr) {
        return;
    }

    node->GetTMController()->SetPositionController(controller);
    IKeyControl* keyControl = GetKeyControlInterface(controller);
    if (keyControl == nullptr) {
        return;
    }

    keyControl->SetNumKeys(static_cast<int>(times.size()));
    for (size_t i = 0; i < times.size(); ++i) {
        ILinPoint3Key key;
        key.time = times[i];
        key.flags = 0;
        key.val = values[i];
        keyControl->SetKey(static_cast<int>(i), &key);
    }
    keyControl->SortKeys();
}

void ApplyRotationKeys(INode* node, const std::vector<TimeValue>& times, const std::vector<Quat>& values) {
    if (node == nullptr || times.empty() || times.size() != values.size()) {
        return;
    }

    Control* controller = static_cast<Control*>(
        CreateInstance(CTRL_ROTATION_CLASS_ID, Class_ID(kRotationCtrlClass, 0)));
    if (controller == nullptr) {
        return;
    }

    node->GetTMController()->SetRotationController(controller);
    IKeyControl* keyControl = GetKeyControlInterface(controller);
    if (keyControl == nullptr) {
        return;
    }

    keyControl->SetNumKeys(static_cast<int>(times.size()));
    for (size_t i = 0; i < times.size(); ++i) {
        ILinRotKey key;
        key.time = times[i];
        key.flags = 0;
        key.val = values[i];
        keyControl->SetKey(static_cast<int>(i), &key);
    }
    keyControl->SortKeys();
}

void ApplyScaleKeys(INode* node, const std::vector<TimeValue>& times, const std::vector<Point3>& values) {
    if (node == nullptr || times.empty() || times.size() != values.size()) {
        return;
    }

    Control* controller = static_cast<Control*>(
        CreateInstance(CTRL_SCALE_CLASS_ID, Class_ID(kScaleCtrlClass, 0)));
    if (controller == nullptr) {
        return;
    }

    node->GetTMController()->SetScaleController(controller);
    IKeyControl* keyControl = GetKeyControlInterface(controller);
    if (keyControl == nullptr) {
        return;
    }

    keyControl->SetNumKeys(static_cast<int>(times.size()));
    for (size_t i = 0; i < times.size(); ++i) {
        ILinScaleKey key;
        key.time = times[i];
        key.flags = 0;
        key.val = ScaleValue(values[i]);
        keyControl->SetKey(static_cast<int>(i), &key);
    }
    keyControl->SortKeys();
}

void ApplyNodeTransform(INode* node, const LocalTransform& defaults, const NodeAnimation& animation) {
    std::vector<TimeValue> posTimes;
    std::vector<Point3> posValues;
    if (animation.translation.present) {
        posTimes = animation.translation.times;
        posValues = animation.translation.values;
    } else {
        posTimes.push_back(0);
        posValues.push_back(defaults.translation);
    }

    std::vector<TimeValue> rotTimes;
    std::vector<Quat> rotValues;
    if (animation.rotation.present) {
        rotTimes = animation.rotation.times;
        rotValues = animation.rotation.values;
    } else {
        rotTimes.push_back(0);
        rotValues.push_back(defaults.rotation);
    }

    std::vector<TimeValue> scaleTimes;
    std::vector<Point3> scaleValues;
    if (animation.scale.present) {
        scaleTimes = animation.scale.times;
        scaleValues = animation.scale.values;
    } else {
        scaleTimes.push_back(0);
        scaleValues.push_back(defaults.scale);
    }

    ApplyPositionKeys(node, posTimes, posValues);
    ApplyRotationKeys(node, rotTimes, rotValues);
    ApplyScaleKeys(node, scaleTimes, scaleValues);
}

IDerivedObject* EnsureDerivedObject(INode* node) {
    if (node == nullptr) {
        return nullptr;
    }

    Object* object = node->GetObjectRef();
    if (object == nullptr) {
        return nullptr;
    }

    if (object->SuperClassID() == GEN_DERIVOB_CLASS_ID) {
        return static_cast<IDerivedObject*>(object);
    }

    IDerivedObject* derived = CreateDerivedObject(object);
    if (derived == nullptr) {
        return nullptr;
    }

    node->SetObjectRef(derived);
    return derived;
}

class GLTFImport : public SceneImport {
public:
    GLTFImport() = default;
    ~GLTFImport() override = default;

    int ExtCount() override { return 2; }

    const TCHAR* Ext(int n) override {
        switch (n) {
        case 0: return _T("gltf");
        case 1: return _T("glb");
        default: return _T("");
        }
    }

    const TCHAR* LongDesc() override { return GetString(IDS_IMPORTER_FILE); }
    const TCHAR* ShortDesc() override { return GetString(IDS_IMPORTER_SHORT); }
    const TCHAR* AuthorName() override { return GetString(IDS_IMPORTER_AUTHOR); }
    const TCHAR* CopyrightMessage() override { return GetString(IDS_IMPORTER_COPYRIGHT); }
    const TCHAR* OtherMessage1() override { return _T("Imports mesh geometry and node TRS animation."); }
    const TCHAR* OtherMessage2() override { return _T("First animation clip only in this initial version."); }
    unsigned int Version() override { return 100; }
    void ShowAbout(HWND) override {}

    int DoImport(const TCHAR* filename, ImpInterface* i, Interface* gi, BOOL suppressPrompts) override;

private:
    bool LoadModel(const fs::path& filename);
    void BuildMaterials();
    Mtl* CreateMaterial(int materialIndex);
    fs::path ResolveTexturePath(int textureIndex);
    void CollectAnimations();
    void ImportScene();
    void ImportNodeRecursive(int nodeIndex, INode* parent);
    INode* CreateAnchorNode(const tinygltf::Node& node, int nodeIndex, INode* parent);
    INode* CreateDummyNode(const TSTR& name, INode* parent);
    INode* CreateMeshNode(
        const tinygltf::Primitive& primitive,
        const TSTR& name,
        INode* parent,
        const std::vector<Point3>* overridePositions = nullptr,
        bool hidden = false);
    bool ReadPositions(const tinygltf::Primitive& primitive, std::vector<Point3>& positions) const;
    bool ReadMorphTargetPositions(
        const tinygltf::Primitive& primitive,
        int morphTargetIndex,
        std::vector<Point3>& positions) const;
    bool ReadUVs(const tinygltf::Primitive& primitive, std::vector<UVVert>& uvs) const;
    bool ReadAnimationSamplerTimes(int accessorIndex, std::vector<TimeValue>& times) const;
    bool ReadAnimationVec3Values(
        int accessorIndex,
        const std::string& interpolation,
        std::vector<Point3>& values,
        bool convertScale) const;
    bool ReadAnimationQuatValues(
        int accessorIndex,
        const std::string& interpolation,
        std::vector<Quat>& values) const;
    bool ReadAnimationWeightValues(
        int accessorIndex,
        const std::string& interpolation,
        size_t targetCount,
        std::vector<std::vector<float>>& values) const;
    size_t GetMorphTargetCountForNode(int nodeIndex) const;
    std::vector<float> GetDefaultMorphWeightsForNode(int nodeIndex) const;
    void SetupMorphTargetsForNode(
        int nodeIndex,
        const tinygltf::Primitive& primitive,
        INode* baseNode,
        const TSTR& name,
        NodeRecord& record);
    void ApplyMorphAnimation(NodeRecord& record, const WeightKeyTrack& animation);
    TSTR MakeNodeName(const tinygltf::Node& node, int nodeIndex, const TCHAR* suffix = _T("")) const;

private:
    tinygltf::Model model_;
    ImpInterface* imp_ = nullptr;
    Interface* ip_ = nullptr;
    fs::path sourceFile_;
    fs::path sourceDir_;
    fs::path textureDir_;
    std::vector<Mtl*> materials_;
    std::vector<NodeAnimation> nodeAnimations_;
    std::vector<NodeRecord> nodeRecords_;
    TimeValue animationEnd_ = 0;
};

bool GLTFImport::LoadModel(const fs::path& filename) {
    tinygltf::TinyGLTF loader;
    std::string errors;
    std::string warnings;
    bool loaded = false;
    const std::string pathString = filename.string();

    if (_tcsicmp(filename.extension().c_str(), _T(".glb")) == 0) {
        loaded = loader.LoadBinaryFromFile(&model_, &errors, &warnings, pathString);
    } else {
        loaded = loader.LoadASCIIFromFile(&model_, &errors, &warnings, pathString);
    }

    if (!warnings.empty()) {
        DebugPrint(_T("glTF importer warning: %hs\n"), warnings.c_str());
    }
    if (!loaded && !errors.empty()) {
        DebugPrint(_T("glTF importer error: %hs\n"), errors.c_str());
    }

    return loaded;
}

bool GLTFImport::ReadPositions(const tinygltf::Primitive& primitive, std::vector<Point3>& positions) const {
    auto it = primitive.attributes.find("POSITION");
    if (it == primitive.attributes.end()) {
        return false;
    }

    std::vector<double> raw;
    if (!ReadAccessorRaw(model_, it->second, TINYGLTF_TYPE_VEC3, raw)) {
        return false;
    }

    positions.clear();
    positions.reserve(raw.size() / 3);
    for (size_t i = 0; i + 2 < raw.size(); i += 3) {
        positions.push_back(ConvertPoint(raw[i], raw[i + 1], raw[i + 2]));
    }
    return !positions.empty();
}

bool GLTFImport::ReadMorphTargetPositions(
    const tinygltf::Primitive& primitive,
    int morphTargetIndex,
    std::vector<Point3>& positions) const {
    if (morphTargetIndex < 0 || morphTargetIndex >= static_cast<int>(primitive.targets.size())) {
        return false;
    }

    std::vector<Point3> basePositions;
    if (!ReadPositions(primitive, basePositions) || basePositions.empty()) {
        return false;
    }

    const auto targetIt = primitive.targets[morphTargetIndex].find("POSITION");
    if (targetIt == primitive.targets[morphTargetIndex].end()) {
        return false;
    }

    std::vector<double> raw;
    if (!ReadAccessorRaw(model_, targetIt->second, TINYGLTF_TYPE_VEC3, raw)) {
        return false;
    }
    if ((raw.size() / 3) != basePositions.size()) {
        return false;
    }

    positions = basePositions;
    for (size_t i = 0; i < basePositions.size(); ++i) {
        const size_t rawIndex = i * 3;
        positions[i] += ConvertPoint(raw[rawIndex], raw[rawIndex + 1], raw[rawIndex + 2]);
    }
    return true;
}

bool GLTFImport::ReadUVs(const tinygltf::Primitive& primitive, std::vector<UVVert>& uvs) const {
    auto it = primitive.attributes.find("TEXCOORD_0");
    if (it == primitive.attributes.end()) {
        return false;
    }

    std::vector<double> raw;
    if (!ReadAccessorRaw(model_, it->second, TINYGLTF_TYPE_VEC2, raw)) {
        return false;
    }

    uvs.clear();
    uvs.reserve(raw.size() / 2);
    for (size_t i = 0; i + 1 < raw.size(); i += 2) {
        const float u = static_cast<float>(raw[i]);
        const float v = 1.0f - static_cast<float>(raw[i + 1]);
        uvs.push_back(UVVert(u, v, 0.0f));
    }
    return !uvs.empty();
}

fs::path GLTFImport::ResolveTexturePath(int textureIndex) {
    if (textureIndex < 0 || textureIndex >= static_cast<int>(model_.textures.size())) {
        return {};
    }

    const tinygltf::Texture& texture = model_.textures[textureIndex];
    if (texture.source < 0 || texture.source >= static_cast<int>(model_.images.size())) {
        return {};
    }

    const tinygltf::Image& image = model_.images[texture.source];
    if (IsImageUriExternal(image.uri)) {
        fs::path external = sourceDir_ / fs::path(image.uri);
        if (fs::exists(external)) {
            return external;
        }
    }

    if (image.image.empty() || image.width <= 0 || image.height <= 0 || image.component <= 0) {
        return {};
    }

    std::error_code ec;
    fs::create_directories(textureDir_, ec);

    const std::string baseName = SanitizeFileName(
        !image.name.empty() ? image.name : ("image_" + std::to_string(texture.source)),
        "image");
    fs::path outPath = textureDir_ / fs::path(baseName + ".png");
    if (fs::exists(outPath)) {
        return outPath;
    }

    const int stride = image.width * image.component;
    if (stbi_write_png(
            outPath.string().c_str(),
            image.width,
            image.height,
            image.component,
            image.image.data(),
            stride) == 0) {
        return {};
    }

    return outPath;
}

Mtl* GLTFImport::CreateMaterial(int materialIndex) {
    if (materialIndex < 0 || materialIndex >= static_cast<int>(model_.materials.size())) {
        return nullptr;
    }

    const tinygltf::Material& src = model_.materials[materialIndex];
    StdMat2* material = NewDefaultStdMat();
    if (material == nullptr) {
        return nullptr;
    }

    const std::string matName = src.name.empty()
        ? ("Material_" + std::to_string(materialIndex))
        : src.name;
    material->SetName(ToTSTR(matName));

    double baseColor[4] = { 1.0, 1.0, 1.0, 1.0 };
    if (src.pbrMetallicRoughness.baseColorFactor.size() >= 4) {
        for (int i = 0; i < 4; ++i) {
            baseColor[i] = src.pbrMetallicRoughness.baseColorFactor[i];
        }
    }

    material->SetDiffuse(
        Color(
            static_cast<float>(baseColor[0]),
            static_cast<float>(baseColor[1]),
            static_cast<float>(baseColor[2])),
        0);
    material->SetOpacity(static_cast<float>(baseColor[3]), 0);

    if (src.pbrMetallicRoughness.baseColorTexture.index >= 0) {
        const fs::path texturePath = ResolveTexturePath(src.pbrMetallicRoughness.baseColorTexture.index);
        if (!texturePath.empty()) {
            BitmapTex* bitmap = NewDefaultBitmapTex();
            if (bitmap != nullptr) {
                bitmap->SetMapName(texturePath.c_str());
                material->SetSubTexmap(ID_DI, bitmap);
                material->EnableMap(ID_DI, TRUE);
            }
        }
    }

    return material;
}

void GLTFImport::BuildMaterials() {
    materials_.assign(model_.materials.size(), nullptr);
    for (size_t i = 0; i < model_.materials.size(); ++i) {
        materials_[i] = CreateMaterial(static_cast<int>(i));
    }
}

bool GLTFImport::ReadAnimationSamplerTimes(int accessorIndex, std::vector<TimeValue>& times) const {
    std::vector<double> raw;
    if (!ReadAccessorRaw(model_, accessorIndex, TINYGLTF_TYPE_SCALAR, raw)) {
        return false;
    }

    times.clear();
    times.reserve(raw.size());
    for (double value : raw) {
        times.push_back(SecondsToTicks(value));
    }
    return !times.empty();
}

bool GLTFImport::ReadAnimationVec3Values(
    int accessorIndex,
    const std::string& interpolation,
    std::vector<Point3>& values,
    bool convertScaleValues) const {
    std::vector<double> raw;
    if (!ReadAccessorRaw(model_, accessorIndex, TINYGLTF_TYPE_VEC3, raw)) {
        return false;
    }

    size_t start = 0;
    size_t step = 3;
    if (interpolation == "CUBICSPLINE") {
        start = 3;
        step = 9;
    }

    values.clear();
    for (size_t i = start; i + 2 < raw.size(); i += step) {
        if (convertScaleValues) {
            values.push_back(ConvertScale(raw[i], raw[i + 1], raw[i + 2]));
        } else {
            values.push_back(ConvertPoint(raw[i], raw[i + 1], raw[i + 2]));
        }
    }
    return !values.empty();
}

bool GLTFImport::ReadAnimationQuatValues(
    int accessorIndex,
    const std::string& interpolation,
    std::vector<Quat>& values) const {
    std::vector<double> raw;
    if (!ReadAccessorRaw(model_, accessorIndex, TINYGLTF_TYPE_VEC4, raw)) {
        return false;
    }

    size_t start = 0;
    size_t step = 4;
    if (interpolation == "CUBICSPLINE") {
        start = 4;
        step = 12;
    }

    values.clear();
    for (size_t i = start; i + 3 < raw.size(); i += step) {
        values.push_back(ConvertQuat(raw[i], raw[i + 1], raw[i + 2], raw[i + 3]));
    }
    return !values.empty();
}

bool GLTFImport::ReadAnimationWeightValues(
    int accessorIndex,
    const std::string& interpolation,
    size_t targetCount,
    std::vector<std::vector<float>>& values) const {
    if (targetCount == 0) {
        return false;
    }

    std::vector<double> raw;
    if (!ReadAccessorRaw(model_, accessorIndex, TINYGLTF_TYPE_SCALAR, raw)) {
        return false;
    }

    size_t start = 0;
    size_t step = targetCount;
    if (interpolation == "CUBICSPLINE") {
        start = targetCount;
        step = targetCount * 3;
    }

    values.clear();
    for (size_t i = start; i + targetCount - 1 < raw.size(); i += step) {
        std::vector<float> frame;
        frame.reserve(targetCount);
        for (size_t targetIndex = 0; targetIndex < targetCount; ++targetIndex) {
            frame.push_back(static_cast<float>(raw[i + targetIndex] * 100.0));
        }
        values.push_back(std::move(frame));
    }
    return !values.empty();
}

size_t GLTFImport::GetMorphTargetCountForNode(int nodeIndex) const {
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model_.nodes.size())) {
        return 0;
    }

    const tinygltf::Node& node = model_.nodes[nodeIndex];
    if (node.mesh < 0 || node.mesh >= static_cast<int>(model_.meshes.size())) {
        return 0;
    }

    const tinygltf::Mesh& mesh = model_.meshes[node.mesh];
    if (!mesh.weights.empty()) {
        return mesh.weights.size();
    }
    if (!mesh.primitives.empty()) {
        return mesh.primitives[0].targets.size();
    }
    return 0;
}

std::vector<float> GLTFImport::GetDefaultMorphWeightsForNode(int nodeIndex) const {
    const size_t targetCount = GetMorphTargetCountForNode(nodeIndex);
    std::vector<float> weights(targetCount, 0.0f);
    if (targetCount == 0 || nodeIndex < 0 || nodeIndex >= static_cast<int>(model_.nodes.size())) {
        return weights;
    }

    const tinygltf::Node& node = model_.nodes[nodeIndex];
    if (!node.weights.empty()) {
        for (size_t i = 0; i < std::min(weights.size(), node.weights.size()); ++i) {
            weights[i] = static_cast<float>(node.weights[i] * 100.0);
        }
        return weights;
    }

    if (node.mesh >= 0 && node.mesh < static_cast<int>(model_.meshes.size())) {
        const tinygltf::Mesh& mesh = model_.meshes[node.mesh];
        for (size_t i = 0; i < std::min(weights.size(), mesh.weights.size()); ++i) {
            weights[i] = static_cast<float>(mesh.weights[i] * 100.0);
        }
    }
    return weights;
}

void GLTFImport::CollectAnimations() {
    nodeAnimations_.assign(model_.nodes.size(), NodeAnimation{});
    animationEnd_ = 0;
    if (model_.animations.empty()) {
        return;
    }

    const tinygltf::Animation& animation = model_.animations[0];

    for (const tinygltf::AnimationChannel& channel : animation.channels) {
        if (channel.target_node < 0 ||
            channel.target_node >= static_cast<int>(nodeAnimations_.size()) ||
            channel.sampler < 0 ||
            channel.sampler >= static_cast<int>(animation.samplers.size())) {
            continue;
        }

        const tinygltf::AnimationSampler& sampler = animation.samplers[channel.sampler];
        std::vector<TimeValue> times;
        if (!ReadAnimationSamplerTimes(sampler.input, times)) {
            continue;
        }
        if (!times.empty()) {
            animationEnd_ = std::max(animationEnd_, times.back());
        }

        NodeAnimation& dst = nodeAnimations_[channel.target_node];
        if (channel.target_path == "translation") {
            std::vector<Point3> values;
            if (ReadAnimationVec3Values(sampler.output, sampler.interpolation, values, false) &&
                values.size() == times.size()) {
                dst.translation.present = true;
                dst.translation.times = std::move(times);
                dst.translation.values = std::move(values);
            }
        } else if (channel.target_path == "scale") {
            std::vector<Point3> values;
            if (ReadAnimationVec3Values(sampler.output, sampler.interpolation, values, true) &&
                values.size() == times.size()) {
                dst.scale.present = true;
                dst.scale.times = std::move(times);
                dst.scale.values = std::move(values);
            }
        } else if (channel.target_path == "rotation") {
            std::vector<Quat> values;
            if (ReadAnimationQuatValues(sampler.output, sampler.interpolation, values) &&
                values.size() == times.size()) {
                dst.rotation.present = true;
                dst.rotation.times = std::move(times);
                dst.rotation.values = std::move(values);
            }
        } else if (channel.target_path == "weights") {
            const size_t targetCount = GetMorphTargetCountForNode(channel.target_node);
            std::vector<std::vector<float>> values;
            if (ReadAnimationWeightValues(sampler.output, sampler.interpolation, targetCount, values) &&
                values.size() == times.size()) {
                dst.weights.present = true;
                dst.weights.times = std::move(times);
                dst.weights.values = std::move(values);
            }
        }
    }
}

TSTR GLTFImport::MakeNodeName(const tinygltf::Node& node, int nodeIndex, const TCHAR* suffix) const {
    TSTR name;
    if (!node.name.empty()) {
        name = ToTSTR(node.name);
    } else {
        name.printf(_T("Node_%d"), nodeIndex);
    }
    if (suffix != nullptr && suffix[0] != 0) {
        name += suffix;
    }
    return name;
}

INode* GLTFImport::CreateDummyNode(const TSTR& name, INode* parent) {
    DummyObject* dummy = static_cast<DummyObject*>(CreateInstance(HELPER_CLASS_ID, Class_ID(DUMMY_CLASS_ID, 0)));
    if (dummy == nullptr) {
        return nullptr;
    }

    INode* node = ip_->CreateObjectNode(dummy);
    if (node == nullptr) {
        return nullptr;
    }

    node->SetName(name);
    AttachNodeToParent(node, parent);
    return node;
}

INode* GLTFImport::CreateMeshNode(
    const tinygltf::Primitive& primitive,
    const TSTR& name,
    INode* parent,
    const std::vector<Point3>* overridePositions,
    bool hidden) {
    std::vector<Point3> generatedPositions;
    const std::vector<Point3>* positions = overridePositions;
    if (positions == nullptr) {
        if (!ReadPositions(primitive, generatedPositions) || generatedPositions.empty()) {
            return nullptr;
        }
        positions = &generatedPositions;
    }
    if (positions->empty()) {
        return nullptr;
    }

    std::vector<int> rawIndices = ReadIndices(model_, primitive, positions->size());
    std::vector<int> triangles = ConvertPrimitiveToTriangles(primitive, rawIndices);
    if (triangles.empty()) {
        return nullptr;
    }

    TriObject* triObject = CreateNewTriObject();
    Mesh& mesh = triObject->mesh;

    mesh.setNumVerts(static_cast<int>(positions->size()));
    mesh.setNumFaces(static_cast<int>(triangles.size() / 3));

    for (size_t i = 0; i < positions->size(); ++i) {
        mesh.setVert(static_cast<int>(i), (*positions)[i]);
    }

    for (size_t faceIndex = 0; faceIndex < triangles.size() / 3; ++faceIndex) {
        Face& face = mesh.faces[faceIndex];
        face.setVerts(
            triangles[(faceIndex * 3) + 0],
            triangles[(faceIndex * 3) + 1],
            triangles[(faceIndex * 3) + 2]);
        face.setEdgeVisFlags(1, 1, 1);
        face.setSmGroup(1);
        face.setMatID(0);
    }

    std::vector<UVVert> uvs;
    if (ReadUVs(primitive, uvs) && uvs.size() == positions->size()) {
        mesh.setNumTVerts(static_cast<int>(uvs.size()));
        mesh.setNumTVFaces(static_cast<int>(triangles.size() / 3));
        for (size_t i = 0; i < uvs.size(); ++i) {
            mesh.setTVert(static_cast<int>(i), uvs[i]);
        }
        for (size_t faceIndex = 0; faceIndex < triangles.size() / 3; ++faceIndex) {
            TVFace& tvFace = mesh.tvFace[faceIndex];
            tvFace.setTVerts(
                triangles[(faceIndex * 3) + 0],
                triangles[(faceIndex * 3) + 1],
                triangles[(faceIndex * 3) + 2]);
        }
    }

    mesh.InvalidateGeomCache();
    mesh.InvalidateTopologyCache();
    mesh.buildNormals();

    INode* node = ip_->CreateObjectNode(triObject);
    if (node == nullptr) {
        return nullptr;
    }

    node->SetName(name);
    if (hidden) {
        node->Hide(TRUE);
        node->SetRenderable(FALSE);
    }
    if (primitive.material >= 0 && primitive.material < static_cast<int>(materials_.size()) &&
        materials_[primitive.material] != nullptr) {
        node->SetMtl(materials_[primitive.material]);
    }

    AttachNodeToParent(node, parent);

    const LocalTransform identity;
    const NodeAnimation noAnimation;
    ApplyNodeTransform(node, identity, noAnimation);
    return node;
}

void GLTFImport::SetupMorphTargetsForNode(
    int nodeIndex,
    const tinygltf::Primitive& primitive,
    INode* baseNode,
    const TSTR& name,
    NodeRecord& record) {
    if (baseNode == nullptr || primitive.targets.empty()) {
        return;
    }

    Modifier* morpher = static_cast<Modifier*>(CreateInstance(OSM_CLASS_ID, MR3_CLASS_ID));
    if (morpher == nullptr) {
        return;
    }

    IDerivedObject* derived = EnsureDerivedObject(baseNode);
    if (derived == nullptr) {
        return;
    }

    derived->AddModifier(morpher);

    MaxMorphModifier morphModifier(morpher);
    if (!morphModifier.IsValid()) {
        return;
    }

    const std::vector<float> defaultWeights = GetDefaultMorphWeightsForNode(nodeIndex);
    morphModifier.SetNumChannels(static_cast<int>(primitive.targets.size()));
    morphModifier.RebuildCache();
    IMorpher* morphInterface = static_cast<IMorpher*>(morpher->GetInterface(I_MORPHER_INTERFACE_ID));
    if (morphInterface == nullptr) {
        return;
    }

    std::vector<Point3> basePositions;
    if (!ReadPositions(primitive, basePositions) || basePositions.empty()) {
        return;
    }

    MorphBinding binding;
    binding.baseNode = baseNode;
    binding.morpher = morpher;
    binding.defaultWeights = defaultWeights;

    for (size_t morphTargetIndex = 0; morphTargetIndex < primitive.targets.size(); ++morphTargetIndex) {
        std::vector<Point3> targetPositions;
        if (!ReadMorphTargetPositions(primitive, static_cast<int>(morphTargetIndex), targetPositions)) {
            continue;
        }

        TSTR targetName = name;
        targetName += _T("_morph_");
        targetName += TSTR::FromUTF8(std::to_string(morphTargetIndex).c_str());

        if (targetPositions.size() != basePositions.size()) {
            continue;
        }

        MaxMorphChannel channel = morphModifier.GetMorphChannel(static_cast<int>(morphTargetIndex));
        IMorpherChannel* channelData = morphInterface->GetChannel(static_cast<int>(morphTargetIndex), true);
        if (channelData == nullptr) {
            continue;
        }

        channel.Reset(true, true, static_cast<int>(targetPositions.size()));
        channel.SetName(targetName.data(), false);
        channel.SetName(targetName.data(), true);
        channel.SetActive(true);
        channelData->SetConnection(nullptr);

        for (size_t pointIndex = 0; pointIndex < targetPositions.size(); ++pointIndex) {
            Point3 point = targetPositions[pointIndex];
            Point3 delta = (targetPositions[pointIndex] - basePositions[pointIndex]) / 100.0f;
            channelData->SetPoint(static_cast<int>(pointIndex), point);
            channelData->SetDelta(static_cast<int>(pointIndex), delta);
            channelData->SetWeight(static_cast<int>(pointIndex), 1.0);
        }

        if (morphTargetIndex < defaultWeights.size()) {
            channel.SetMorphWeight(0, defaultWeights[morphTargetIndex]);
        }

        binding.channelCount = std::max(binding.channelCount, morphTargetIndex + 1);
    }

    if (binding.channelCount > 0) {
        morphModifier.RefreshChannelsUI();
        record.morphBindings.push_back(std::move(binding));
    }
}

void GLTFImport::ApplyMorphAnimation(NodeRecord& record, const WeightKeyTrack& animation) {
    const TimeValue originalTime = ip_ != nullptr ? ip_->GetTime() : 0;
    const BOOL originalAnimateState = Animating() ? TRUE : FALSE;

    SuspendAnimate();
    AnimateOn();
    if (ip_ != nullptr) {
        ip_->SetAnimateButtonState(TRUE);
    }

    for (MorphBinding& binding : record.morphBindings) {
        MaxMorphModifier morphModifier(binding.morpher);
        if (!morphModifier.IsValid()) {
            continue;
        }

        const size_t channelCount = std::min(binding.channelCount, static_cast<size_t>(morphModifier.NumMorphChannels()));
        if (animation.present) {
            for (size_t keyIndex = 0; keyIndex < animation.times.size() && keyIndex < animation.values.size(); ++keyIndex) {
                if (ip_ != nullptr) {
                    ip_->SetTime(animation.times[keyIndex], FALSE);
                }
                const std::vector<float>& frameWeights = animation.values[keyIndex];
                for (size_t channelIndex = 0; channelIndex < channelCount && channelIndex < frameWeights.size(); ++channelIndex) {
                    morphModifier.GetMorphChannel(static_cast<int>(channelIndex))
                        .SetMorphWeight(animation.times[keyIndex], frameWeights[channelIndex]);
                }
            }
        } else {
            for (size_t channelIndex = 0; channelIndex < channelCount && channelIndex < binding.defaultWeights.size(); ++channelIndex) {
                morphModifier.GetMorphChannel(static_cast<int>(channelIndex))
                    .SetMorphWeight(0, binding.defaultWeights[channelIndex]);
            }
        }

        morphModifier.RefreshChannelsUI();
    }

    ResumeAnimate();
    if (ip_ != nullptr) {
        ip_->SetAnimateButtonState(originalAnimateState);
        ip_->SetTime(originalTime, FALSE);
    }
}

INode* GLTFImport::CreateAnchorNode(const tinygltf::Node& node, int nodeIndex, INode* parent) {
    if (node.mesh >= 0 && node.mesh < static_cast<int>(model_.meshes.size())) {
        const tinygltf::Mesh& mesh = model_.meshes[node.mesh];
        if (mesh.primitives.size() == 1) {
            INode* singleMeshNode = CreateMeshNode(mesh.primitives[0], MakeNodeName(node, nodeIndex), parent);
            if (singleMeshNode != nullptr) {
                return singleMeshNode;
            }
        }
    }

    return CreateDummyNode(MakeNodeName(node, nodeIndex), parent);
}

void GLTFImport::ImportNodeRecursive(int nodeIndex, INode* parent) {
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(model_.nodes.size())) {
        return;
    }

    const tinygltf::Node& node = model_.nodes[nodeIndex];
    INode* anchor = CreateAnchorNode(node, nodeIndex, parent);
    if (anchor == nullptr) {
        return;
    }

    NodeRecord& record = nodeRecords_[nodeIndex];
    record.anchor = anchor;
    ApplyNodeTransform(anchor, GetNodeTransform(node), nodeAnimations_[nodeIndex]);

    if (node.mesh >= 0 && node.mesh < static_cast<int>(model_.meshes.size())) {
        const tinygltf::Mesh& mesh = model_.meshes[node.mesh];
        if (mesh.primitives.size() == 1) {
            SetupMorphTargetsForNode(nodeIndex, mesh.primitives[0], anchor, MakeNodeName(node, nodeIndex), record);
        } else if (mesh.primitives.size() > 1) {
            for (size_t primitiveIndex = 0; primitiveIndex < mesh.primitives.size(); ++primitiveIndex) {
                TSTR suffix;
                suffix.printf(_T("_%d"), static_cast<int>(primitiveIndex));
                TSTR primitiveName = MakeNodeName(node, nodeIndex, suffix);
                INode* primitiveNode = CreateMeshNode(mesh.primitives[primitiveIndex], primitiveName, anchor);
                SetupMorphTargetsForNode(nodeIndex, mesh.primitives[primitiveIndex], primitiveNode, primitiveName, record);
            }
        }
    }

    for (int childIndex : node.children) {
        ImportNodeRecursive(childIndex, anchor);
    }
}

void GLTFImport::ImportScene() {
    nodeRecords_.assign(model_.nodes.size(), NodeRecord{});

    if (!model_.scenes.empty()) {
        const int sceneIndex = (model_.defaultScene >= 0 && model_.defaultScene < static_cast<int>(model_.scenes.size()))
            ? model_.defaultScene
            : 0;
        for (int nodeIndex : model_.scenes[sceneIndex].nodes) {
            ImportNodeRecursive(nodeIndex, nullptr);
        }
    } else {
        std::vector<bool> hasParent(model_.nodes.size(), false);
        for (const tinygltf::Node& node : model_.nodes) {
            for (int childIndex : node.children) {
                if (childIndex >= 0 && childIndex < static_cast<int>(hasParent.size())) {
                    hasParent[childIndex] = true;
                }
            }
        }

        for (size_t i = 0; i < model_.nodes.size(); ++i) {
            if (!hasParent[i]) {
                ImportNodeRecursive(static_cast<int>(i), nullptr);
            }
        }
    }

    for (size_t i = 0; i < nodeRecords_.size(); ++i) {
        ApplyMorphAnimation(nodeRecords_[i], nodeAnimations_[i].weights);
    }
}

int GLTFImport::DoImport(const TCHAR* filename, ImpInterface* i, Interface* gi, BOOL) {
    imp_ = i;
    ip_ = gi;
    model_ = tinygltf::Model{};

    sourceFile_ = fs::path(filename);
    sourceDir_ = sourceFile_.parent_path();
    textureDir_ = sourceDir_ / fs::path(sourceFile_.stem().string() + "_gltf_textures");

    if (!LoadModel(sourceFile_)) {
        MaxSDK::MaxMessageBox(
            gi->GetMAXHWnd(),
            _T("Failed to load glTF/GLB file."),
            GetString(IDS_IMPORTER_NAME),
            MB_OK | MB_ICONERROR);
        return 0;
    }

    BuildMaterials();
    CollectAnimations();
    if (animationEnd_ > 0) {
        gi->SetAnimRange(Interval(0, animationEnd_));
        gi->SetTime(0, FALSE);
    }
    ImportScene();

    return 1;
}

class GLTFImportClassDesc : public ClassDesc {
public:
    int IsPublic() override { return 1; }
    void* Create(BOOL) override { return new GLTFImport(); }
    const TCHAR* ClassName() override { return GetString(IDS_IMPORTER_NAME); }
    const TCHAR* NonLocalizedClassName() override { return _T("glTF Scene Import"); }
    SClass_ID SuperClassID() override { return SCENE_IMPORT_CLASS_ID; }
    Class_ID ClassID() override { return kImporterClassId; }
    const TCHAR* Category() override { return _T("Scene Import"); }
};

GLTFImportClassDesc gGLTFImportClassDesc;

} // namespace

BOOL WINAPI DllMain(HINSTANCE hinstDLL, ULONG fdwReason, LPVOID) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        MaxSDK::Util::UseLanguagePackLocale();
        hInstance = hinstDLL;
        DisableThreadLibraryCalls(hInstance);
    }
    return TRUE;
}

__declspec(dllexport) const TCHAR* LibDescription() {
    return GetString(IDS_LIBDESCRIPTION);
}

__declspec(dllexport) int LibNumberClasses() {
    return 1;
}

__declspec(dllexport) ClassDesc* LibClassDesc(int i) {
    return (i == 0) ? &gGLTFImportClassDesc : nullptr;
}

__declspec(dllexport) ULONG LibVersion() {
    return VERSION_3DSMAX;
}

__declspec(dllexport) ULONG CanAutoDefer() {
    return 1;
}
