#include "Picker.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>

#include "Highlight.h"

namespace Pom::Picker
{
namespace
{
	constexpr UINT MAX_PICK_DRAWS = 32768;

	template <typename T>
	void SafeRelease(T*& object)
	{
		if (object)
		{
			object->Release();
			object = nullptr;
		}
	}

	// Whether the draw is tested against the scene's depth. Every frame ends with fullscreen passes that cover each pixel
	// after the scene, and those aren't: this is what keeps them out of it.
	bool TestsDepth(ID3D11DeviceContext* context, bool mustWriteIt)
	{
		ID3D11DepthStencilView* view = nullptr;
		context->OMGetRenderTargets(0, nullptr, &view);
		if (!view)
			return false;
		view->Release();
		ID3D11DepthStencilState* state = nullptr;
		UINT reference = 0;
		context->OMGetDepthStencilState(&state, &reference);
		if (!state)
			return true;  // The default: on, and writing.
		D3D11_DEPTH_STENCIL_DESC desc{};
		state->GetDesc(&desc);
		state->Release();
		return desc.DepthEnable && (!mustWriteIt || desc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL);
	}
}

bool Core::Init(ID3D11Device* device, std::string& error)
{
	device_ = device;
	device_->AddRef();

	D3D11_BLEND_DESC blend{};
	blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
	blend.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
	blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	blend.RenderTarget[0].RenderTargetWriteMask = 0;
	HRESULT hr = device_->CreateBlendState(&blend, &noColor_);

	blend.RenderTarget[0].BlendEnable = TRUE;
	blend.RenderTarget[0].SrcBlend = D3D11_BLEND_BLEND_FACTOR;
	blend.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
	blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
	blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
	blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
	if (SUCCEEDED(hr))
		hr = device_->CreateBlendState(&blend, &additive_);
	if (SUCCEEDED(hr))
		hr = device_->CreatePixelShader(g_Highlight, sizeof(g_Highlight), nullptr, &flat_);
	if (FAILED(hr))
	{
		error = "the picker's GPU objects weren't created";
		return false;
	}
	return true;
}

void Core::Release()
{
	ClearRecords();
	for (ID3D11Query* query : queries_)
		query->Release();
	queries_.clear();
	for (auto& state : scissored_)
	{
		if (state.first)
			state.first->Release();
		state.second->Release();
	}
	scissored_.clear();
	SafeRelease(highlight_);
	SafeRelease(flat_);
	SafeRelease(additive_);
	SafeRelease(noColor_);
	SafeRelease(device_);
	table_.reset();
}

void Core::SetTable(std::shared_ptr<const Table> table)
{
	table_ = std::move(table);
}

void Core::SetScreenSize(UINT width, UINT height)
{
	screenWidth_ = width;
	screenHeight_ = height;
}

void Core::SetHighlight(ID3D11ShaderResourceView* normal)
{
	if (normal)
		normal->AddRef();
	SafeRelease(highlight_);
	highlight_ = normal;
}

void Core::SetHighlightColor(float r, float g, float b)
{
	highlightColor_[0] = r;
	highlightColor_[1] = g;
	highlightColor_[2] = b;
}

void Core::RequestPick(float u, float v)
{
	pickU_ = u;
	pickV_ = v;
	if (pick_ == Pick::Idle)
		pick_ = Pick::Requested;
}

void Core::ClearRecords()
{
	for (Record& record : records_)
	{
		if (record.color)
			record.color->Release();
		if (record.normal)
			record.normal->Release();
	}
	records_.clear();
}

bool Core::EndFrame(ID3D11DeviceContext* context, Hit& out)
{
	if (pick_ == Pick::Requested)
	{
		// The frame about to be drawn is a whole one.
		ClearRecords();
		report_ = Report();
		pick_ = Pick::Drawing;
		picking_ = true;
		return false;
	}
	if (pick_ == Pick::Drawing)
	{
		picking_ = false;
		pick_ = Pick::Waiting;
		return false;
	}
	if (pick_ != Pick::Waiting)
		return false;

	out = Hit();
	if (!records_.empty())
	{
		// Draws finish in order, so the last one being ready means they all are.
		UINT64 samples = 0;
		if (context->GetData(queries_[records_.size() - 1], &samples, sizeof(samples), 0) == S_FALSE)
			return false;
		for (size_t i = records_.size(); i-- > 0;)
		{
			report_.known += records_[i].material != 0;
			samples = 0;
			if (context->GetData(queries_[i], &samples, sizeof(samples), 0) != S_OK || !samples)
				continue;
			if (report_.reached++)
				continue;
			out.material = records_[i].material;
			out.color = records_[i].color;
			out.normal = records_[i].normal;
			if (out.color)
				out.color->AddRef();
			if (out.normal)
				out.normal->AddRef();
		}
	}
	report_.solid = unsigned(records_.size());
	report_.tableSize = table_ ? unsigned(table_->surfaces.size()) : 0;
	ClearRecords();
	pick_ = Pick::Idle;
	return true;
}

bool Core::ReadBindings(ID3D11DeviceContext* context, Bindings& bound)
{
	// Depth passes have no color target. Neither they nor anything but the view proper is of interest.
	ID3D11RenderTargetView* target = nullptr;
	context->OMGetRenderTargets(1, &target, nullptr);
	if (!target)
		return false;
	target->Release();

	UINT viewports = 1;
	context->RSGetViewports(&viewports, &bound.viewport);
	if (!viewports || bound.viewport.Width < screenWidth_ * 0.5f || bound.viewport.Height < screenHeight_ * 0.5f)
		return false;

	context->PSGetShader(&bound.shader, nullptr, nullptr);
	if (!bound.shader)
		return false;
	ID3D11ShaderResourceView* views[2] = {};
	context->PSGetShaderResources(0, 2, views);
	bound.color = views[0];
	bound.normal = views[1];
	return true;
}

void Core::ReleaseBindings(Bindings& bound)
{
	SafeRelease(bound.color);
	SafeRelease(bound.normal);
	SafeRelease(bound.shader);
}

ID3D11RasterizerState* Core::ScissoredRaster(ID3D11RasterizerState* original)
{
	const auto known = scissored_.find(original);
	if (known != scissored_.end())
		return known->second;
	D3D11_RASTERIZER_DESC desc{};
	if (original)
		original->GetDesc(&desc);
	else
	{
		desc.FillMode = D3D11_FILL_SOLID;
		desc.CullMode = D3D11_CULL_BACK;
		desc.DepthClipEnable = TRUE;
	}
	desc.ScissorEnable = TRUE;
	ID3D11RasterizerState* state = nullptr;
	if (FAILED(device_->CreateRasterizerState(&desc, &state)))
		return nullptr;
	if (original)
		original->AddRef();
	scissored_[original] = state;
	return state;
}

bool Core::BeginPickDraw(ID3D11DeviceContext* context, Bindings& bound)
{
	++report_.draws;
	if (records_.size() >= MAX_PICK_DRAWS)
		return false;

	uintptr_t material = 0;
	if (table_ && bound.color)
	{
		auto found = table_->surfaces.find(Table::Key(bound.color, bound.normal));
		if (found == table_->surfaces.end())
			found = table_->surfaces.find(Table::Key(bound.color, nullptr));
		if (found != table_->surfaces.end())
			material = found->second;
	}
	// A draw it doesn't know has to be solid to count, so it can hide what's behind it, as a click should land on what's
	// visible. Anything else it doesn't know is no surface: sky, effects, the passes that finish a frame.
	if (!TestsDepth(context, material == 0))
		return false;

	context->OMGetBlendState(&bound.blend, bound.blendFactor, &bound.sampleMask);
	if (bound.blend)
	{
		D3D11_BLEND_DESC desc{};
		bound.blend->GetDesc(&desc);
		if (desc.RenderTarget[0].BlendEnable)
		{
			SafeRelease(bound.blend);
			return false;
		}
	}

	context->RSGetState(&bound.raster);
	ID3D11RasterizerState* scissored = ScissoredRaster(bound.raster);
	if (!scissored)
	{
		SafeRelease(bound.blend);
		SafeRelease(bound.raster);
		return false;
	}

	if (queries_.size() <= records_.size())
	{
		D3D11_QUERY_DESC desc{D3D11_QUERY_OCCLUSION, 0};
		ID3D11Query* query = nullptr;
		if (FAILED(device_->CreateQuery(&desc, &query)))
		{
			SafeRelease(bound.blend);
			SafeRelease(bound.raster);
			return false;
		}
		queries_.push_back(query);
	}

	bound.scissorCount = 0;
	context->RSGetScissorRects(&bound.scissorCount, nullptr);
	if (bound.scissorCount)
		context->RSGetScissorRects(&bound.scissorCount, bound.scissors);
	const LONG x = LONG(bound.viewport.TopLeftX + pickU_ * bound.viewport.Width);
	const LONG y = LONG(bound.viewport.TopLeftY + pickV_ * bound.viewport.Height);
	const D3D11_RECT pixel{x, y, x + 1, y + 1};
	context->RSSetState(scissored);
	context->RSSetScissorRects(1, &pixel);
	context->OMSetBlendState(noColor_, bound.blendFactor, bound.sampleMask);

	Record record{bound.color, bound.normal, material};
	if (record.color)
		record.color->AddRef();
	if (record.normal)
		record.normal->AddRef();
	records_.push_back(record);
	context->Begin(queries_[records_.size() - 1]);
	return true;
}

void Core::EndPickDraw(ID3D11DeviceContext* context, Bindings& bound)
{
	context->End(queries_[records_.size() - 1]);
	context->OMSetBlendState(bound.blend, bound.blendFactor, bound.sampleMask);
	context->RSSetState(bound.raster);
	context->RSSetScissorRects(bound.scissorCount, bound.scissorCount ? bound.scissors : nullptr);
	SafeRelease(bound.blend);
	SafeRelease(bound.raster);
}

void Core::BeginHighlightDraw(ID3D11DeviceContext* context, Bindings& bound)
{
	context->OMGetBlendState(&bound.blend, bound.blendFactor, &bound.sampleMask);
	const FLOAT factor[4] = {highlightColor_[0], highlightColor_[1], highlightColor_[2], 0};
	context->OMSetBlendState(additive_, factor, bound.sampleMask);
	context->PSSetShader(flat_, nullptr, 0);
}

void Core::EndHighlightDraw(ID3D11DeviceContext* context, Bindings& bound)
{
	context->PSSetShader(bound.shader, nullptr, 0);
	context->OMSetBlendState(bound.blend, bound.blendFactor, bound.sampleMask);
	SafeRelease(bound.blend);
}
}
