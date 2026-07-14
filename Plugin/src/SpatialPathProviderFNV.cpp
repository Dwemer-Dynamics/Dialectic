// SpatialPathProviderFNV.cpp - Optional FNV path-distance bridge for spatial awareness

#include "SpatialPathProviderFNV.h"

#include "GameThreadDispatcher.h"
#include "Config.h"
#include "Logger.h"
#include "RuntimeGeneration.h"
#include "RuntimeSnapshot.h"
#include "TaskManager.h"
#include "XNVSEAdapter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace SpatialPathProviderFNV {
namespace {

struct CachedPathResult {
    PathResult result;
    std::chrono::steady_clock::time_point timestamp;
};

static std::mutex g_pathCacheMutex;
static std::unordered_map<std::uint64_t, CachedPathResult> g_pathCache;
struct CachedLosResult {
    bool hasLineOfSight{false};
    std::chrono::steady_clock::time_point timestamp;
};
static std::unordered_map<std::uint64_t, CachedLosResult> g_losCache;
static std::unordered_set<std::uint64_t> g_pendingLosQueries;
static constexpr auto kPathCacheTtl = std::chrono::seconds(3);
static constexpr auto kLosCacheTtl = std::chrono::seconds(2);
static constexpr std::size_t kMaxPathVisits = 50000;

struct NavEdge {
    std::uint32_t target{0};
    float distance{0.0f};
};

struct NavNode {
    ActorPositionResolverFNV::Vector3 centroid;
    std::vector<NavEdge> edges;
    std::uint32_t doorFormId{0};
};

struct NativeGraph {
    std::uint32_t cellFormId{0};
    std::uint64_t generation{0};
    bool interior{false};
    std::vector<NavNode> nodes;
    std::size_t edgeCount{0};
};

struct IndexedEdgeKey {
    std::uint32_t mesh{0};
    std::uint32_t low{0};
    std::uint32_t high{0};
    bool operator==(const IndexedEdgeKey& other) const {
        return mesh == other.mesh && low == other.low && high == other.high;
    }
};

struct IndexedEdgeHash {
    std::size_t operator()(const IndexedEdgeKey& key) const {
        std::size_t value = std::hash<std::uint32_t>{}(key.mesh);
        value ^= std::hash<std::uint32_t>{}(key.low + 0x9e3779b9U + (value << 6U) + (value >> 2U));
        value ^= std::hash<std::uint32_t>{}(key.high + 0x9e3779b9U + (value << 6U) + (value >> 2U));
        return value;
    }
};

struct QuantizedPoint {
    int x{0};
    int y{0};
    int z{0};
    bool operator==(const QuantizedPoint& other) const {
        return x == other.x && y == other.y && z == other.z;
    }
    bool operator<(const QuantizedPoint& other) const {
        if (x != other.x) return x < other.x;
        if (y != other.y) return y < other.y;
        return z < other.z;
    }
};

struct GeometricEdgeKey {
    QuantizedPoint low;
    QuantizedPoint high;
    bool operator==(const GeometricEdgeKey& other) const {
        return low == other.low && high == other.high;
    }
};

struct GeometricEdgeHash {
    std::size_t operator()(const GeometricEdgeKey& key) const {
        std::size_t value = std::hash<int>{}(key.low.x);
        const int fields[] = {key.low.y, key.low.z, key.high.x, key.high.y, key.high.z};
        for (int field : fields) {
            value ^= std::hash<int>{}(field) + 0x9e3779b9U + (value << 6U) + (value >> 2U);
        }
        return value;
    }
};

static std::mutex g_graphMutex;
static std::shared_ptr<const NativeGraph> g_nativeGraph;
static std::uint32_t g_buildingCellFormId{0};
static std::uint64_t g_buildingGeneration{0};

std::uint64_t PathKey(std::uint32_t sourceFormId, std::uint32_t listenerFormId) {
    const std::uint32_t low = std::min(sourceFormId, listenerFormId);
    const std::uint32_t high = std::max(sourceFormId, listenerFormId);
    return (static_cast<std::uint64_t>(low) << 32U) | static_cast<std::uint64_t>(high);
}

std::uint64_t DirectionalKey(std::uint32_t sourceFormId, std::uint32_t listenerFormId) {
    return (static_cast<std::uint64_t>(sourceFormId) << 32U) |
        static_cast<std::uint64_t>(listenerFormId);
}

float Distance(
    const ActorPositionResolverFNV::Vector3& a,
    const ActorPositionResolverFNV::Vector3& b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
}

bool IsFinite(const RuntimeSnapshot::NavVertex& vertex) {
    return std::isfinite(vertex.x) && std::isfinite(vertex.y) && std::isfinite(vertex.z);
}

QuantizedPoint Quantize(const RuntimeSnapshot::NavVertex& vertex) {
    constexpr float kPortalTolerance = 4.0f;
    return {
        static_cast<int>(std::lround(vertex.x / kPortalTolerance)),
        static_cast<int>(std::lround(vertex.y / kPortalTolerance)),
        static_cast<int>(std::lround(vertex.z / kPortalTolerance))
    };
}

void AddGraphEdge(NativeGraph& graph, std::uint32_t first, std::uint32_t second) {
    if (first == second || first >= graph.nodes.size() || second >= graph.nodes.size()) return;
    auto addOne = [&graph](std::uint32_t from, std::uint32_t to) {
        auto& edges = graph.nodes[from].edges;
        if (std::any_of(edges.begin(), edges.end(), [to](const NavEdge& edge) {
                return edge.target == to;
            })) return false;
        edges.push_back({to, Distance(graph.nodes[from].centroid, graph.nodes[to].centroid)});
        return true;
    };
    if (addOne(first, second)) {
        addOne(second, first);
        ++graph.edgeCount;
    }
}

std::shared_ptr<NativeGraph> BuildGraph(const RuntimeSnapshot::NavSceneState& scene,
                                        const TaskManager::CancellationToken& token,
                                        std::size_t& stitchedEdges,
                                        std::size_t& skippedTriangles) {
    auto graph = std::make_shared<NativeGraph>();
    graph->cellFormId = scene.cellFormId;
    graph->generation = scene.generation;
    graph->interior = scene.interior;

    std::vector<std::vector<std::uint32_t>> nodeByTriangle(scene.meshes.size());
    constexpr std::uint32_t kInvalidNode = std::numeric_limits<std::uint32_t>::max();
    for (std::size_t meshIndex = 0; meshIndex < scene.meshes.size(); ++meshIndex) {
        if (token.IsCancellationRequested()) return {};
        const auto& mesh = scene.meshes[meshIndex];
        auto& mapping = nodeByTriangle[meshIndex];
        mapping.assign(mesh.triangles.size(), kInvalidNode);
        for (std::size_t triangleIndex = 0; triangleIndex < mesh.triangles.size(); ++triangleIndex) {
            const auto& triangle = mesh.triangles[triangleIndex];
            bool valid = true;
            ActorPositionResolverFNV::Vector3 centroid;
            for (int corner = 0; corner < 3; ++corner) {
                const int vertexIndex = triangle.vertices[corner];
                if (vertexIndex < 0 || static_cast<std::size_t>(vertexIndex) >= mesh.vertices.size() ||
                    !IsFinite(mesh.vertices[vertexIndex])) {
                    valid = false;
                    break;
                }
                centroid.x += mesh.vertices[vertexIndex].x;
                centroid.y += mesh.vertices[vertexIndex].y;
                centroid.z += mesh.vertices[vertexIndex].z;
            }
            if (!valid) {
                ++skippedTriangles;
                continue;
            }
            centroid.x /= 3.0f;
            centroid.y /= 3.0f;
            centroid.z /= 3.0f;
            mapping[triangleIndex] = static_cast<std::uint32_t>(graph->nodes.size());
            graph->nodes.push_back({centroid, {}, triangle.doorFormId});
        }
    }

    std::unordered_map<IndexedEdgeKey, std::uint32_t, IndexedEdgeHash> indexedEdges;
    std::unordered_map<GeometricEdgeKey, std::pair<std::uint32_t, std::uint32_t>, GeometricEdgeHash> geometricEdges;
    for (std::size_t meshIndex = 0; meshIndex < scene.meshes.size(); ++meshIndex) {
        if (token.IsCancellationRequested()) return {};
        const auto& mesh = scene.meshes[meshIndex];
        for (std::size_t triangleIndex = 0; triangleIndex < mesh.triangles.size(); ++triangleIndex) {
            const std::uint32_t node = nodeByTriangle[meshIndex][triangleIndex];
            if (node == kInvalidNode) continue;
            const auto& triangle = mesh.triangles[triangleIndex];
            for (int side = 0; side < 3; ++side) {
                const std::uint32_t firstVertex = static_cast<std::uint32_t>(triangle.vertices[side]);
                const std::uint32_t secondVertex = static_cast<std::uint32_t>(triangle.vertices[(side + 1) % 3]);
                IndexedEdgeKey indexedKey{
                    static_cast<std::uint32_t>(meshIndex),
                    std::min(firstVertex, secondVertex),
                    std::max(firstVertex, secondVertex)
                };
                const auto indexed = indexedEdges.emplace(indexedKey, node);
                if (!indexed.second) AddGraphEdge(*graph, indexed.first->second, node);

                QuantizedPoint firstPoint = Quantize(mesh.vertices[firstVertex]);
                QuantizedPoint secondPoint = Quantize(mesh.vertices[secondVertex]);
                if (secondPoint < firstPoint) std::swap(firstPoint, secondPoint);
                GeometricEdgeKey geometricKey{firstPoint, secondPoint};
                const auto geometric = geometricEdges.emplace(
                    geometricKey, std::make_pair(node, static_cast<std::uint32_t>(meshIndex)));
                if (!geometric.second && geometric.first->second.second != meshIndex) {
                    const std::size_t before = graph->edgeCount;
                    AddGraphEdge(*graph, geometric.first->second.first, node);
                    if (graph->edgeCount > before) ++stitchedEdges;
                }
            }
        }
    }
    return graph;
}

bool FindNearestNode(const NativeGraph& graph,
                     const ActorPositionResolverFNV::Vector3& position,
                     std::uint32_t& node,
                     float& distance) {
    float bestSquared = std::numeric_limits<float>::max();
    std::uint32_t best = 0;
    for (std::uint32_t index = 0; index < graph.nodes.size(); ++index) {
        const float dx = graph.nodes[index].centroid.x - position.x;
        const float dy = graph.nodes[index].centroid.y - position.y;
        const float dz = graph.nodes[index].centroid.z - position.z;
        const float squared = dx * dx + dy * dy + dz * dz;
        if (squared < bestSquared) {
            bestSquared = squared;
            best = index;
        }
    }
    const float snapLimit = std::max(50.0f, Config::spatialNavmeshSnapDistance);
    if (graph.nodes.empty() || bestSquared > snapLimit * snapLimit) return false;
    node = best;
    distance = std::sqrt(bestSquared);
    return true;
}

PathResult EvaluateNativeGraph(const NativeGraph& graph,
                               const ActorPositionResolverFNV::PositionResult& source,
                               const ActorPositionResolverFNV::PositionResult& listener,
                               float airDistance) {
    PathResult result;
    result.airDistance = airDistance;
    if (!source.cellResolved || !listener.cellResolved || source.cellFormId != listener.cellFormId ||
        source.cellFormId != graph.cellFormId || graph.nodes.empty()) return result;

    std::uint32_t sourceNode = 0;
    std::uint32_t listenerNode = 0;
    float sourceSnap = 0.0f;
    float listenerSnap = 0.0f;
    if (!FindNearestNode(graph, source.position, sourceNode, sourceSnap) ||
        !FindNearestNode(graph, listener.position, listenerNode, listenerSnap)) return result;

    result.nativeGraphUsed = true;
    std::vector<float> distances(graph.nodes.size(), std::numeric_limits<float>::infinity());
    std::vector<std::uint32_t> previous(graph.nodes.size(), std::numeric_limits<std::uint32_t>::max());
    using QueueEntry = std::pair<float, std::uint32_t>;
    std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<QueueEntry>> queue;
    distances[sourceNode] = 0.0f;
    queue.push({0.0f, sourceNode});
    std::size_t visited = 0;
    while (!queue.empty() && visited < kMaxPathVisits) {
        const auto current = queue.top();
        queue.pop();
        if (current.first != distances[current.second]) continue;
        ++visited;
        if (current.second == listenerNode) break;
        for (const auto& edge : graph.nodes[current.second].edges) {
            const float candidate = current.first + edge.distance;
            if (candidate < distances[edge.target]) {
                distances[edge.target] = candidate;
                previous[edge.target] = current.second;
                queue.push({candidate, edge.target});
            }
        }
    }
    if (!std::isfinite(distances[listenerNode])) {
        result.status = graph.interior ? PathStatus::NoPath : PathStatus::Unavailable;
        return result;
    }

    result.status = PathStatus::Success;
    result.pathDistance = sourceSnap + distances[listenerNode] + listenerSnap;
    std::unordered_set<std::uint32_t> doors;
    for (std::uint32_t node = listenerNode; node != std::numeric_limits<std::uint32_t>::max();) {
        const std::uint32_t doorFormId = graph.nodes[node].doorFormId;
        if (doorFormId != 0) doors.insert(doorFormId);
        if (node == sourceNode) break;
        node = previous[node];
    }
    for (std::uint32_t doorFormId : doors) {
        RuntimeSnapshot::ReferenceState door;
        if (!RuntimeSnapshot::TryGetReference(doorFormId, door) || !door.openStateKnown) continue;
        if (door.openState == 3 || door.openState == 4) ++result.closedDoorCount;
        else if (door.openState == 1 || door.openState == 2) ++result.openDoorCount;
    }
    return result;
}

bool TryGetCachedPath(std::uint32_t sourceFormId, std::uint32_t listenerFormId, PathResult& result) {
    if (sourceFormId == 0 || listenerFormId == 0) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_pathCacheMutex);
    const auto it = g_pathCache.find(PathKey(sourceFormId, listenerFormId));
    if (it == g_pathCache.end() || now - it->second.timestamp > kPathCacheTtl) {
        return false;
    }

    result = it->second.result;
    return true;
}

} // namespace

void PublishNativeScene(const RuntimeSnapshot::NavSceneState& scene) {
    if (!scene.valid || !scene.complete || scene.cellFormId == 0 || scene.generation == 0) return;
    {
        std::lock_guard<std::mutex> lock(g_graphMutex);
        if ((g_nativeGraph && g_nativeGraph->cellFormId == scene.cellFormId &&
             g_nativeGraph->generation == scene.generation) ||
            (g_buildingCellFormId == scene.cellFormId && g_buildingGeneration == scene.generation)) return;
        g_buildingCellFormId = scene.cellFormId;
        g_buildingGeneration = scene.generation;
    }

    char key[32] = {};
    sprintf_s(key, "cell_%08X", scene.cellFormId);
    const std::uint64_t taskId = TaskManager::Enqueue("spatial_navgraph", key, scene.generation,
        false, std::chrono::seconds(10),
        [scene](const TaskManager::CancellationToken& token) {
            const auto started = std::chrono::steady_clock::now();
            std::size_t stitched = 0;
            std::size_t skipped = 0;
            auto graph = BuildGraph(scene, token, stitched, skipped);
            if (!graph || token.IsCancellationRequested() || !RuntimeGeneration::IsCurrent(scene.generation)) return;
            const auto game = RuntimeSnapshot::GetGameState();
            if (game.cellFormId != scene.cellFormId) return;
            {
                std::lock_guard<std::mutex> lock(g_graphMutex);
                g_nativeGraph = graph;
                g_buildingCellFormId = 0;
                g_buildingGeneration = 0;
            }
            {
                std::lock_guard<std::mutex> lock(g_pathCacheMutex);
                g_pathCache.clear();
            }
            Logger::LogInfo(
                "[NATIVE_NAV] graph ready cell=0x%08X nodes=%zu edges=%zu stitched=%zu skipped=%zu build_ms=%lld",
                scene.cellFormId, graph->nodes.size(), graph->edgeCount, stitched, skipped,
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started).count()));
        },
        [cellFormId = scene.cellFormId, generation = scene.generation](bool success, const char* reason) {
            if (success) return;
            std::lock_guard<std::mutex> lock(g_graphMutex);
            if (g_buildingCellFormId == cellFormId && g_buildingGeneration == generation) {
                g_buildingCellFormId = 0;
                g_buildingGeneration = 0;
            }
            Logger::LogWarning("[NATIVE_NAV] graph build dropped cell=0x%08X reason=%s",
                cellFormId, reason ? reason : "unknown");
        });
    if (taskId == 0) {
        std::lock_guard<std::mutex> lock(g_graphMutex);
        g_buildingCellFormId = 0;
        g_buildingGeneration = 0;
        Logger::LogWarning("[NATIVE_NAV] graph build enqueue rejected cell=0x%08X", scene.cellFormId);
    }
}

NativeGraphStatus GetNativeGraphStatus() {
    std::lock_guard<std::mutex> lock(g_graphMutex);
    NativeGraphStatus status;
    status.building = g_buildingCellFormId != 0;
    if (g_nativeGraph) {
        status.cellFormId = g_nativeGraph->cellFormId;
        status.generation = g_nativeGraph->generation;
        status.nodes = g_nativeGraph->nodes.size();
        status.edges = g_nativeGraph->edgeCount;
        status.ready = true;
    } else if (status.building) {
        status.cellFormId = g_buildingCellFormId;
        status.generation = g_buildingGeneration;
    }
    return status;
}

PathResult EvaluatePath(
    const ActorPositionResolverFNV::PositionResult& source,
    const ActorPositionResolverFNV::PositionResult& listener) {
    PathResult result;

    if (!source.resolved || !listener.resolved) {
        return result;
    }

    if (source.formId != 0 && source.formId == listener.formId) {
        result.status = PathStatus::Success;
        result.airDistance = 0.0f;
        result.pathDistance = 0.0f;
        return result;
    }

    result.airDistance = Distance(source.position, listener.position);
    if (!std::isfinite(result.airDistance)) {
        result.airDistance = 0.0f;
        return result;
    }

    std::shared_ptr<const NativeGraph> graph;
    {
        std::lock_guard<std::mutex> lock(g_graphMutex);
        graph = g_nativeGraph;
    }
    if (graph && graph->generation == RuntimeGeneration::Current()) {
        PathResult nativeResult = EvaluateNativeGraph(*graph, source, listener, result.airDistance);
        if (nativeResult.status != PathStatus::Unavailable || nativeResult.nativeGraphUsed) {
            return nativeResult;
        }
    }

    PathResult cached;
    if (TryGetCachedPath(source.formId, listener.formId, cached)) {
        if (cached.airDistance <= 0.0f) {
            cached.airDistance = result.airDistance;
        }
        return cached;
    }

    return result;
}

void RememberScriptPathResult(
    std::uint32_t sourceFormId,
    std::uint32_t listenerFormId,
    PathStatus status,
    float airDistance,
    float pathDistance) {
    if (sourceFormId == 0 || listenerFormId == 0 || sourceFormId == listenerFormId) {
        return;
    }

    PathResult result;
    result.status = status;
    result.airDistance = std::isfinite(airDistance) ? std::max(0.0f, airDistance) : 0.0f;
    result.pathDistance = std::isfinite(pathDistance) ? pathDistance : -1.0f;

    std::lock_guard<std::mutex> lock(g_pathCacheMutex);
    g_pathCache[PathKey(sourceFormId, listenerFormId)] = {
        result,
        std::chrono::steady_clock::now()
    };
}

bool TryGetNativeLineOfSight(std::uint32_t sourceFormId,
                             std::uint32_t listenerFormId,
                             bool& hasLineOfSight) {
    if (sourceFormId == 0 || listenerFormId == 0) {
        return false;
    }
    const std::uint64_t key = DirectionalKey(sourceFormId, listenerFormId);
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_pathCacheMutex);
        const auto cached = g_losCache.find(key);
        if (cached != g_losCache.end() && now - cached->second.timestamp <= kLosCacheTtl) {
            hasLineOfSight = cached->second.hasLineOfSight;
            return true;
        }
        if (!g_pendingLosQueries.insert(key).second) {
            return false;
        }
    }

    char commandKey[48] = {};
    sprintf_s(commandKey, "%08X:%08X", sourceFormId, listenerFormId);
    if (!GameThreadDispatcher::Enqueue("spatial_los", commandKey, RuntimeGeneration::Current(),
        [sourceFormId, listenerFormId, key]() {
            bool visible = false;
            const bool ok = XNVSEAdapter::QueryNativeLineOfSight(
                sourceFormId, listenerFormId, visible);
            std::lock_guard<std::mutex> lock(g_pathCacheMutex);
            g_pendingLosQueries.erase(key);
            if (ok) {
                g_losCache[key] = {visible, std::chrono::steady_clock::now()};
            }
        },
        [key](const char* reason) {
            std::lock_guard<std::mutex> lock(g_pathCacheMutex);
            g_pendingLosQueries.erase(key);
            Logger::LogWarning("[NATIVE_SPATIAL] LOS query dropped reason=%s",
                reason ? reason : "unknown");
        })) {
        std::lock_guard<std::mutex> lock(g_pathCacheMutex);
        g_pendingLosQueries.erase(key);
    }
    return false;
}

void InvalidateCache() {
    {
        std::lock_guard<std::mutex> lock(g_pathCacheMutex);
        g_pathCache.clear();
        g_losCache.clear();
        g_pendingLosQueries.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_graphMutex);
        g_nativeGraph.reset();
        g_buildingCellFormId = 0;
        g_buildingGeneration = 0;
    }
    TaskManager::CancelByType("spatial_navgraph");
}

} // namespace SpatialPathProviderFNV
