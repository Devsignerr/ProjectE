# glTF 모델의 경계 상자(엔진 좌표, cm)를 계산한다 — 배치 높이/크기 맞춤용 (노드 계층 변환 포함, 정점 min/max 8점 변환)
import json
import math
import os


def _QuatToMatrix(Q):
	X, Y, Z, W = Q
	return [[1 - 2 * (Y * Y + Z * Z), 2 * (X * Y - Z * W), 2 * (X * Z + Y * W)],
			[2 * (X * Y + Z * W), 1 - 2 * (X * X + Z * Z), 2 * (Y * Z - X * W)],
			[2 * (X * Z - Y * W), 2 * (Y * Z + X * W), 1 - 2 * (X * X + Y * Y)]]


def _NodeMatrix(Node):
	if "matrix" in Node:
		M = Node["matrix"]
		return [[M[0], M[4], M[8], M[12]], [M[1], M[5], M[9], M[13]], [M[2], M[6], M[10], M[14]], [0, 0, 0, 1]]
	T = Node.get("translation", [0, 0, 0])
	R = _QuatToMatrix(Node.get("rotation", [0, 0, 0, 1]))
	S = Node.get("scale", [1, 1, 1])
	return [[R[0][0] * S[0], R[0][1] * S[1], R[0][2] * S[2], T[0]],
			[R[1][0] * S[0], R[1][1] * S[1], R[1][2] * S[2], T[1]],
			[R[2][0] * S[0], R[2][1] * S[1], R[2][2] * S[2], T[2]], [0, 0, 0, 1]]


def _Mul(A, B):
	return [[sum(A[I][K] * B[K][J] for K in range(4)) for J in range(4)] for I in range(4)]


def _LoadJson(Path):
	if Path.lower().endswith(".glb"):
		import struct
		Data = open(Path, "rb").read()
		Length = struct.unpack("<I", Data[12:16])[0]
		return json.loads(Data[20:20 + Length])
	return json.load(open(Path, encoding="utf-8"))


def ComputeBounds(Path):
	# 반환: (Min, Max) 엔진 좌표 cm. glTF(+Y 위, 미터, 오른손) → 엔진(Z 위, cm, 왼손): FGltfLoader::ConvertPosition과 같은 축 매핑
	Gltf = _LoadJson(Path)
	Lo = [math.inf] * 3
	Hi = [-math.inf] * 3

	def Visit(Index, Parent):
		Node = Gltf["nodes"][Index]
		World = _Mul(Parent, _NodeMatrix(Node))
		if "mesh" in Node:
			for Prim in Gltf["meshes"][Node["mesh"]]["primitives"]:
				Acc = Gltf["accessors"][Prim["attributes"]["POSITION"]]
				Mn, Mx = Acc["min"], Acc["max"]
				for C in range(8):
					P = [Mx[0] if C & 1 else Mn[0], Mx[1] if C & 2 else Mn[1], Mx[2] if C & 4 else Mn[2], 1]
					W = [sum(World[R][K] * P[K] for K in range(4)) for R in range(3)]
					E = ConvertPosition(W)
					for A in range(3):
						Lo[A] = min(Lo[A], E[A])
						Hi[A] = max(Hi[A], E[A])
		for Child in Node.get("children", []):
			Visit(Child, World)

	Identity = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0], [0, 0, 0, 1]]
	Scene = Gltf["scenes"][Gltf.get("scene", 0)]
	for Root in Scene["nodes"]:
		Visit(Root, Identity)
	return Lo, Hi


def ConvertPosition(P):
	return _CONVERT(P)


_CONVERT = None
