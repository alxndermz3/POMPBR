#include "Reflect.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <vector>

#include "Log.h"

namespace Pom::Reflect
{
namespace
{
	constexpr UINT SLOT_COUNT = 2;
	constexpr size_t SHADER_SETS_KEPT = 4;

	template <typename T>
	void SafeRelease(T*& object)
	{
		if (object)
		{
			object->Release();
			object = nullptr;
		}
	}

	std::atomic<const Shaders*> g_Shaders{nullptr};
	std::mutex g_ConstantsLock;
	Brdf::Constants g_Wanted;
	std::atomic<bool> g_ConstantsChanged{true};

	// Render thread only.
	bool g_InScene = false;
	bool g_Inside = false;  // What's called from here isn't the game's.
	ID3D11RenderTargetView* g_Target = nullptr;      // Only compared.
	ID3D11DepthStencilView* g_DepthTarget = nullptr;
	ID3D11RenderTargetView* g_Ignored = nullptr;     // A target that was too small: checked once.
	ID3D11Texture2D* g_SceneColor = nullptr;
	ID3D11Texture2D* g_SceneDepth = nullptr;

	ID3D11Texture2D* g_ColorCopy = nullptr;
	ID3D11Texture2D* g_DepthCopy = nullptr;
	ID3D11ShaderResourceView* g_ColorView = nullptr;
	ID3D11ShaderResourceView* g_DepthView = nullptr;
	ID3D11Buffer* g_Buffer = nullptr;
	bool g_HasMips = false;
	bool g_HaveFrame = false;
	bool g_CopiesFailed = false;
	UINT g_ScreenWidth = 0;
	unsigned g_CopiedFrames = 0;
	bool g_Logged = false;

	ID3D11Texture2D* TextureOf(ID3D11View* view)
	{
		ID3D11Resource* resource = nullptr;
		view->GetResource(&resource);
		ID3D11Texture2D* texture = nullptr;
		if (resource)
			resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
		SafeRelease(resource);
		return texture;
	}

	// The SRV format that reads the depth part of a depth texture, and the format a readable copy of it needs.
	bool DepthFormats(DXGI_FORMAT format, DXGI_FORMAT& copy, DXGI_FORMAT& read)
	{
		switch (format)
		{
		case DXGI_FORMAT_R32G8X24_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
			copy = DXGI_FORMAT_R32G8X24_TYPELESS;
			read = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
			return true;
		case DXGI_FORMAT_R32_TYPELESS:
		case DXGI_FORMAT_D32_FLOAT:
			copy = DXGI_FORMAT_R32_TYPELESS;
			read = DXGI_FORMAT_R32_FLOAT;
			return true;
		case DXGI_FORMAT_R24G8_TYPELESS:
		case DXGI_FORMAT_D24_UNORM_S8_UINT:
			copy = DXGI_FORMAT_R24G8_TYPELESS;
			read = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			return true;
		default:
			return false;
		}
	}

	void ReleaseCopies()
	{
		SafeRelease(g_ColorView);
		SafeRelease(g_DepthView);
		SafeRelease(g_ColorCopy);
		SafeRelease(g_DepthCopy);
		g_HaveFrame = false;
	}

	bool SameSize(ID3D11Texture2D* a, const D3D11_TEXTURE2D_DESC& b)
	{
		if (!a)
			return false;
		D3D11_TEXTURE2D_DESC d{};
		a->GetDesc(&d);
		return d.Width == b.Width && d.Height == b.Height;
	}

	bool MakeCopies(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& color, DXGI_FORMAT viewFormat, const D3D11_TEXTURE2D_DESC& depth)
	{
		ReleaseCopies();
		if (color.SampleDesc.Count > 1 || depth.SampleDesc.Count > 1)
		{
			Log::Write("screen reflections: the scene is multisampled, which they can't read: probes only");
			return false;
		}
		DXGI_FORMAT depthCopyFormat, depthReadFormat;
		if (!DepthFormats(depth.Format, depthCopyFormat, depthReadFormat))
		{
			Log::Write("screen reflections: depth format %d can't be read: probes only", int(depth.Format));
			return false;
		}

		UINT support = 0;
		g_HasMips = SUCCEEDED(device->CheckFormatSupport(viewFormat, &support)) && (support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN) && (support & D3D11_FORMAT_SUPPORT_RENDER_TARGET);

		D3D11_TEXTURE2D_DESC d = color;
		d.MipLevels = g_HasMips ? 0 : 1;
		d.ArraySize = 1;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.CPUAccessFlags = 0;
		d.BindFlags = D3D11_BIND_SHADER_RESOURCE | (g_HasMips ? D3D11_BIND_RENDER_TARGET : 0);
		d.MiscFlags = g_HasMips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
		D3D11_SHADER_RESOURCE_VIEW_DESC view{};
		view.Format = viewFormat;
		view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		view.Texture2D.MipLevels = UINT(-1);
		HRESULT hr = device->CreateTexture2D(&d, nullptr, &g_ColorCopy);
		if (SUCCEEDED(hr))
			hr = device->CreateShaderResourceView(g_ColorCopy, &view, &g_ColorView);

		if (SUCCEEDED(hr))
		{
			d = depth;
			d.Format = depthCopyFormat;
			d.MipLevels = 1;
			d.ArraySize = 1;
			d.Usage = D3D11_USAGE_DEFAULT;
			d.CPUAccessFlags = 0;
			d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			d.MiscFlags = 0;
			view.Format = depthReadFormat;
			view.Texture2D.MipLevels = 1;
			hr = device->CreateTexture2D(&d, nullptr, &g_DepthCopy);
			if (SUCCEEDED(hr))
				hr = device->CreateShaderResourceView(g_DepthCopy, &view, &g_DepthView);
		}
		if (FAILED(hr))
		{
			Log::Write("screen reflections: couldn't make the copies: 0x%08X: probes only", unsigned(hr));
			ReleaseCopies();
			return false;
		}
		Log::Write("screen reflections: scene %ux%u, color format %d (view %d), depth format %d, mips %d", color.Width, color.Height, int(color.Format), int(viewFormat), int(depth.Format), g_HasMips);
		return true;
	}

	bool EnsureBuffer(ID3D11DeviceContext* context)
	{
		if (g_Buffer)
			return true;
		ID3D11Device* device = nullptr;
		context->GetDevice(&device);
		if (!device)
			return false;
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = (sizeof(Brdf::Constants) + 15) & ~15u;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		const HRESULT hr = device->CreateBuffer(&desc, nullptr, &g_Buffer);
		device->Release();
		if (FAILED(hr))
			Log::Write("couldn't make the PBR constants: 0x%08X: the game's own values stand", unsigned(hr));
		g_ConstantsChanged = true;
		return SUCCEEDED(hr);
	}

	void Upload(ID3D11DeviceContext* context, UINT width, UINT height, bool canReflect)
	{
		Brdf::Constants constants;
		{
			std::lock_guard<std::mutex> lock(g_ConstantsLock);
			constants = g_Wanted;
		}
		constants.screenWidth = float(width);
		constants.screenHeight = float(height);
		if (!canReflect)
			constants.screenAmount = 0;
		context->UpdateSubresource(g_Buffer, 0, nullptr, &constants, 0, 0);
	}

	void OpenScene(ID3D11DeviceContext* context)
	{
		ID3D11RenderTargetView* target = nullptr;
		ID3D11DepthStencilView* depthTarget = nullptr;
		context->OMGetRenderTargets(1, &target, &depthTarget);
		const auto finish = [&]
		{
			SafeRelease(target);
			SafeRelease(depthTarget);
		};
		if (!target || !depthTarget || target == g_Ignored)
			return finish();

		ID3D11Texture2D* color = TextureOf(target);
		ID3D11Texture2D* depth = TextureOf(depthTarget);
		if (!color || !depth)
		{
			SafeRelease(color);
			SafeRelease(depth);
			return finish();
		}
		D3D11_TEXTURE2D_DESC colorDesc{}, depthDesc{};
		color->GetDesc(&colorDesc);
		depth->GetDesc(&depthDesc);

		// An extra camera: its lit shaders mustn't look at the scene's frame, which is a different view.
		if (colorDesc.Width * 2 < g_ScreenWidth)
		{
			g_Ignored = target;
			ID3D11ShaderResourceView* nothing[SLOT_COUNT] = {};
			context->PSSetShaderResources(Brdf::SCENE_COLOR_SLOT, SLOT_COUNT, nothing);
			SafeRelease(color);
			SafeRelease(depth);
			return finish();
		}

		ID3D11Device* device = nullptr;
		context->GetDevice(&device);
		// A vid_restart makes a new device, and everything of the old one is no use.
		if (g_Buffer)
		{
			ID3D11Device* owner = nullptr;
			g_Buffer->GetDevice(&owner);
			if (owner != device)
				Release();
			SafeRelease(owner);
		}
		bool remade = false;
		if (!g_CopiesFailed && (!SameSize(g_ColorCopy, colorDesc) || !SameSize(g_DepthCopy, depthDesc)))
		{
			D3D11_RENDER_TARGET_VIEW_DESC targetDesc{};
			target->GetDesc(&targetDesc);
			g_CopiesFailed = !MakeCopies(device, colorDesc, targetDesc.Format, depthDesc);
			remade = true;
		}
		SafeRelease(device);

		if (EnsureBuffer(context))
		{
			if (remade || g_ConstantsChanged.exchange(false))
				Upload(context, colorDesc.Width, colorDesc.Height, g_ColorCopy != nullptr);
			context->PSSetConstantBuffers(Brdf::CONSTANT_SLOT, 1, &g_Buffer);
		}
		if (g_ColorCopy)
		{
			ID3D11ShaderResourceView* views[SLOT_COUNT] = {g_HaveFrame ? g_ColorView : nullptr, g_HaveFrame ? g_DepthView : nullptr};
			context->PSSetShaderResources(Brdf::SCENE_COLOR_SLOT, SLOT_COUNT, views);
		}

		g_InScene = true;
		g_Target = target;
		g_DepthTarget = depthTarget;
		SafeRelease(g_SceneColor);
		SafeRelease(g_SceneDepth);
		g_SceneColor = color;
		g_SceneDepth = depth;
		finish();  // The textures keep them alive, and these two are only compared.
	}

	void CloseScene(ID3D11DeviceContext* context)
	{
		g_InScene = false;
		if (g_ColorCopy && g_SceneColor && g_SceneDepth)
		{
			ID3D11ShaderResourceView* nothing[SLOT_COUNT] = {};
			context->PSSetShaderResources(Brdf::SCENE_COLOR_SLOT, SLOT_COUNT, nothing);
			context->CopySubresourceRegion(g_ColorCopy, 0, 0, 0, 0, g_SceneColor, 0, nullptr);
			context->CopyResource(g_DepthCopy, g_SceneDepth);
			if (g_HasMips)
				context->GenerateMips(g_ColorView);
			g_HaveFrame = true;
			if (!g_Logged && ++g_CopiedFrames == 60)
			{
				g_Logged = true;
				Log::Write("screen reflections: running");
			}
		}
		SafeRelease(g_SceneColor);
		SafeRelease(g_SceneDepth);
		g_Target = nullptr;
		g_DepthTarget = nullptr;
	}
}

void SetShaders(const Shaders* shaders)
{
	static std::vector<std::unique_ptr<const Shaders>> kept;
	const Shaders* old = g_Shaders.exchange(shaders, std::memory_order_acq_rel);
	if (old)
		kept.emplace_back(old);
	while (kept.size() > SHADER_SETS_KEPT)
		kept.erase(kept.begin());
}

void SetConstants(const Brdf::Constants& constants)
{
	std::lock_guard<std::mutex> lock(g_ConstantsLock);
	g_Wanted = constants;
	g_ConstantsChanged = true;
}

void BeforeShader(ID3D11DeviceContext* context, ID3D11PixelShader* shader)
{
	if (g_InScene || g_Inside || !shader)
		return;
	const Shaders* shaders = g_Shaders.load(std::memory_order_acquire);
	if (!shaders || !shaders->count(shader))
		return;
	g_Inside = true;
	OpenScene(context);
	g_Inside = false;
}

void BeforeTargets(ID3D11DeviceContext* context, UINT count, ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* depth)
{
	g_Ignored = nullptr;
	if (!g_InScene || g_Inside)
		return;
	if (count && targets && targets[0] == g_Target && depth == g_DepthTarget)
		return;
	g_Inside = true;
	CloseScene(context);
	g_Inside = false;
}

void EndFrame(ID3D11DeviceContext* context, UINT screenWidth, UINT screenHeight)
{
	g_ScreenWidth = screenWidth;
	(void)screenHeight;
	if (!g_InScene)
		return;
	// Nothing changed the targets: the game drew straight into what's about to be shown.
	g_Inside = true;
	CloseScene(context);
	g_Inside = false;
}

void Release()
{
	SafeRelease(g_SceneColor);
	SafeRelease(g_SceneDepth);
	g_InScene = false;
	g_Target = nullptr;
	g_DepthTarget = nullptr;
	g_Ignored = nullptr;
	g_CopiesFailed = false;
	ReleaseCopies();
	SafeRelease(g_Buffer);
}
}
