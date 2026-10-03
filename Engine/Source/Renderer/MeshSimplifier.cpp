#include "Renderer/MeshSimplifier.h"

#include "Core/Math/Box.h"
#include "Renderer/LodMath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <queue>
#include <unordered_map>

namespace
{
	// 대칭 4x4 이차식 (평면 a x + b y + c z + d = 0의 제곱 거리 합)
	struct FQuadric
	{
		double A[10] = {}; // aa ab ac ad bb bc bd cc cd dd

		void AddPlane(double Na, double Nb, double Nc, double D, double Weight)
		{
			A[0] += Weight * Na * Na; A[1] += Weight * Na * Nb; A[2] += Weight * Na * Nc; A[3] += Weight * Na * D;
			A[4] += Weight * Nb * Nb; A[5] += Weight * Nb * Nc; A[6] += Weight * Nb * D;
			A[7] += Weight * Nc * Nc; A[8] += Weight * Nc * D;
			A[9] += Weight * D * D;
		}

		void operator+=(const FQuadric& Other)
		{
			for (int32 Index = 0; Index < 10; ++Index)
			{
				A[Index] += Other.A[Index];
			}
		}

		double Evaluate(const FVector3& P) const
		{
			const double X = P.X, Y = P.Y, Z = P.Z;
			return A[0] * X * X + 2.0 * A[1] * X * Y + 2.0 * A[2] * X * Z + 2.0 * A[3] * X + A[4] * Y * Y + 2.0 * A[5] * Y * Z + 2.0 * A[6] * Y +
			       A[7] * Z * Z + 2.0 * A[8] * Z + A[9];
		}
	};

	struct FCollapse
	{
		double Cost     = 0.0;
		uint32 From     = 0;
		uint32 To       = 0;
		uint32 FromStamp = 0;
		uint32 ToStamp   = 0;
		bool   operator>(const FCollapse& Other) const { return Cost > Other.Cost; }
	};

	struct FPositionKey
	{
		uint32 Bits[3] = {};
		bool   operator==(const FPositionKey& Other) const { return Bits[0] == Other.Bits[0] && Bits[1] == Other.Bits[1] && Bits[2] == Other.Bits[2]; }
	};

	struct FPositionKeyHash
	{
		size_t operator()(const FPositionKey& Key) const
		{
			uint64 Hash = 1469598103934665603ull;
			for (const uint32 Value : Key.Bits)
			{
				Hash = (Hash ^ Value) * 1099511628211ull;
			}
			return static_cast<size_t>(Hash);
		}
	};

	uint64 EdgeKey(uint32 A, uint32 B)
	{
		return A < B ? (static_cast<uint64>(A) << 32) | B : (static_cast<uint64>(B) << 32) | A;
	}

	// 위치 단위 반-모서리 붕괴 단순화기. Init 후 Run(목표)을 여러 번 불러 LOD를 차례로 만든다 (상태가 이어진다)
	class FSimplifier
	{
	public:
		FSimplifier(const std::vector<FVertex>& InVertices, const std::vector<uint32>& Indices)
			: Vertices(InVertices)
		{
			// 위치 묶기: 경계 크기의 1e-5 이내면 같은 위치 (이음매 양쪽 정점이 삼각 함수 오차로 비트 단위로는 다를 수 있다 —
			// 묶지 않으면 이음매 양쪽이 따로 줄어 틈이 벌어진다). 칸 = 허용 오차, 이웃 27칸에서 찾는다
			FBox Bounds;
			for (const FVertex& Vertex : Vertices)
			{
				Bounds.AddPoint(Vertex.Position);
			}
			const FVector3 Size      = Bounds.IsValid() ? Bounds.GetSize() : FVector3::ZeroVector;
			const float    Tolerance = FMath::Max(FMath::Max(FMath::Max(Size.X, Size.Y), Size.Z) * 1.0e-5f, 1.0e-6f);
			const auto     CellOf    = [&](const FVector3& P, int32 DX, int32 DY, int32 DZ) {
				FPositionKey Key;
				Key.Bits[0] = static_cast<uint32>(static_cast<int32>(FMath::Floor((P.X - Bounds.Min.X) / Tolerance)) + DX);
				Key.Bits[1] = static_cast<uint32>(static_cast<int32>(FMath::Floor((P.Y - Bounds.Min.Y) / Tolerance)) + DY);
				Key.Bits[2] = static_cast<uint32>(static_cast<int32>(FMath::Floor((P.Z - Bounds.Min.Z) / Tolerance)) + DZ);
				return Key;
			};
			std::unordered_map<FPositionKey, std::vector<uint32>, FPositionKeyHash> Cells;
			Cells.reserve(Vertices.size());
			PositionOf.resize(Vertices.size());
			for (uint32 Index = 0; Index < Vertices.size(); ++Index)
			{
				const FVector3& P     = Vertices[Index].Position;
				uint32          Found = ~0u;
				for (int32 DZ = -1; DZ <= 1 && Found == ~0u; ++DZ)
				{
					for (int32 DY = -1; DY <= 1 && Found == ~0u; ++DY)
					{
						for (int32 DX = -1; DX <= 1 && Found == ~0u; ++DX)
						{
							const auto It = Cells.find(CellOf(P, DX, DY, DZ));
							if (It == Cells.end())
							{
								continue;
							}
							for (const uint32 Candidate : It->second)
							{
								if (FVector3::DistanceSquared(Positions[Candidate], P) <= Tolerance * Tolerance)
								{
									Found = Candidate;
									break;
								}
							}
						}
					}
				}
				if (Found == ~0u)
				{
					Found = static_cast<uint32>(Positions.size());
					Positions.push_back(P);
					Cells[CellOf(P, 0, 0, 0)].push_back(Found);
				}
				PositionOf[Index] = Found;
			}
			const size_t PositionCount = Positions.size();
			AttributesOf.resize(PositionCount);
			for (uint32 Index = 0; Index < Vertices.size(); ++Index)
			{
				AttributesOf[PositionOf[Index]].push_back(Index);
			}

			TrianglesOf.resize(PositionCount);
			Quadrics.resize(PositionCount);
			Stamps.assign(PositionCount, 0);
			bDead.assign(PositionCount, 0);
			bBorder.assign(PositionCount, 0);
			bLocked.assign(PositionCount, 0);

			std::unordered_map<uint64, uint32> EdgeUse;
			EdgeUse.reserve(Indices.size());
			for (size_t Index = 0; Index + 2 < Indices.size(); Index += 3)
			{
				const std::array<uint32, 3> Tri = { Indices[Index], Indices[Index + 1], Indices[Index + 2] };
				const uint32 P0 = PositionOf[Tri[0]], P1 = PositionOf[Tri[1]], P2 = PositionOf[Tri[2]];
				if (P0 == P1 || P1 == P2 || P0 == P2)
				{
					continue; // 원본의 퇴화 삼각형은 버린다
				}
				const uint32 TriangleIndex = static_cast<uint32>(Triangles.size());
				Triangles.push_back(Tri);
				bAlive.push_back(1);
				++AliveCount;
				for (const uint32 Position : { P0, P1, P2 })
				{
					TrianglesOf[Position].push_back(TriangleIndex);
				}
				++EdgeUse[EdgeKey(P0, P1)];
				++EdgeUse[EdgeKey(P1, P2)];
				++EdgeUse[EdgeKey(P2, P0)];

				// 면 평면 이차식 (넓이 가중)
				const FVector3 Cross  = FVector3::Cross(Positions[P1] - Positions[P0], Positions[P2] - Positions[P0]);
				const float    Length = Cross.Length();
				if (Length > 0.0f)
				{
					const FVector3 N = Cross / Length;
					const double   D = -FVector3::Dot(N, Positions[P0]);
					FQuadric       Plane;
					Plane.AddPlane(N.X, N.Y, N.Z, D, 0.5 * Length);
					for (const uint32 Position : { P0, P1, P2 })
					{
						Quadrics[Position] += Plane;
					}
				}
			}
			for (const auto& [Key, Count] : EdgeUse)
			{
				const uint32 A = static_cast<uint32>(Key >> 32);
				const uint32 B = static_cast<uint32>(Key & 0xFFFFFFFFu);
				if (Count == 1)
				{
					bBorder[A] = bBorder[B] = 1;
					BorderEdges.emplace(Key, static_cast<uint8>(1));
				}
				else if (Count > 2)
				{
					bLocked[A] = bLocked[B] = 1; // 비다양체
				}
			}

			// 모든 모서리 후보
			for (const auto& [Key, Count] : EdgeUse)
			{
				const uint32 A = static_cast<uint32>(Key >> 32);
				const uint32 B = static_cast<uint32>(Key & 0xFFFFFFFFu);
				Push(A, B);
				Push(B, A);
			}
		}

		uint32 GetAliveCount() const { return AliveCount; }

		void Run(uint32 TargetTriangles)
		{
			while (AliveCount > TargetTriangles && !Queue.empty())
			{
				const FCollapse Top = Queue.top();
				Queue.pop();
				if (bDead[Top.From] || bDead[Top.To] || Stamps[Top.From] != Top.FromStamp || Stamps[Top.To] != Top.ToStamp)
				{
					continue; // 오래된 후보
				}
				if (!Collapse(Top.From, Top.To))
				{
					continue; // 지금은 붕괴할 수 없다 (이웃이 바뀌면 다시 후보가 된다)
				}
			}
		}

		std::vector<uint32> GetIndices() const
		{
			std::vector<uint32> Result;
			Result.reserve(static_cast<size_t>(AliveCount) * 3);
			for (size_t Index = 0; Index < Triangles.size(); ++Index)
			{
				if (bAlive[Index])
				{
					Result.insert(Result.end(), Triangles[Index].begin(), Triangles[Index].end());
				}
			}
			return Result;
		}

	private:
		void Push(uint32 From, uint32 To)
		{
			if (bLocked[From] || bDead[From] || bDead[To])
			{
				return;
			}
			FQuadric Combined = Quadrics[From];
			Combined += Quadrics[To];
			const double LengthSq = FVector3::DistanceSquared(Positions[From], Positions[To]);
			// 오차가 같으면(평면 등) 짧은 모서리부터 → 가는 삼각형이 덜 생긴다
			Queue.push({ FMath::Max(Combined.Evaluate(Positions[To]), 0.0) + LengthSq * 1.0e-6, From, To, Stamps[From], Stamps[To] });
		}

		void CollectNeighbors(uint32 Position, std::vector<uint32>& Out) const
		{
			Out.clear();
			for (const uint32 Triangle : TrianglesOf[Position])
			{
				if (!bAlive[Triangle])
				{
					continue;
				}
				for (const uint32 Attribute : Triangles[Triangle])
				{
					const uint32 Other = PositionOf[Attribute];
					if (Other != Position && std::find(Out.begin(), Out.end(), Other) == Out.end())
					{
						Out.push_back(Other);
					}
				}
			}
		}

		bool SatisfiesLinkCondition(uint32 From, uint32 To)
		{
			CollectNeighbors(From, FromNeighbors);
			CollectNeighbors(To, ToNeighbors);
			// 사라지는 삼각형(From과 To를 모두 가진 것)의 세 번째 위치
			Apexes.clear();
			for (const uint32 Triangle : TrianglesOf[From])
			{
				if (!bAlive[Triangle])
				{
					continue;
				}
				bool   bHasTo = false;
				uint32 Apex   = ~0u;
				for (const uint32 Attribute : Triangles[Triangle])
				{
					const uint32 Position = PositionOf[Attribute];
					if (Position == To)
					{
						bHasTo = true;
					}
					else if (Position != From)
					{
						Apex = Position;
					}
				}
				if (bHasTo && Apex != ~0u && std::find(Apexes.begin(), Apexes.end(), Apex) == Apexes.end())
				{
					Apexes.push_back(Apex);
				}
			}
			for (const uint32 Neighbor : FromNeighbors)
			{
				if (Neighbor != To && std::find(ToNeighbors.begin(), ToNeighbors.end(), Neighbor) != ToNeighbors.end() &&
				    std::find(Apexes.begin(), Apexes.end(), Neighbor) == Apexes.end())
				{
					return false;
				}
			}
			return true;
		}

		// From → To 붕괴. 할 수 없으면 false (상태 변경 없음)
		bool Collapse(uint32 From, uint32 To)
		{
			// 경계 위치는 경계 모서리를 따라서만
			if (bBorder[From] && (!bBorder[To] || !BorderEdges.contains(EdgeKey(From, To))))
			{
				return false;
			}

			// 속성 정점 대응: From의 속성 p마다 같은 삼각형에 있는 To의 속성 q
			Remap.clear();
			bool bAdjacent = false;
			for (const uint32 Triangle : TrianglesOf[From])
			{
				if (!bAlive[Triangle])
				{
					continue;
				}
				const std::array<uint32, 3>& Tri = Triangles[Triangle];
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					if (PositionOf[Tri[Corner]] != From)
					{
						continue;
					}
					for (int32 Other = 0; Other < 3; ++Other)
					{
						if (PositionOf[Tri[Other]] == To)
						{
							bAdjacent = true;
							if (std::find_if(Remap.begin(), Remap.end(), [&](const auto& Pair) { return Pair.first == Tri[Corner]; }) == Remap.end())
							{
								Remap.emplace_back(Tri[Corner], Tri[Other]);
							}
						}
					}
				}
			}
			if (!bAdjacent)
			{
				return false;
			}
			for (const uint32 Attribute : AttributesOf[From])
			{
				// 살아 있는 삼각형이 쓰는 속성만 대응이 필요하다
				bool bUsed = false;
				for (const uint32 Triangle : TrianglesOf[From])
				{
					if (bAlive[Triangle] && std::find(Triangles[Triangle].begin(), Triangles[Triangle].end(), Attribute) != Triangles[Triangle].end())
					{
						bUsed = true;
						break;
					}
				}
				if (bUsed && std::find_if(Remap.begin(), Remap.end(), [&](const auto& Pair) { return Pair.first == Attribute; }) == Remap.end())
				{
					return false; // 이음매를 가로지른다
				}
			}

			// 연결 조건(link condition): From과 To의 공통 이웃 = 사라지는 삼각형의 세 번째 꼭짓점이어야 한다.
			// 아니면 붕괴 뒤 비다양체(같은 모서리를 세 삼각형이 공유)가 되어 나중에 구멍이 생긴다
			if (!SatisfiesLinkCondition(From, To))
			{
				return false;
			}

			// 뒤집힘 검사: 살아남는 삼각형(To를 포함하지 않는 것)의 법선이 반대가 되면 안 된다
			for (const uint32 Triangle : TrianglesOf[From])
			{
				if (!bAlive[Triangle])
				{
					continue;
				}
				const std::array<uint32, 3>& Tri = Triangles[Triangle];
				FVector3                     Before[3];
				FVector3                     After[3];
				bool                         bHasTo = false;
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const uint32 Position = PositionOf[Tri[Corner]];
					bHasTo |= Position == To;
					Before[Corner] = Positions[Position];
					After[Corner]  = Position == From ? Positions[To] : Positions[Position];
				}
				if (bHasTo)
				{
					continue; // 사라지는 삼각형
				}
				const FVector3 OldNormal = FVector3::Cross(Before[1] - Before[0], Before[2] - Before[0]);
				const FVector3 NewNormal = FVector3::Cross(After[1] - After[0], After[2] - After[0]);
				if (FVector3::Dot(OldNormal, NewNormal) <= 0.2f * OldNormal.Length() * NewNormal.Length())
				{
					return false; // 뒤집히거나 크게 꺾인다 (78도 이상)
				}
			}

			// ---- 적용
			for (const uint32 Triangle : TrianglesOf[From])
			{
				if (!bAlive[Triangle])
				{
					continue;
				}
				std::array<uint32, 3>& Tri    = Triangles[Triangle];
				bool                   bHasTo = false;
				for (const uint32 Attribute : Tri)
				{
					bHasTo |= PositionOf[Attribute] == To;
				}
				if (bHasTo)
				{
					bAlive[Triangle] = 0;
					--AliveCount;
					continue;
				}
				for (uint32& Attribute : Tri)
				{
					if (PositionOf[Attribute] == From)
					{
						Attribute = std::find_if(Remap.begin(), Remap.end(), [&](const auto& Pair) { return Pair.first == Attribute; })->second;
					}
				}
				TrianglesOf[To].push_back(Triangle);
			}
			TrianglesOf[From].clear();
			Quadrics[To] += Quadrics[From];
			bDead[From] = 1;
			++Stamps[From];
			++Stamps[To];

			// 죽은 삼각형 정리 + To 주변 후보 다시 계산
			std::vector<uint32>& ToTriangles = TrianglesOf[To];
			ToTriangles.erase(std::remove_if(ToTriangles.begin(), ToTriangles.end(), [&](uint32 Triangle) { return !bAlive[Triangle]; }), ToTriangles.end());
			Neighbors.clear();
			for (const uint32 Triangle : ToTriangles)
			{
				for (const uint32 Attribute : Triangles[Triangle])
				{
					const uint32 Position = PositionOf[Attribute];
					if (Position != To && std::find(Neighbors.begin(), Neighbors.end(), Position) == Neighbors.end())
					{
						Neighbors.push_back(Position);
					}
				}
			}
			// To가 낀 후보는 도장이 바뀌어 무효 → 다시 계산. 이웃의 다른 모서리도 (전에 뒤집힘 등으로 거절됐을 수 있으니) 다시 넣는다
			for (const uint32 Neighbor : Neighbors)
			{
				Push(To, Neighbor);
				Push(Neighbor, To);
				for (const uint32 Triangle : TrianglesOf[Neighbor])
				{
					if (!bAlive[Triangle])
					{
						continue;
					}
					for (const uint32 Attribute : Triangles[Triangle])
					{
						const uint32 Position = PositionOf[Attribute];
						if (Position != Neighbor && Position != To)
						{
							Push(Neighbor, Position);
						}
					}
				}
			}
			return true;
		}

		const std::vector<FVertex>& Vertices;
		std::vector<FVector3>              Positions;
		std::vector<uint32>                PositionOf;   // 속성 정점 → 위치
		std::vector<std::vector<uint32>>   AttributesOf; // 위치 → 속성 정점들
		std::vector<std::vector<uint32>>   TrianglesOf;  // 위치 → 삼각형 (죽은 것 포함 가능)
		std::vector<std::array<uint32, 3>> Triangles;
		std::vector<uint8>                 bAlive;
		std::vector<FQuadric>              Quadrics;
		std::vector<uint32>                Stamps;
		std::vector<uint8>                 bDead;
		std::vector<uint8>                 bBorder;
		std::vector<uint8>                 bLocked;
		std::unordered_map<uint64, uint8>  BorderEdges;
		std::priority_queue<FCollapse, std::vector<FCollapse>, std::greater<FCollapse>> Queue;
		std::vector<std::pair<uint32, uint32>> Remap;     // 붕괴 하나의 속성 대응 (재사용)
		std::vector<uint32>                    Neighbors; // 재사용
		std::vector<uint32>                    FromNeighbors;
		std::vector<uint32>                    ToNeighbors;
		std::vector<uint32>                    Apexes;
		uint32                                 AliveCount = 0;
	};
} // namespace

namespace MeshSimplifier
{
	std::vector<uint32> SimplifyToTriangleCount(const std::vector<FVertex>& Vertices, const std::vector<uint32>& Indices, uint32 TargetTriangles)
	{
		FSimplifier Simplifier(Vertices, Indices);
		Simplifier.Run(TargetTriangles);
		return Simplifier.GetIndices();
	}

	void GenerateLods(FMeshData& Mesh, uint32 LodCount)
	{
		Mesh.Lods.clear();
		const uint32 SourceTriangles = static_cast<uint32>(Mesh.Indices.size() / 3);
		if (LodCount <= 1 || SourceTriangles < MinTriangles)
		{
			return;
		}
		LodCount = std::min(LodCount, LodMath::MaxLods);

		FSimplifier Simplifier(Mesh.Vertices, Mesh.Indices);
		uint32      PrevTriangles = SourceTriangles;
		for (uint32 Lod = 1; Lod < LodCount; ++Lod)
		{
			const uint32 Target = std::max<uint32>(static_cast<uint32>(static_cast<float>(SourceTriangles) * LodMath::DefaultTriangleRatios[Lod]), 4u);
			Simplifier.Run(Target);
			const uint32 Triangles = Simplifier.GetAliveCount();
			if (Triangles == 0 || static_cast<float>(Triangles) > 0.85f * static_cast<float>(PrevTriangles))
			{
				break; // 더 줄일 수 없다 (이음매·경계가 많은 메시)
			}
			FMeshLod& Level  = Mesh.Lods.emplace_back();
			Level.Indices    = Simplifier.GetIndices();
			Level.ScreenSize = LodMath::DefaultScreenSizes[Lod];
			PrevTriangles    = Triangles;
		}
	}

	void CompactVertices(FMeshData& Mesh)
	{
		std::vector<uint32>  Remap(Mesh.Vertices.size(), UINT32_MAX);
		std::vector<FVertex> Vertices;
		const auto Visit = [&](std::vector<uint32>& Indices) {
			for (uint32& Index : Indices)
			{
				if (Remap[Index] == UINT32_MAX)
				{
					Remap[Index] = static_cast<uint32>(Vertices.size());
					Vertices.push_back(Mesh.Vertices[Index]);
				}
				Index = Remap[Index];
			}
		};
		Visit(Mesh.Indices);
		for (FMeshLod& Level : Mesh.Lods)
		{
			Visit(Level.Indices);
		}
		Mesh.Vertices = std::move(Vertices);
	}

	void SimplifyBase(FMeshData& Mesh, uint32 TargetTriangles)
	{
		if (TargetTriangles == 0 || Mesh.Indices.size() / 3 <= TargetTriangles)
		{
			return;
		}
		Mesh.Lods.clear();
		Mesh.Indices = SimplifyToTriangleCount(Mesh.Vertices, Mesh.Indices, TargetTriangles);
		CompactVertices(Mesh);
	}

	uint32 FindIslands(const FMeshData& Mesh, std::vector<uint32>& OutTriangleIsland)
	{
		// 위치가 같은 정점을 한 점으로 묶은 뒤(UV 이음매로 갈라진 정점도 같은 섬), 삼각형 꼭짓점끼리 합집합
		const size_t        VertexCount = Mesh.Vertices.size();
		std::vector<uint32> Parent(VertexCount);
		for (size_t Index = 0; Index < VertexCount; ++Index)
		{
			Parent[Index] = static_cast<uint32>(Index);
		}
		const auto Find = [&Parent](uint32 X) {
			while (Parent[X] != X)
			{
				Parent[X] = Parent[Parent[X]];
				X         = Parent[X];
			}
			return X;
		};
		const auto Union = [&](uint32 A, uint32 B) {
			A = Find(A);
			B = Find(B);
			if (A != B)
			{
				Parent[std::max(A, B)] = std::min(A, B);
			}
		};
		struct FPositionKey
		{
			float X, Y, Z;
			bool  operator==(const FPositionKey& Other) const { return X == Other.X && Y == Other.Y && Z == Other.Z; }
		};
		struct FPositionHash
		{
			size_t operator()(const FPositionKey& Key) const
			{
				uint32 Bits[3];
				std::memcpy(Bits, &Key, sizeof(Bits));
				return (static_cast<size_t>(Bits[0]) * 73856093u) ^ (static_cast<size_t>(Bits[1]) * 19349663u) ^ (static_cast<size_t>(Bits[2]) * 83492791u);
			}
		};
		std::unordered_map<FPositionKey, uint32, FPositionHash> FirstAtPosition;
		FirstAtPosition.reserve(VertexCount);
		for (size_t Index = 0; Index < VertexCount; ++Index)
		{
			const FVector3& P = Mesh.Vertices[Index].Position;
			const auto [It, bInserted] = FirstAtPosition.emplace(FPositionKey{ P.X, P.Y, P.Z }, static_cast<uint32>(Index));
			if (!bInserted)
			{
				Union(It->second, static_cast<uint32>(Index));
			}
		}
		const size_t TriangleCount = Mesh.Indices.size() / 3;
		for (size_t Tri = 0; Tri < TriangleCount; ++Tri)
		{
			Union(Mesh.Indices[Tri * 3], Mesh.Indices[Tri * 3 + 1]);
			Union(Mesh.Indices[Tri * 3], Mesh.Indices[Tri * 3 + 2]);
		}
		std::unordered_map<uint32, uint32> RootToIsland;
		OutTriangleIsland.resize(TriangleCount);
		for (size_t Tri = 0; Tri < TriangleCount; ++Tri)
		{
			const uint32 Root = Find(Mesh.Indices[Tri * 3]);
			const auto [It, bInserted] = RootToIsland.emplace(Root, static_cast<uint32>(RootToIsland.size()));
			OutTriangleIsland[Tri] = It->second;
		}
		return static_cast<uint32>(RootToIsland.size());
	}

	bool IsIslandMesh(const FMeshData& Mesh)
	{
		std::vector<uint32> TriangleIsland;
		const uint32        Islands = FindIslands(Mesh, TriangleIsland);
		return Islands >= MinIslands && TriangleIsland.size() <= static_cast<size_t>(Islands) * MaxIslandTriangles;
	}
} // namespace MeshSimplifier

namespace
{
	uint64 SplitMix64(uint64 X)
	{
		X += 0x9E3779B97F4A7C15ull;
		X = (X ^ (X >> 30)) * 0xBF58476D1CE4E5B9ull;
		X = (X ^ (X >> 27)) * 0x94D049BB133111EBull;
		return X ^ (X >> 31);
	}

	// 남길 섬 (고정 해시 순 앞에서 Keep개 — 공간적으로 고르게 흩어진다) + 섬별 무게중심 + 키울 배율
	struct FIslandSelection
	{
		std::vector<uint8>    bKeep;
		std::vector<FVector3> Centroids;
		float                 Scale = 1.0f;
	};

	FIslandSelection SelectIslands(const FMeshData& Mesh, const std::vector<uint32>& TriangleIsland, uint32 Islands, float KeepRatio)
	{
		FIslandSelection Result;
		Result.bKeep.assign(Islands, 0);
		Result.Centroids.assign(Islands, FVector3::ZeroVector);
		std::vector<uint32> Counts(Islands, 0);
		for (size_t Tri = 0; Tri < TriangleIsland.size(); ++Tri)
		{
			const uint32 Island = TriangleIsland[Tri];
			for (uint32 Corner = 0; Corner < 3; ++Corner)
			{
				Result.Centroids[Island] += Mesh.Vertices[Mesh.Indices[Tri * 3 + Corner]].Position;
			}
			Counts[Island] += 3;
		}
		for (uint32 Island = 0; Island < Islands; ++Island)
		{
			Result.Centroids[Island] = Result.Centroids[Island] / static_cast<float>(std::max(Counts[Island], 1u));
		}
		std::vector<std::pair<uint64, uint32>> Order(Islands);
		for (uint32 Island = 0; Island < Islands; ++Island)
		{
			Order[Island] = { SplitMix64(Island), Island };
		}
		std::sort(Order.begin(), Order.end());
		const uint32 Keep = std::clamp<uint32>(static_cast<uint32>(std::lround(static_cast<double>(Islands) * KeepRatio)), 1u, Islands);
		for (uint32 Index = 0; Index < Keep; ++Index)
		{
			Result.bKeep[Order[Index].second] = 1;
		}
		const float ActualRatio = static_cast<float>(Keep) / static_cast<float>(Islands);
		Result.Scale            = std::min(1.0f / std::sqrt(ActualRatio), MeshSimplifier::MaxThinningScale);
		return Result;
	}

	// 남긴 섬의 삼각형을 키운 정점으로 Vertices 뒤에 덧붙이고 그 인덱스를 돌려준다
	std::vector<uint32> AppendThinned(std::vector<FVertex>& Vertices, const FMeshData& Source, const std::vector<uint32>& TriangleIsland,
		const FIslandSelection& Selection)
	{
		std::vector<uint32>                Indices;
		std::unordered_map<uint32, uint32> Remap;
		for (size_t Tri = 0; Tri < TriangleIsland.size(); ++Tri)
		{
			const uint32 Island = TriangleIsland[Tri];
			if (!Selection.bKeep[Island])
			{
				continue;
			}
			for (uint32 Corner = 0; Corner < 3; ++Corner)
			{
				const uint32 SourceIndex = Source.Indices[Tri * 3 + Corner];
				const auto [It, bInserted] = Remap.emplace(SourceIndex, static_cast<uint32>(Vertices.size()));
				if (bInserted)
				{
					FVertex Vertex  = Source.Vertices[SourceIndex];
					Vertex.Position = Selection.Centroids[Island] + (Vertex.Position - Selection.Centroids[Island]) * Selection.Scale;
					Vertices.push_back(Vertex);
				}
				Indices.push_back(It->second);
			}
		}
		return Indices;
	}
} // namespace

namespace MeshSimplifier
{
	void ThinBase(FMeshData& Mesh, uint32 TargetTriangles)
	{
		const size_t TriangleCount = Mesh.Indices.size() / 3;
		if (TargetTriangles == 0 || TriangleCount <= TargetTriangles)
		{
			return;
		}
		std::vector<uint32>    TriangleIsland;
		const uint32           Islands   = FindIslands(Mesh, TriangleIsland);
		const FIslandSelection Selection = SelectIslands(Mesh, TriangleIsland, Islands, static_cast<float>(TargetTriangles) / static_cast<float>(TriangleCount));
		std::vector<FVertex>   Vertices;
		std::vector<uint32>    Indices = AppendThinned(Vertices, Mesh, TriangleIsland, Selection);
		Mesh.Vertices                  = std::move(Vertices);
		Mesh.Indices                   = std::move(Indices);
		Mesh.Lods.clear();
	}

	void GenerateIslandLods(FMeshData& Mesh, uint32 LodCount)
	{
		Mesh.Lods.clear();
		LodCount = std::min(LodCount, LodMath::MaxLods);
		if (LodCount <= 1)
		{
			return;
		}
		std::vector<uint32> TriangleIsland;
		const uint32        Islands = FindIslands(Mesh, TriangleIsland);
		const FMeshData     Base    = Mesh; // 덧붙이는 동안 LOD0 정점·인덱스를 읽는다
		for (uint32 Lod = 1; Lod < LodCount; ++Lod)
		{
			const FIslandSelection Selection = SelectIslands(Base, TriangleIsland, Islands, LodMath::DefaultTriangleRatios[Lod]);
			FMeshLod&              Level     = Mesh.Lods.emplace_back();
			Level.Indices                    = AppendThinned(Mesh.Vertices, Base, TriangleIsland, Selection);
			Level.ScreenSize                 = LodMath::DefaultScreenSizes[Lod];
		}
	}
} // namespace MeshSimplifier
