#include "Decode.h"

#include <algorithm>
#include <cstring>

namespace Pom::Decode
{
namespace
{
	// BC4, and BC3 and BC5's channels: two end points and sixteen 3-bit picks.
	void Alpha(const uint8_t* block, uint8_t values[16])
	{
		const int a = block[0], b = block[1];
		int table[8] = {a, b};
		if (a > b)
		{
			for (int i = 1; i < 7; ++i)
				table[i + 1] = ((7 - i) * a + i * b + 3) / 7;
		}
		else
		{
			for (int i = 1; i < 5; ++i)
				table[i + 1] = ((5 - i) * a + i * b + 2) / 5;
			table[6] = 0;
			table[7] = 255;
		}
		uint64_t bits = 0;
		for (int i = 0; i < 6; ++i)
			bits |= uint64_t(block[2 + i]) << (8 * i);
		for (int i = 0; i < 16; ++i)
			values[i] = uint8_t(table[(bits >> (3 * i)) & 7]);
	}

	// BC1's colors. Only BC1 itself has the three-color mode.
	void Color(const uint8_t* block, uint8_t rgba[16][4], bool threeColorMode)
	{
		const int c0 = block[0] | (block[1] << 8), c1 = block[2] | (block[3] << 8);
		const auto expand = [](int c, int out[3])
		{
			out[0] = ((c >> 11) & 31) * 255 / 31;
			out[1] = ((c >> 5) & 63) * 255 / 63;
			out[2] = (c & 31) * 255 / 31;
		};
		int table[4][4] = {};
		expand(c0, table[0]);
		expand(c1, table[1]);
		table[0][3] = table[1][3] = table[2][3] = table[3][3] = 255;
		for (int c = 0; c < 3; ++c)
		{
			if (threeColorMode && c0 <= c1)
			{
				table[2][c] = (table[0][c] + table[1][c]) / 2;
				table[3][c] = 0;
			}
			else
			{
				table[2][c] = (2 * table[0][c] + table[1][c]) / 3;
				table[3][c] = (table[0][c] + 2 * table[1][c]) / 3;
			}
		}
		if (threeColorMode && c0 <= c1)
			table[3][3] = 0;
		uint32_t bits;
		memcpy(&bits, block + 4, 4);
		for (int i = 0; i < 16; ++i)
		{
			const int* picked = table[(bits >> (2 * i)) & 3];
			for (int c = 0; c < 4; ++c)
				rgba[i][c] = uint8_t(picked[c]);
		}
	}

	template <typename DecodeBlock>
	void Blocks(uint32_t width, uint32_t height, const uint8_t* data, uint32_t pitch, uint32_t blockBytes, Bake::Pixels& out, DecodeBlock decode)
	{
		for (uint32_t by = 0; by * 4 < height; ++by)
		{
			const uint8_t* row = data + size_t(by) * pitch;
			for (uint32_t bx = 0; bx * 4 < width; ++bx)
			{
				uint8_t rgba[16][4];
				decode(row + size_t(bx) * blockBytes, rgba);
				for (uint32_t y = 0; y < 4 && by * 4 + y < height; ++y)
				{
					for (uint32_t x = 0; x < 4 && bx * 4 + x < width; ++x)
						memcpy(&out.rgba[(size_t(by * 4 + y) * width + bx * 4 + x) * 4], rgba[y * 4 + x], 4);
				}
			}
		}
	}

	bool IsBlank(const Bake::Pixels& pixels)
	{
		for (size_t i = 0; i < pixels.rgba.size(); i += 4)
		{
			if (pixels.rgba[i] || pixels.rgba[i + 1] || pixels.rgba[i + 2])
				return false;
		}
		return true;
	}
}

bool Level(DXGI_FORMAT format, uint32_t width, uint32_t height, const uint8_t* data, uint32_t pitch, Bake::Pixels& out)
{
	out.width = width;
	out.height = height;
	out.rgba.assign(size_t(width) * height * 4, 255);
	switch (format)
	{
	case DXGI_FORMAT_BC1_TYPELESS:
	case DXGI_FORMAT_BC1_UNORM:
	case DXGI_FORMAT_BC1_UNORM_SRGB:
		Blocks(width, height, data, pitch, 8, out, [](const uint8_t* block, uint8_t rgba[16][4]) { Color(block, rgba, true); });
		return true;
	case DXGI_FORMAT_BC2_TYPELESS:
	case DXGI_FORMAT_BC2_UNORM:
	case DXGI_FORMAT_BC2_UNORM_SRGB:
		Blocks(width, height, data, pitch, 16, out, [](const uint8_t* block, uint8_t rgba[16][4])
		{
			Color(block + 8, rgba, false);
			for (int i = 0; i < 16; ++i)
				rgba[i][3] = uint8_t(((block[i / 2] >> ((i & 1) * 4)) & 15) * 17);
		});
		return true;
	case DXGI_FORMAT_BC3_TYPELESS:
	case DXGI_FORMAT_BC3_UNORM:
	case DXGI_FORMAT_BC3_UNORM_SRGB:
		Blocks(width, height, data, pitch, 16, out, [](const uint8_t* block, uint8_t rgba[16][4])
		{
			Color(block + 8, rgba, false);
			uint8_t alpha[16];
			Alpha(block, alpha);
			for (int i = 0; i < 16; ++i)
				rgba[i][3] = alpha[i];
		});
		return true;
	case DXGI_FORMAT_BC4_TYPELESS:
	case DXGI_FORMAT_BC4_UNORM:
		Blocks(width, height, data, pitch, 8, out, [](const uint8_t* block, uint8_t rgba[16][4])
		{
			uint8_t red[16];
			Alpha(block, red);
			for (int i = 0; i < 16; ++i)
			{
				rgba[i][0] = rgba[i][1] = rgba[i][2] = red[i];
				rgba[i][3] = 255;
			}
		});
		return true;
	case DXGI_FORMAT_BC5_TYPELESS:
	case DXGI_FORMAT_BC5_UNORM:
		Blocks(width, height, data, pitch, 16, out, [](const uint8_t* block, uint8_t rgba[16][4])
		{
			uint8_t red[16], green[16];
			Alpha(block, red);
			Alpha(block + 8, green);
			for (int i = 0; i < 16; ++i)
			{
				rgba[i][0] = red[i];
				rgba[i][1] = green[i];
				rgba[i][2] = 0;
				rgba[i][3] = 255;
			}
		});
		return true;
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		for (uint32_t y = 0; y < height; ++y)
			memcpy(&out.rgba[size_t(y) * width * 4], data + size_t(y) * pitch, size_t(width) * 4);
		return true;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		for (uint32_t y = 0; y < height; ++y)
		{
			const uint8_t* from = data + size_t(y) * pitch;
			uint8_t* to = &out.rgba[size_t(y) * width * 4];
			for (uint32_t x = 0; x < width; ++x)
			{
				to[x * 4] = from[x * 4 + 2];
				to[x * 4 + 1] = from[x * 4 + 1];
				to[x * 4 + 2] = from[x * 4];
				to[x * 4 + 3] = from[x * 4 + 3];
			}
		}
		return true;
	default:
		out.rgba.clear();
		return false;
	}
}

bool Read(ID3D11DeviceContext* context, ID3D11ShaderResourceView* view, std::vector<Bake::Pixels>& out, std::string& error)
{
	out.clear();
	ID3D11Resource* resource = nullptr;
	view->GetResource(&resource);
	ID3D11Texture2D* texture = nullptr;
	if (resource)
		resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture));
	if (resource)
		resource->Release();
	if (!texture)
	{
		error = "it isn't a 2D texture";
		return false;
	}

	D3D11_TEXTURE2D_DESC desc{};
	texture->GetDesc(&desc);
	D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
	view->GetDesc(&viewDesc);
	const UINT firstMip = viewDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D ? viewDesc.Texture2D.MostDetailedMip : 0;

	D3D11_TEXTURE2D_DESC copyDesc = desc;
	copyDesc.Usage = D3D11_USAGE_STAGING;
	copyDesc.BindFlags = 0;
	copyDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	copyDesc.MiscFlags = 0;
	ID3D11Device* device = nullptr;
	texture->GetDevice(&device);
	ID3D11Texture2D* copy = nullptr;
	HRESULT hr = desc.SampleDesc.Count == 1 && desc.ArraySize == 1 ? device->CreateTexture2D(&copyDesc, nullptr, &copy) : E_INVALIDARG;
	device->Release();
	if (FAILED(hr))
	{
		texture->Release();
		error = "it can't be read back";
		return false;
	}
	context->CopyResource(copy, texture);
	texture->Release();

	for (UINT mip = firstMip; mip < desc.MipLevels; ++mip)
	{
		const UINT width = (std::max)(desc.Width >> mip, 1u), height = (std::max)(desc.Height >> mip, 1u);
		D3D11_MAPPED_SUBRESOURCE map{};
		if (FAILED(context->Map(copy, D3D11CalcSubresource(mip, 0, desc.MipLevels), D3D11_MAP_READ, 0, &map)))
			continue;
		Bake::Pixels level;
		const bool known = Level(desc.Format, width, height, static_cast<const uint8_t*>(map.pData), map.RowPitch, level);
		context->Unmap(copy, D3D11CalcSubresource(mip, 0, desc.MipLevels));
		if (!known)
		{
			copy->Release();
			out.clear();
			error = "its format isn't supported (" + std::to_string(int(desc.Format)) + ")";
			return false;
		}
		// The streamer fills the big mips last, so a texture can exist with nothing in them yet.
		if (out.empty() && IsBlank(level))
			continue;
		out.push_back(std::move(level));
	}
	copy->Release();
	if (out.empty())
		error = "the texture has no pixels yet";
	return !out.empty();
}
}
