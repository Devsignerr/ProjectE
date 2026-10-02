#include "RHI/D3D12/D3D12PipelineRecipe.h"

#include <cstring>
#include <format>
#include <type_traits>
#include <unordered_set>

namespace PipelineCache
{
	namespace
	{
		class FWriter
		{
		public:
			template <typename T>
			void Put(T Value)
			{
				static_assert(std::is_trivially_copyable_v<T>);
				const size_t Offset = Bytes.size();
				Bytes.resize(Offset + sizeof(T));
				std::memcpy(Bytes.data() + Offset, &Value, sizeof(T));
			}
			void PutBytes(const void* Data, size_t Size)
			{
				Put(static_cast<uint32>(Size));
				const size_t Offset = Bytes.size();
				Bytes.resize(Offset + Size);
				if (Size > 0)
				{
					std::memcpy(Bytes.data() + Offset, Data, Size);
				}
			}
			std::vector<uint8> Bytes;
		};

		class FReader
		{
		public:
			explicit FReader(std::span<const uint8> InBytes) : Bytes(InBytes) {}
			template <typename T>
			bool Get(T& Out)
			{
				static_assert(std::is_trivially_copyable_v<T>);
				if (Offset + sizeof(T) > Bytes.size())
				{
					return false;
				}
				std::memcpy(&Out, Bytes.data() + Offset, sizeof(T));
				Offset += sizeof(T);
				return true;
			}
			bool GetBytes(std::vector<uint8>& Out)
			{
				uint32 Size = 0;
				if (!Get(Size) || Offset + Size > Bytes.size())
				{
					return false;
				}
				Out.assign(Bytes.begin() + static_cast<std::ptrdiff_t>(Offset), Bytes.begin() + static_cast<std::ptrdiff_t>(Offset + Size));
				Offset += Size;
				return true;
			}
			bool IsEnd() const { return Offset == Bytes.size(); }

		private:
			std::span<const uint8> Bytes;
			size_t                 Offset = 0;
		};

		FShaderRef MakeShaderRef(const D3D12_SHADER_BYTECODE& Code)
		{
			FShaderRef Ref;
			if (Code.pShaderBytecode != nullptr && Code.BytecodeLength > 0)
			{
				Ref.Hash = HashBytes(Code.pShaderBytecode, Code.BytecodeLength);
				Ref.Size = static_cast<uint32>(Code.BytecodeLength);
			}
			return Ref;
		}

		void PutStencilOp(FWriter& W, const D3D12_DEPTH_STENCILOP_DESC& Op)
		{
			W.Put(static_cast<uint32>(Op.StencilFailOp));
			W.Put(static_cast<uint32>(Op.StencilDepthFailOp));
			W.Put(static_cast<uint32>(Op.StencilPassOp));
			W.Put(static_cast<uint32>(Op.StencilFunc));
		}
		bool GetStencilOp(FReader& R, D3D12_DEPTH_STENCILOP_DESC& Op)
		{
			uint32 V[4] = {};
			for (uint32& Value : V)
			{
				if (!R.Get(Value))
				{
					return false;
				}
			}
			Op.StencilFailOp      = static_cast<D3D12_STENCIL_OP>(V[0]);
			Op.StencilDepthFailOp = static_cast<D3D12_STENCIL_OP>(V[1]);
			Op.StencilPassOp      = static_cast<D3D12_STENCIL_OP>(V[2]);
			Op.StencilFunc        = static_cast<D3D12_COMPARISON_FUNC>(V[3]);
			return true;
		}
	} // namespace

	uint64 HashBytes(const void* Data, size_t Size, uint64 Seed)
	{
		uint64       Hash  = Seed;
		const uint8* Bytes = static_cast<const uint8*>(Data);
		for (size_t Index = 0; Index < Size; ++Index)
		{
			Hash ^= Bytes[Index];
			Hash *= 0x100000001b3ull;
		}
		return Hash;
	}

	bool IsCacheable(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& Desc)
	{
		return Desc.pRootSignature != nullptr && Desc.VS.pShaderBytecode != nullptr && Desc.HS.pShaderBytecode == nullptr &&
		       Desc.DS.pShaderBytecode == nullptr && Desc.GS.pShaderBytecode == nullptr && Desc.StreamOutput.NumEntries == 0 &&
		       Desc.CachedPSO.pCachedBlob == nullptr && Desc.NumRenderTargets <= 8;
	}

	bool IsCacheable(const D3D12_COMPUTE_PIPELINE_STATE_DESC& Desc)
	{
		return Desc.pRootSignature != nullptr && Desc.CS.pShaderBytecode != nullptr && Desc.CachedPSO.pCachedBlob == nullptr;
	}

	FRecipe FromDesc(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& Desc, uint64 RootSignatureHash)
	{
		FRecipe Recipe;
		Recipe.Type              = EPipelineType::Graphics;
		Recipe.RootSignatureHash = RootSignatureHash;
		Recipe.Shaders[0]        = MakeShaderRef(Desc.VS);
		Recipe.Shaders[1]        = MakeShaderRef(Desc.PS);
		Recipe.Blend             = Desc.BlendState;
		Recipe.SampleMask        = Desc.SampleMask;
		Recipe.Rasterizer        = Desc.RasterizerState;
		Recipe.DepthStencil      = Desc.DepthStencilState;
		for (uint32 Index = 0; Index < Desc.InputLayout.NumElements; ++Index)
		{
			const D3D12_INPUT_ELEMENT_DESC& Element = Desc.InputLayout.pInputElementDescs[Index];
			Recipe.InputLayout.push_back({ Element.SemanticName != nullptr ? Element.SemanticName : "", Element.SemanticIndex, Element.Format,
			                               Element.InputSlot, Element.AlignedByteOffset, Element.InputSlotClass, Element.InstanceDataStepRate });
		}
		Recipe.StripCut         = Desc.IBStripCutValue;
		Recipe.TopologyType     = Desc.PrimitiveTopologyType;
		Recipe.NumRenderTargets = Desc.NumRenderTargets;
		for (uint32 Index = 0; Index < 8; ++Index)
		{
			Recipe.RtvFormats[Index] = Index < Desc.NumRenderTargets ? Desc.RTVFormats[Index] : DXGI_FORMAT_UNKNOWN;
		}
		Recipe.DsvFormat  = Desc.DSVFormat;
		Recipe.SampleDesc = Desc.SampleDesc;
		Recipe.NodeMask   = Desc.NodeMask;
		Recipe.Flags      = Desc.Flags;
		return Recipe;
	}

	FRecipe FromDesc(const D3D12_COMPUTE_PIPELINE_STATE_DESC& Desc, uint64 RootSignatureHash)
	{
		FRecipe Recipe;
		Recipe.Type              = EPipelineType::Compute;
		Recipe.RootSignatureHash = RootSignatureHash;
		Recipe.Shaders[0]        = MakeShaderRef(Desc.CS);
		Recipe.NodeMask          = Desc.NodeMask;
		Recipe.Flags             = Desc.Flags;
		return Recipe;
	}

	std::vector<uint8> Serialize(const FRecipe& Recipe)
	{
		FWriter W;
		W.Put(static_cast<uint8>(Recipe.Type));
		W.Put(Recipe.RootSignatureHash);
		for (const FShaderRef& Shader : Recipe.Shaders)
		{
			W.Put(Shader.Hash);
			W.Put(Shader.Size);
		}
		W.Put(Recipe.NodeMask);
		W.Put(static_cast<uint32>(Recipe.Flags));
		if (Recipe.Type == EPipelineType::Compute)
		{
			return std::move(W.Bytes);
		}
		W.Put(static_cast<uint32>(Recipe.Blend.AlphaToCoverageEnable));
		W.Put(static_cast<uint32>(Recipe.Blend.IndependentBlendEnable));
		for (const D3D12_RENDER_TARGET_BLEND_DESC& Target : Recipe.Blend.RenderTarget)
		{
			W.Put(static_cast<uint32>(Target.BlendEnable));
			W.Put(static_cast<uint32>(Target.LogicOpEnable));
			W.Put(static_cast<uint32>(Target.SrcBlend));
			W.Put(static_cast<uint32>(Target.DestBlend));
			W.Put(static_cast<uint32>(Target.BlendOp));
			W.Put(static_cast<uint32>(Target.SrcBlendAlpha));
			W.Put(static_cast<uint32>(Target.DestBlendAlpha));
			W.Put(static_cast<uint32>(Target.BlendOpAlpha));
			W.Put(static_cast<uint32>(Target.LogicOp));
			W.Put(static_cast<uint32>(Target.RenderTargetWriteMask));
		}
		W.Put(Recipe.SampleMask);
		const D3D12_RASTERIZER_DESC& R = Recipe.Rasterizer;
		W.Put(static_cast<uint32>(R.FillMode));
		W.Put(static_cast<uint32>(R.CullMode));
		W.Put(static_cast<uint32>(R.FrontCounterClockwise));
		W.Put(static_cast<int32>(R.DepthBias));
		W.Put(R.DepthBiasClamp);
		W.Put(R.SlopeScaledDepthBias);
		W.Put(static_cast<uint32>(R.DepthClipEnable));
		W.Put(static_cast<uint32>(R.MultisampleEnable));
		W.Put(static_cast<uint32>(R.AntialiasedLineEnable));
		W.Put(static_cast<uint32>(R.ForcedSampleCount));
		W.Put(static_cast<uint32>(R.ConservativeRaster));
		const D3D12_DEPTH_STENCIL_DESC& D = Recipe.DepthStencil;
		W.Put(static_cast<uint32>(D.DepthEnable));
		W.Put(static_cast<uint32>(D.DepthWriteMask));
		W.Put(static_cast<uint32>(D.DepthFunc));
		W.Put(static_cast<uint32>(D.StencilEnable));
		W.Put(static_cast<uint32>(D.StencilReadMask));
		W.Put(static_cast<uint32>(D.StencilWriteMask));
		PutStencilOp(W, D.FrontFace);
		PutStencilOp(W, D.BackFace);
		W.Put(static_cast<uint32>(Recipe.InputLayout.size()));
		for (const FInputElement& Element : Recipe.InputLayout)
		{
			W.PutBytes(Element.SemanticName.data(), Element.SemanticName.size());
			W.Put(Element.SemanticIndex);
			W.Put(static_cast<uint32>(Element.Format));
			W.Put(Element.InputSlot);
			W.Put(Element.AlignedByteOffset);
			W.Put(static_cast<uint32>(Element.Classification));
			W.Put(Element.InstanceDataStepRate);
		}
		W.Put(static_cast<uint32>(Recipe.StripCut));
		W.Put(static_cast<uint32>(Recipe.TopologyType));
		W.Put(Recipe.NumRenderTargets);
		for (const DXGI_FORMAT Format : Recipe.RtvFormats)
		{
			W.Put(static_cast<uint32>(Format));
		}
		W.Put(static_cast<uint32>(Recipe.DsvFormat));
		W.Put(static_cast<uint32>(Recipe.SampleDesc.Count));
		W.Put(static_cast<uint32>(Recipe.SampleDesc.Quality));
		return std::move(W.Bytes);
	}

	bool Deserialize(std::span<const uint8> Bytes, FRecipe& Out)
	{
		FReader R(Bytes);
		Out        = FRecipe{};
		uint8 Type = 0;
		uint32 U   = 0;
		const auto GetU = [&](auto& Field) {
			if (!R.Get(U))
			{
				return false;
			}
			Field = static_cast<std::remove_reference_t<decltype(Field)>>(U);
			return true;
		};
		if (!R.Get(Type) || Type > 1 || !R.Get(Out.RootSignatureHash))
		{
			return false;
		}
		Out.Type = static_cast<EPipelineType>(Type);
		for (FShaderRef& Shader : Out.Shaders)
		{
			if (!R.Get(Shader.Hash) || !R.Get(Shader.Size))
			{
				return false;
			}
		}
		if (!R.Get(Out.NodeMask) || !GetU(Out.Flags))
		{
			return false;
		}
		if (Out.Type == EPipelineType::Compute)
		{
			return R.IsEnd();
		}
		bool bOk = GetU(Out.Blend.AlphaToCoverageEnable) && GetU(Out.Blend.IndependentBlendEnable);
		for (D3D12_RENDER_TARGET_BLEND_DESC& T : Out.Blend.RenderTarget)
		{
			bOk = bOk && GetU(T.BlendEnable) && GetU(T.LogicOpEnable) && GetU(T.SrcBlend) && GetU(T.DestBlend) && GetU(T.BlendOp) && GetU(T.SrcBlendAlpha) &&
			      GetU(T.DestBlendAlpha) && GetU(T.BlendOpAlpha) && GetU(T.LogicOp) && GetU(T.RenderTargetWriteMask);
		}
		D3D12_RASTERIZER_DESC& Rs = Out.Rasterizer;
		int32                  Bias = 0;
		bOk = bOk && R.Get(Out.SampleMask) && GetU(Rs.FillMode) && GetU(Rs.CullMode) && GetU(Rs.FrontCounterClockwise) && R.Get(Bias) &&
		      R.Get(Rs.DepthBiasClamp) && R.Get(Rs.SlopeScaledDepthBias) && GetU(Rs.DepthClipEnable) && GetU(Rs.MultisampleEnable) &&
		      GetU(Rs.AntialiasedLineEnable) && GetU(Rs.ForcedSampleCount) && GetU(Rs.ConservativeRaster);
		Rs.DepthBias = Bias;
		D3D12_DEPTH_STENCIL_DESC& Ds = Out.DepthStencil;
		bOk = bOk && GetU(Ds.DepthEnable) && GetU(Ds.DepthWriteMask) && GetU(Ds.DepthFunc) && GetU(Ds.StencilEnable) && GetU(Ds.StencilReadMask) &&
		      GetU(Ds.StencilWriteMask) && GetStencilOp(R, Ds.FrontFace) && GetStencilOp(R, Ds.BackFace);
		uint32 ElementCount = 0;
		if (!bOk || !R.Get(ElementCount) || ElementCount > 64)
		{
			return false;
		}
		for (uint32 Index = 0; Index < ElementCount; ++Index)
		{
			FInputElement      Element;
			std::vector<uint8> Name;
			if (!R.GetBytes(Name) || !R.Get(Element.SemanticIndex) || !GetU(Element.Format) || !R.Get(Element.InputSlot) ||
			    !R.Get(Element.AlignedByteOffset) || !GetU(Element.Classification) || !R.Get(Element.InstanceDataStepRate))
			{
				return false;
			}
			Element.SemanticName.assign(Name.begin(), Name.end());
			Out.InputLayout.push_back(std::move(Element));
		}
		bOk = GetU(Out.StripCut) && GetU(Out.TopologyType) && R.Get(Out.NumRenderTargets) && Out.NumRenderTargets <= 8;
		for (DXGI_FORMAT& Format : Out.RtvFormats)
		{
			bOk = bOk && GetU(Format);
		}
		bOk = bOk && GetU(Out.DsvFormat) && GetU(Out.SampleDesc.Count) && GetU(Out.SampleDesc.Quality);
		return bOk && R.IsEnd();
	}

	uint64 ComputeKey(const FRecipe& Recipe)
	{
		const std::vector<uint8> Bytes = Serialize(Recipe);
		return HashBytes(Bytes.data(), Bytes.size());
	}

	std::wstring MakeLibraryName(uint64 Key)
	{
		return std::format(L"PSO_{:016X}", Key);
	}

	D3D12_GRAPHICS_PIPELINE_STATE_DESC ToGraphicsDesc(const FRecipe& Recipe, ID3D12RootSignature* RootSignature, D3D12_SHADER_BYTECODE Vertex,
	                                                  D3D12_SHADER_BYTECODE Pixel, std::vector<D3D12_INPUT_ELEMENT_DESC>& OutElements)
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC Desc{};
		Desc.pRootSignature    = RootSignature;
		Desc.VS                = Vertex;
		Desc.PS                = Pixel;
		Desc.BlendState        = Recipe.Blend;
		Desc.SampleMask        = Recipe.SampleMask;
		Desc.RasterizerState   = Recipe.Rasterizer;
		Desc.DepthStencilState = Recipe.DepthStencil;
		OutElements.clear();
		for (const FInputElement& Element : Recipe.InputLayout)
		{
			OutElements.push_back({ Element.SemanticName.c_str(), Element.SemanticIndex, Element.Format, Element.InputSlot, Element.AlignedByteOffset,
			                        Element.Classification, Element.InstanceDataStepRate });
		}
		Desc.InputLayout.pInputElementDescs = OutElements.empty() ? nullptr : OutElements.data();
		Desc.InputLayout.NumElements        = static_cast<UINT>(OutElements.size());
		Desc.IBStripCutValue                = Recipe.StripCut;
		Desc.PrimitiveTopologyType          = Recipe.TopologyType;
		Desc.NumRenderTargets               = Recipe.NumRenderTargets;
		for (uint32 Index = 0; Index < 8; ++Index)
		{
			Desc.RTVFormats[Index] = Recipe.RtvFormats[Index];
		}
		Desc.DSVFormat  = Recipe.DsvFormat;
		Desc.SampleDesc = Recipe.SampleDesc;
		Desc.NodeMask   = Recipe.NodeMask;
		Desc.Flags      = Recipe.Flags;
		return Desc;
	}

	D3D12_COMPUTE_PIPELINE_STATE_DESC ToComputeDesc(const FRecipe& Recipe, ID3D12RootSignature* RootSignature, D3D12_SHADER_BYTECODE Compute)
	{
		D3D12_COMPUTE_PIPELINE_STATE_DESC Desc{};
		Desc.pRootSignature = RootSignature;
		Desc.CS             = Compute;
		Desc.NodeMask       = Recipe.NodeMask;
		Desc.Flags          = Recipe.Flags;
		return Desc;
	}

	std::vector<uint8> FRecipeFile::Write() const
	{
		FWriter W;
		W.Put(Magic);
		W.Put(Version);
		W.Put(RunCounter);
		W.Put(static_cast<uint32>(Blobs.size()));
		for (const auto& [Hash, Bytes] : Blobs)
		{
			W.Put(Hash);
			W.PutBytes(Bytes.data(), Bytes.size());
		}
		W.Put(static_cast<uint32>(Recipes.size()));
		for (const auto& [Key, Entry] : Recipes)
		{
			const std::vector<uint8> Bytes = Serialize(Entry.Recipe);
			W.Put(Entry.LastUsedRun);
			W.PutBytes(Bytes.data(), Bytes.size());
		}
		return std::move(W.Bytes);
	}

	bool FRecipeFile::Read(std::span<const uint8> Bytes)
	{
		Blobs.clear();
		Recipes.clear();
		RunCounter = 0;
		FReader R(Bytes);
		uint32  FileMagic   = 0;
		uint32  FileVersion = 0;
		uint32  BlobCount   = 0;
		if (!R.Get(FileMagic) || FileMagic != Magic || !R.Get(FileVersion) || FileVersion != Version || !R.Get(RunCounter) || !R.Get(BlobCount))
		{
			RunCounter = 0;
			return false;
		}
		for (uint32 Index = 0; Index < BlobCount; ++Index)
		{
			uint64             Hash = 0;
			std::vector<uint8> Blob;
			if (!R.Get(Hash) || !R.GetBytes(Blob) || HashBytes(Blob.data(), Blob.size()) != Hash) // 손상 확인
			{
				*this = FRecipeFile{};
				return false;
			}
			Blobs.emplace(Hash, std::move(Blob));
		}
		uint32 RecipeCount = 0;
		if (!R.Get(RecipeCount))
		{
			*this = FRecipeFile{};
			return false;
		}
		for (uint32 Index = 0; Index < RecipeCount; ++Index)
		{
			FRecipeEntry       Entry;
			std::vector<uint8> RecipeBytes;
			if (!R.Get(Entry.LastUsedRun) || !R.GetBytes(RecipeBytes) || !Deserialize(RecipeBytes, Entry.Recipe))
			{
				*this = FRecipeFile{};
				return false;
			}
			Recipes.emplace(HashBytes(RecipeBytes.data(), RecipeBytes.size()), std::move(Entry));
		}
		if (!R.IsEnd())
		{
			*this = FRecipeFile{};
			return false;
		}
		return true;
	}

	bool FRecipeFile::HasBlobs(const FRecipe& Recipe) const
	{
		if (!Blobs.contains(Recipe.RootSignatureHash))
		{
			return false;
		}
		for (const FShaderRef& Shader : Recipe.Shaders)
		{
			if (Shader.Size > 0)
			{
				const auto It = Blobs.find(Shader.Hash);
				if (It == Blobs.end() || It->second.size() != Shader.Size)
				{
					return false;
				}
			}
		}
		return true;
	}

	uint32 FRecipeFile::Prune(uint32 CurrentRun, uint32 MaxAgeRuns)
	{
		uint32 Removed = 0;
		for (auto It = Recipes.begin(); It != Recipes.end();)
		{
			if (CurrentRun > It->second.LastUsedRun && CurrentRun - It->second.LastUsedRun > MaxAgeRuns)
			{
				It = Recipes.erase(It);
				++Removed;
			}
			else
			{
				++It;
			}
		}
		std::unordered_set<uint64> Used;
		for (const auto& [Key, Entry] : Recipes)
		{
			Used.insert(Entry.Recipe.RootSignatureHash);
			for (const FShaderRef& Shader : Entry.Recipe.Shaders)
			{
				if (Shader.Size > 0)
				{
					Used.insert(Shader.Hash);
				}
			}
		}
		std::erase_if(Blobs, [&](const auto& Pair) { return !Used.contains(Pair.first); });
		return Removed;
	}

	void FRecipeFile::Merge(const FRecipeFile& Other)
	{
		for (const auto& [Hash, Bytes] : Other.Blobs)
		{
			Blobs.try_emplace(Hash, Bytes);
		}
		for (const auto& [Key, Entry] : Other.Recipes)
		{
			auto [It, bInserted] = Recipes.try_emplace(Key, Entry);
			if (!bInserted && Entry.LastUsedRun > It->second.LastUsedRun)
			{
				It->second.LastUsedRun = Entry.LastUsedRun;
			}
		}
	}

	bool IsLibraryCompatible(const FLibraryHeader& Stored, const FLibraryHeader& Current)
	{
		return Stored.FileMagic == FLibraryHeader::Magic && Stored.FileVersion == FLibraryHeader::Version && Stored.VendorId == Current.VendorId &&
		       Stored.DeviceId == Current.DeviceId && Stored.SubSysId == Current.SubSysId && Stored.Revision == Current.Revision &&
		       Stored.DriverVersion == Current.DriverVersion && Stored.DataSize > 0;
	}
} // namespace PipelineCache
