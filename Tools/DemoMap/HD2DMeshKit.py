# HD-2D 데모용 절차 메시 glTF 쓰기 도우미 — 내장 도형(큐브·구·평면)으로는 어색한 둥근 소품(등대 탑·계류 기둥·나룻배·나무 부두 기둥·그물)을
#   직접 만든 정점으로 쓴다. 텍스처는 Poly Haven 2k 묶음(diff/arm/nor_gl)을 상대 경로로 참조만 한다(복사 없음 — 원본은 FetchDemoAssets).
#   UV는 월드 크기 기준(TexSize cm = 텍스처 한 장)이라 늘어지지 않는다. 결과 = <이름>.gltf + <이름>.bin (작아서 커밋).
#   좌표: 엔진 규약(왼손 Z-up, cm)으로 짓고 저장할 때 glTF(+Y 위, m, 오른손)로 바꾼다 — 엔진 (X, Y, Z) cm = (-gz, gx, gy) × 100
#   와인딩: 삼각형마다 glTF 공간에서 (P1-P0)×(P2-P0)·N > 0(반시계 = 앞면)으로 맞춘다 (로더가 엔진 CW로 뒤집는다 — CLAUDE.md glTF 임포트)
import json
import math
import os

import numpy as np

PH = "Asset/PolyHaven"


def _ToGltf(P):
	return (P[1] / 100.0, P[2] / 100.0, -P[0] / 100.0)


def _NToGltf(N):
	return (N[1], N[2], -N[0])


class FMaterialDef:
	# Stem: Poly Haven 묶음 이름 (예: "weathered_planks") — 없으면 단색. Tint = 기본색 배율, Emissive = 발광(선형), Mask = 알파 테스트(텍스처 알파)
	def __init__(self, Name, Stem=None, Tint=(1.0, 1.0, 1.0), Rough=1.0, Metal=0.0, Emissive=(0.0, 0.0, 0.0), Texture=None, Mask=False, DoubleSided=False,
				 NormalScale=1.0, Blend=False, EmissiveTexture=None, Alpha=1.0):
		# Blend = 반투명(glTF BLEND), EmissiveTexture = 발광 텍스처(이 glTF 폴더 기준), Alpha = 기본색 알파
		self.Name, self.Stem, self.Tint, self.Rough, self.Metal = Name, Stem, Tint, Rough, Metal
		self.Emissive, self.Texture, self.Mask, self.DoubleSided, self.NormalScale = Emissive, Texture, Mask, DoubleSided, NormalScale
		self.Blend, self.EmissiveTexture, self.Alpha = Blend, EmissiveTexture, Alpha


class FMeshBuilder:
	def __init__(self):
		self.Parts = {}   # 머티리얼 이름 → [정점 위치, 법선, UV, 인덱스]
		self.Materials = {}

	def Material(self, Def):
		self.Materials[Def.Name] = Def
		return Def.Name

	def _Part(self, Mat):
		if Mat not in self.Parts:
			self.Parts[Mat] = ([], [], [], [])
		return self.Parts[Mat]

	def Tri(self, Mat, A, B, C, NA, NB, NC, UA, UB, UC):
		P, N, U, I = self._Part(Mat)
		Base = len(P)
		P.extend((A, B, C))
		N.extend((NA, NB, NC))
		U.extend((UA, UB, UC))
		I.extend((Base, Base + 1, Base + 2))

	def Quad(self, Mat, A, B, C, D, NA, NB, NC, ND, UA, UB, UC, UD):
		# A-B-C-D 둘레 순서 (어느 방향이든 — 저장할 때 법선 기준으로 맞춘다)
		self.Tri(Mat, A, B, C, NA, NB, NC, UA, UB, UC)
		self.Tri(Mat, A, C, D, NA, NC, ND, UA, UC, UD)

	def FlatQuad(self, Mat, A, B, C, D, TexSize=100.0, UAxis=None):
		# 평면 사각형 (법선 = 면 법선, UV = 면 위 월드 거리 / TexSize)
		A, B, C, D = (np.array(V, dtype=float) for V in (A, B, C, D))
		Nn = np.cross(B - A, D - A)
		Nn = Nn / max(np.linalg.norm(Nn), 1e-9)
		Ua = (B - A) / max(np.linalg.norm(B - A), 1e-9) if UAxis is None else np.array(UAxis, dtype=float)
		Va = np.cross(Nn, Ua)

		def UV(P):
			return (float((P - A) @ Ua) / TexSize, float((P - A) @ Va) / TexSize)
		self.Quad(Mat, *(tuple(V) for V in (A, B, C, D)), *(tuple(Nn),) * 4, UV(A), UV(B), UV(C), UV(D))

	def Box(self, Mat, Center, Size, Yaw=0.0, Pitch=0.0, Roll=0.0, TexSize=100.0, Faces="xXyYzZ"):
		# 축 정렬 상자를 (Roll X → Pitch Y → Yaw Z) 순서로 돌려 놓는다. Faces: 그릴 면 (x = -X, X = +X …)
		HX, HY, HZ = (S * 0.5 for S in Size)
		R = _Rotation(Pitch, Yaw, Roll)
		C = np.array(Center, dtype=float)

		def W(X, Y, Z):
			return tuple(C + R @ np.array((X, Y, Z)))
		Corners = {}
		for SX in (-1, 1):
			for SY in (-1, 1):
				for SZ in (-1, 1):
					Corners[(SX, SY, SZ)] = W(SX * HX, SY * HY, SZ * HZ)
		Spec = {
			"x": [(-1, -1, -1), (-1, 1, -1), (-1, 1, 1), (-1, -1, 1)], "X": [(1, -1, -1), (1, -1, 1), (1, 1, 1), (1, 1, -1)],
			"y": [(-1, -1, -1), (-1, -1, 1), (1, -1, 1), (1, -1, -1)], "Y": [(-1, 1, -1), (1, 1, -1), (1, 1, 1), (-1, 1, 1)],
			"z": [(-1, -1, -1), (1, -1, -1), (1, 1, -1), (-1, 1, -1)], "Z": [(-1, -1, 1), (-1, 1, 1), (1, 1, 1), (1, -1, 1)],
		}
		for Face in Faces:
			Q = [Corners[K] for K in Spec[Face]]
			self.FlatQuad(Mat, *Q, TexSize=TexSize)

	def Frustum(self, Mat, Center, R0, R1, Height, Segments=16, TexSize=100.0, Caps=(True, True), Z0=0.0, USpan=None, Smooth=True, Phase=0.0):
		# 세운 원뿔대 (밑 반지름 R0, 위 R1). UV: u = 둘레 거리, v = 높이 (월드 cm / TexSize)
		CX, CY, CZ = Center
		Slope = (R0 - R1) / max(Height, 1e-6)
		Circ = 2.0 * math.pi * max(R0, R1)
		Cols = []
		for K in range(Segments + 1):
			A = Phase + K / Segments * 2.0 * math.pi
			Ca, Sa = math.cos(A), math.sin(A)
			Nn = np.array((Ca, Sa, Slope))
			Nn = Nn / np.linalg.norm(Nn)
			U = (USpan if USpan else Circ) * K / Segments / TexSize
			Cols.append(((CX + Ca * R0, CY + Sa * R0, CZ + Z0), (CX + Ca * R1, CY + Sa * R1, CZ + Z0 + Height), tuple(Nn), U))
		for K in range(Segments):
			(B0, T0, N0, U0), (B1, T1, N1, U1) = Cols[K], Cols[K + 1]
			if not Smooth:
				Mid = (Phase + (K + 0.5) / Segments * 2.0 * math.pi)
				Nf = np.array((math.cos(Mid), math.sin(Mid), Slope))
				N0 = N1 = tuple(Nf / np.linalg.norm(Nf))
			V0, V1 = Z0 / TexSize, (Z0 + Height) / TexSize
			self.Quad(Mat, B0, B1, T1, T0, N0, N1, N1, N0, (U0, V0), (U1, V0), (U1, V1), (U0, V1))
		for Index, (bCap, Z, R, Nz) in enumerate(((Caps[0], CZ + Z0, R0, -1.0), (Caps[1], CZ + Z0 + Height, R1, 1.0))):
			if not bCap or R <= 0.0:
				continue
			Mid = (CX, CY, Z)
			for K in range(Segments):
				A0 = Phase + K / Segments * 2.0 * math.pi
				A1 = Phase + (K + 1) / Segments * 2.0 * math.pi
				P0 = (CX + math.cos(A0) * R, CY + math.sin(A0) * R, Z)
				P1 = (CX + math.cos(A1) * R, CY + math.sin(A1) * R, Z)
				Nn = (0.0, 0.0, Nz)
				self.Tri(Mat, Mid, P0, P1, Nn, Nn, Nn, (CX / TexSize, CY / TexSize), (P0[0] / TexSize, P0[1] / TexSize), (P1[0] / TexSize, P1[1] / TexSize))

	def Lathe(self, Mat, Center, Profile, Segments=16, TexSize=100.0, Phase=0.0):
		# 회전체: Profile = [(반지름, 높이), ...] 아래에서 위로. 법선 = 옆 단면의 기울기
		CX, CY, CZ = Center
		for J in range(len(Profile) - 1):
			(RA, ZA), (RB, ZB) = Profile[J], Profile[J + 1]
			DZ, DR = ZB - ZA, RB - RA
			L = math.hypot(DZ, DR) or 1.0
			NR, NZ = DZ / L, -DR / L
			for K in range(Segments):
				A0 = Phase + K / Segments * 2.0 * math.pi
				A1 = Phase + (K + 1) / Segments * 2.0 * math.pi
				C0, S0, C1, S1 = math.cos(A0), math.sin(A0), math.cos(A1), math.sin(A1)
				P = [(CX + C0 * RA, CY + S0 * RA, CZ + ZA), (CX + C1 * RA, CY + S1 * RA, CZ + ZA), (CX + C1 * RB, CY + S1 * RB, CZ + ZB), (CX + C0 * RB, CY + S0 * RB, CZ + ZB)]
				N = [(C0 * NR, S0 * NR, NZ), (C1 * NR, S1 * NR, NZ), (C1 * NR, S1 * NR, NZ), (C0 * NR, S0 * NR, NZ)]
				Circ = 2.0 * math.pi * max(RA, RB, 1.0)
				U0, U1 = Circ * K / Segments / TexSize, Circ * (K + 1) / Segments / TexSize
				VA, VB = ZA / TexSize, ZB / TexSize
				if RA <= 0.01:
					self.Tri(Mat, P[0], P[2], P[3], N[0], N[2], N[3], (U0, VA), (U1, VB), (U0, VB))
				elif RB <= 0.01:
					self.Tri(Mat, P[0], P[1], P[2], N[0], N[1], N[2], (U0, VA), (U1, VA), (U1, VB))
				else:
					self.Quad(Mat, *P, *N, (U0, VA), (U1, VA), (U1, VB), (U0, VB))

	def Cylinder(self, Mat, A, B, Radius, Segments=10, TexSize=100.0, Caps=True):
		# 임의 방향 원기둥 (A → B 축)
		A, B = np.array(A, dtype=float), np.array(B, dtype=float)
		Axis = B - A
		L = float(np.linalg.norm(Axis))
		if L < 1e-6:
			return
		W = Axis / L
		Tmp = np.array((0.0, 0.0, 1.0)) if abs(W[2]) < 0.9 else np.array((1.0, 0.0, 0.0))
		Uv = np.cross(W, Tmp)
		Uv /= np.linalg.norm(Uv)
		Vv = np.cross(W, Uv)
		Ring = []
		for K in range(Segments + 1):
			Ang = K / Segments * 2.0 * math.pi
			D = Uv * math.cos(Ang) + Vv * math.sin(Ang)
			Ring.append((D, 2.0 * math.pi * Radius * K / Segments / TexSize))
		for K in range(Segments):
			(D0, U0), (D1, U1) = Ring[K], Ring[K + 1]
			P = [tuple(A + D0 * Radius), tuple(A + D1 * Radius), tuple(B + D1 * Radius), tuple(B + D0 * Radius)]
			N = [tuple(D0), tuple(D1), tuple(D1), tuple(D0)]
			self.Quad(Mat, *P, *N, (U0, 0.0), (U1, 0.0), (U1, L / TexSize), (U0, L / TexSize))
		if Caps:
			for End, Sign in ((A, -1.0), (B, 1.0)):
				Nn = tuple(W * Sign)
				for K in range(Segments):
					(D0, _), (D1, _) = Ring[K], Ring[K + 1]
					P0, P1 = tuple(End + D0 * Radius), tuple(End + D1 * Radius)
					self.Tri(Mat, tuple(End), P0, P1, Nn, Nn, Nn, (0.5, 0.5), (0.5 + D0[0] * 0.5, 0.5 + D0[1] * 0.5), (0.5 + D1[0] * 0.5, 0.5 + D1[1] * 0.5))

	def Save(self, Folder, Name, Content):
		# Folder: Content 기준 폴더 (예: Asset/DemoKits/HD2D/Harbor) — 텍스처는 그 폴더에서 Poly Haven까지 상대 경로
		OutDir = os.path.join(Content, *Folder.split("/"))
		os.makedirs(OutDir, exist_ok=True)
		Depth = len(Folder.split("/"))
		ToContent = "/".join([".."] * Depth)
		Blob = bytearray()
		Accessors, Views, Meshes, Prims = [], [], [], []
		Images, Textures, MaterialsOut = [], [], []
		ImageIndex = {}

		def Image_(Uri):
			if Uri not in ImageIndex:
				ImageIndex[Uri] = len(Images)
				Images.append({"uri": Uri})
				Textures.append({"source": ImageIndex[Uri], "sampler": 0})
			return ImageIndex[Uri]

		MatIndex = {}
		for MatName in self.Parts:
			Def = self.Materials[MatName]
			Pbr = {"baseColorFactor": [Def.Tint[0], Def.Tint[1], Def.Tint[2], Def.Alpha], "metallicFactor": Def.Metal, "roughnessFactor": Def.Rough}
			M = {"name": Def.Name, "pbrMetallicRoughness": Pbr, "doubleSided": Def.DoubleSided}
			if Def.Stem:
				# "묶음" → 묶음/묶음_diff_2k.jpg, "폴더/…/줄기" → 그 경로 그대로 (예: modular_fort_01/textures/modular_fort_01_wall)
				Base = f"{ToContent}/{PH}/{Def.Stem}" if "/" in Def.Stem else f"{ToContent}/{PH}/{Def.Stem}/{Def.Stem}"
				Pbr["baseColorTexture"] = {"index": Image_(f"{Base}_diff_2k.jpg")}
				Pbr["metallicRoughnessTexture"] = {"index": Image_(f"{Base}_arm_2k.jpg")}
				Pbr["metallicFactor"] = Def.Metal  # ARM의 B(금속) × 배율 (나무·돌은 0)
				M["normalTexture"] = {"index": Image_(f"{Base}_nor_gl_2k.jpg"), "scale": Def.NormalScale}
				M["occlusionTexture"] = {"index": Image_(f"{Base}_arm_2k.jpg")}
			elif Def.Texture:
				Pbr["baseColorTexture"] = {"index": Image_(Def.Texture)}
			if any(V > 0.0 for V in Def.Emissive):
				M["emissiveFactor"] = list(Def.Emissive)
			if Def.EmissiveTexture:
				M["emissiveTexture"] = {"index": Image_(Def.EmissiveTexture)}
			if Def.Blend:
				M["alphaMode"] = "BLEND"
			if Def.Mask:
				M["alphaMode"] = "MASK"
				M["alphaCutoff"] = 0.35
			MatIndex[MatName] = len(MaterialsOut)
			MaterialsOut.append(M)

		def Push(Data, Fmt, Count, Type, Target, Min=None, Max=None, Component=5126):
			while len(Blob) % 4:
				Blob.append(0)
			Offset = len(Blob)
			Blob.extend(Data)
			Views.append({"buffer": 0, "byteOffset": Offset, "byteLength": len(Data), "target": Target})
			A = {"bufferView": len(Views) - 1, "componentType": Component, "count": Count, "type": Type}
			if Min is not None:
				A["min"], A["max"] = Min, Max
			Accessors.append(A)
			return len(Accessors) - 1

		for MatName, (P, N, U, I) in self.Parts.items():
			GP = [_ToGltf(V) for V in P]
			GN = []
			for V in N:
				G = np.array(_NToGltf(V))
				GN.append(tuple(G / max(np.linalg.norm(G), 1e-9)))
			# 와인딩: 면 법선(정점 법선 평균)과 같은 쪽이 반시계가 되게
			Idx = list(I)
			for T in range(0, len(Idx), 3):
				A_, B_, C_ = (np.array(GP[Idx[T + K]]) for K in range(3))
				Nf = np.array(GN[Idx[T]]) + np.array(GN[Idx[T + 1]]) + np.array(GN[Idx[T + 2]])
				if np.dot(np.cross(B_ - A_, C_ - A_), Nf) < 0.0:
					Idx[T + 1], Idx[T + 2] = Idx[T + 2], Idx[T + 1]
			Arr = np.array(GP, dtype=np.float32)
			PosA = Push(Arr.tobytes(), None, len(GP), "VEC3", 34962, Arr.min(axis=0).tolist(), Arr.max(axis=0).tolist())
			NorA = Push(np.array(GN, dtype=np.float32).tobytes(), None, len(GN), "VEC3", 34962)
			UvA = Push(np.array(U, dtype=np.float32).tobytes(), None, len(U), "VEC2", 34962)
			IdxA = Push(np.array(Idx, dtype=np.uint32).tobytes(), None, len(Idx), "SCALAR", 34963, Component=5125)
			Prims.append({"attributes": {"POSITION": PosA, "NORMAL": NorA, "TEXCOORD_0": UvA}, "indices": IdxA, "material": MatIndex[MatName]})
		Meshes.append({"name": Name, "primitives": Prims})
		with open(os.path.join(OutDir, f"{Name}.bin"), "wb") as File:
			File.write(bytes(Blob))
		Doc = {"asset": {"version": "2.0", "generator": "ProjectE HD2DMeshKit"}, "scene": 0, "scenes": [{"nodes": [0]}],
			   "nodes": [{"name": Name, "mesh": 0}], "meshes": Meshes, "materials": MaterialsOut, "accessors": Accessors, "bufferViews": Views,
			   "buffers": [{"uri": f"{Name}.bin", "byteLength": len(Blob)}]}
		if Images:
			Doc["images"] = Images
			Doc["textures"] = Textures
			Doc["samplers"] = [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}]
		with open(os.path.join(OutDir, f"{Name}.gltf"), "w", encoding="utf-8", newline="\n") as File:
			json.dump(Doc, File, indent=1)
			File.write("\n")
		return f"{Folder}/{Name}.gltf"


def _Rotation(Pitch, Yaw, Roll):
	# FQuat::FromEuler 규약 (Yaw(Z) * Pitch(Y) * Roll(X)) 회전 행렬 — 열벡터 기준 (v' = R v)
	P, Y, R = math.radians(Pitch), math.radians(Yaw), math.radians(Roll)
	Cy, Sy = math.cos(Y), math.sin(Y)
	Cp, Sp = math.cos(P), math.sin(P)
	Cr, Sr = math.cos(R), math.sin(R)
	Rz = np.array([[Cy, -Sy, 0], [Sy, Cy, 0], [0, 0, 1]])
	Ry = np.array([[Cp, 0, -Sp], [0, 1, 0], [Sp, 0, Cp]])  # +Pitch = 기수 위 (+X가 +Z 쪽으로)
	Rx = np.array([[1, 0, 0], [0, Cr, -Sr], [0, Sr, Cr]])
	return Rz @ Ry @ Rx
