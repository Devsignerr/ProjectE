# 모듈식 키트 glTF 조립 도우미 — Poly Haven 모듈 키트(파사드·홈통·배관·철망)는 조각이 한 파일에 격자로 흩어져 있으므로,
# 고른 조각만 원하는 자리에 놓은 "조립 glTF"를 만든다 (엔진 모델 = 파일 하나라 노드 선택 기능이 없음).
#   조립 파일은 원본 키트의 .bin/텍스처를 상대 경로로 참조만 한다(기하·텍스처 복사 없음 — 원본은 FetchDemoAssets로 받음).
#   좌표는 glTF 공간(+Y 위, 미터, 오른손). 엔진 변환: 엔진 (X, Y, Z) cm = (-z, x, y) × 100 (FGltfLoader::ConvertPosition)
import json
import math
import os

import numpy as np


def Translate(X, Y, Z):
	M = np.eye(4)
	M[0:3, 3] = (X, Y, Z)
	return M


def RotateY(Degrees):
	# glTF +Y 축 둘레 회전 (오른손). 엔진 Yaw θ = glTF RotateY(-θ)
	R = math.radians(Degrees)
	C, S = math.cos(R), math.sin(R)
	M = np.eye(4)
	M[0, 0], M[0, 2], M[2, 0], M[2, 2] = C, S, -S, C
	return M


def RotateX(Degrees):
	R = math.radians(Degrees)
	C, S = math.cos(R), math.sin(R)
	M = np.eye(4)
	M[1, 1], M[1, 2], M[2, 1], M[2, 2] = C, -S, S, C
	return M


def RotateZ(Degrees):
	R = math.radians(Degrees)
	C, S = math.cos(R), math.sin(R)
	M = np.eye(4)
	M[0, 0], M[0, 1], M[1, 0], M[1, 1] = C, -S, S, C
	return M


def EngineToGltf(X, Y, Z, Yaw=0.0):
	# 엔진 위치(cm) + Yaw(도) → glTF 행렬 (조립 파일을 엔진 원점에 놓을 때)
	return Translate(Y / 100.0, Z / 100.0, -X / 100.0) @ RotateY(-Yaw)


def _NodeLocal(Node):
	# 키트 노드의 회전·스케일만 (이동은 키트 격자 배치라 버린다)
	if "matrix" in Node:
		M = np.array(Node["matrix"], dtype=float).reshape(4, 4).T
		M[0:3, 3] = 0.0
		return M
	X, Y, Z, W = Node.get("rotation", [0.0, 0.0, 0.0, 1.0])
	R = np.array([[1 - 2 * (Y * Y + Z * Z), 2 * (X * Y - Z * W), 2 * (X * Z + Y * W), 0],
				  [2 * (X * Y + Z * W), 1 - 2 * (X * X + Z * Z), 2 * (Y * Z - X * W), 0],
				  [2 * (X * Z - Y * W), 2 * (Y * Z + X * W), 1 - 2 * (X * X + Y * Y), 0],
				  [0, 0, 0, 1]], dtype=float)
	S = np.diag(list(Node.get("scale", [1.0, 1.0, 1.0])) + [1.0])
	return R @ S


class FGltfKitComposer:
	def __init__(self, KitRoot, OutPath):
		self.KitRoot = KitRoot   # 키트 폴더들의 부모 (…/Asset/PolyHaven)
		self.OutPath = OutPath
		self.Kits = {}           # Id → 원본 JSON
		self.Parts = []          # (Id, 노드 이름, 행렬)

	def Kit(self, Id):
		if Id not in self.Kits:
			with open(os.path.join(self.KitRoot, Id, f"{Id}.gltf"), encoding="utf-8") as File:
				self.Kits[Id] = json.load(File)
		return self.Kits[Id]

	def NodeNames(self, Id):
		return [Node.get("name", "") for Node in self.Kit(Id)["nodes"]]

	def Has(self, Id, Name):
		return Name in self.NodeNames(Id)

	def Add(self, Id, Name, Matrix):
		Nodes = self.Kit(Id)["nodes"]
		for Node in Nodes:
			if Node.get("name") == Name:
				if "mesh" not in Node:
					raise ValueError(f"{Id}/{Name}: 메시 없음")
				self.Parts.append((Id, Name, Matrix @ _NodeLocal(Node)))
				return
		raise KeyError(f"{Id}: 노드 '{Name}' 없음")

	def Save(self):
		OutDir = os.path.dirname(self.OutPath)
		Out = {"asset": {"version": "2.0", "generator": "ProjectE Tools/DemoMap/GltfKit.py"}, "scene": 0,
			   "scenes": [{"nodes": []}], "nodes": [], "meshes": [], "accessors": [], "bufferViews": [], "buffers": [],
			   "materials": [], "textures": [], "images": [], "samplers": []}
		Extensions = set()
		KitMaps = {}    # Id → 인덱스 표
		MeshMap = {}    # (Id, 원본 메시) → 새 메시

		def KitMap(Id):
			if Id in KitMaps:
				return KitMaps[Id]
			Src = self.Kit(Id)
			Extensions.update(Src.get("extensionsUsed", []))
			Rel = os.path.relpath(os.path.join(self.KitRoot, Id), OutDir).replace("\\", "/")
			Map = {"Buffer": {}, "View": {}, "Accessor": {}, "Material": {}, "Texture": {}, "Image": {}, "Sampler": {}, "Rel": Rel}
			KitMaps[Id] = Map
			return Map

		def MapBuffer(Id, Index):
			Map = KitMap(Id)
			if Index not in Map["Buffer"]:
				Buffer = dict(self.Kit(Id)["buffers"][Index])
				Buffer["uri"] = f"{Map['Rel']}/{Buffer['uri']}"
				Map["Buffer"][Index] = len(Out["buffers"])
				Out["buffers"].append(Buffer)
			return Map["Buffer"][Index]

		def MapView(Id, Index):
			Map = KitMap(Id)
			if Index not in Map["View"]:
				View = dict(self.Kit(Id)["bufferViews"][Index])
				View["buffer"] = MapBuffer(Id, View["buffer"])
				Map["View"][Index] = len(Out["bufferViews"])
				Out["bufferViews"].append(View)
			return Map["View"][Index]

		def MapAccessor(Id, Index):
			Map = KitMap(Id)
			if Index not in Map["Accessor"]:
				Accessor = json.loads(json.dumps(self.Kit(Id)["accessors"][Index]))
				if "bufferView" in Accessor:
					Accessor["bufferView"] = MapView(Id, Accessor["bufferView"])
				if "sparse" in Accessor:
					raise ValueError(f"{Id}: sparse 접근자는 지원하지 않음")
				Map["Accessor"][Index] = len(Out["accessors"])
				Out["accessors"].append(Accessor)
			return Map["Accessor"][Index]

		def MapSampler(Id, Index):
			Map = KitMap(Id)
			if Index not in Map["Sampler"]:
				Map["Sampler"][Index] = len(Out["samplers"])
				Out["samplers"].append(dict(self.Kit(Id)["samplers"][Index]))
			return Map["Sampler"][Index]

		def MapImage(Id, Index):
			Map = KitMap(Id)
			if Index not in Map["Image"]:
				Image = dict(self.Kit(Id)["images"][Index])
				if "uri" not in Image:
					raise ValueError(f"{Id}: 버퍼 안 이미지는 지원하지 않음")
				Image["uri"] = f"{Map['Rel']}/{Image['uri']}"
				Map["Image"][Index] = len(Out["images"])
				Out["images"].append(Image)
			return Map["Image"][Index]

		def MapTexture(Id, Index):
			Map = KitMap(Id)
			if Index not in Map["Texture"]:
				Texture = json.loads(json.dumps(self.Kit(Id)["textures"][Index]))
				if "source" in Texture:
					Texture["source"] = MapImage(Id, Texture["source"])
				if "sampler" in Texture:
					Texture["sampler"] = MapSampler(Id, Texture["sampler"])
				for Ext in Texture.get("extensions", {}).values():
					if "source" in Ext:
						Ext["source"] = MapImage(Id, Ext["source"])
				Map["Texture"][Index] = len(Out["textures"])
				Out["textures"].append(Texture)
			return Map["Texture"][Index]

		def RemapTextureRefs(Value, Id, Key=""):
			if isinstance(Value, dict):
				if Key.endswith("Texture") and "index" in Value:
					Value["index"] = MapTexture(Id, Value["index"])
				for K, V in Value.items():
					RemapTextureRefs(V, Id, K)
			elif isinstance(Value, list):
				for V in Value:
					RemapTextureRefs(V, Id, Key)

		def MapMaterial(Id, Index):
			Map = KitMap(Id)
			if Index not in Map["Material"]:
				Material = json.loads(json.dumps(self.Kit(Id)["materials"][Index]))
				RemapTextureRefs(Material, Id)
				Map["Material"][Index] = len(Out["materials"])
				Out["materials"].append(Material)
			return Map["Material"][Index]

		def MapMesh(Id, Index):
			if (Id, Index) not in MeshMap:
				Mesh = json.loads(json.dumps(self.Kit(Id)["meshes"][Index]))
				for Prim in Mesh["primitives"]:
					Prim["attributes"] = {K: MapAccessor(Id, V) for K, V in Prim["attributes"].items()}
					if "indices" in Prim:
						Prim["indices"] = MapAccessor(Id, Prim["indices"])
					if "material" in Prim:
						Prim["material"] = MapMaterial(Id, Prim["material"])
					if "targets" in Prim:
						Prim["targets"] = [{K: MapAccessor(Id, V) for K, V in T.items()} for T in Prim["targets"]]
				MeshMap[(Id, Index)] = len(Out["meshes"])
				Out["meshes"].append(Mesh)
			return MeshMap[(Id, Index)]

		for Index, (Id, Name, Matrix) in enumerate(self.Parts):
			Node = next(N for N in self.Kit(Id)["nodes"] if N.get("name") == Name)
			Out["nodes"].append({"name": f"{Name}_{Index}", "mesh": MapMesh(Id, Node["mesh"]),
								 "matrix": [round(float(V), 6) + 0.0 for V in Matrix.T.reshape(-1)]})
			Out["scenes"][0]["nodes"].append(Index)
		for Key in ("textures", "images", "samplers", "materials"):
			if not Out[Key]:
				del Out[Key]
		if Extensions:
			Out["extensionsUsed"] = sorted(Extensions)
		os.makedirs(OutDir, exist_ok=True)
		with open(self.OutPath, "w", encoding="utf-8", newline="\n") as File:
			json.dump(Out, File, separators=(",", ":"))
			File.write("\n")
		return len(self.Parts)
