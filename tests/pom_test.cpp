// Checks the POM patch without the game, on a real D3D11 device. What's at stake in each mode:
//
//   pom_test.exe --ray [--hardware]
//     The prologue alone, as a pixel shader writing the UV it found, against pure geometry: planes in any orientation,
//     with rotated, mirrored and stretched UVs. Where a ray meets a flat height is a line/plane intersection, so sign
//     and size of the shift are checked to the pixel. Stripes check that it stops at the first surface it meets.
//
//   pom_test.exe --shadow [--hardware]
//     The sun's relief shadow alone, against the same walk on the CPU in far finer steps, over walls and inverted.
//
//   pom_test.exe --shaders <list of .cso> [--hardware] [--limit n]
//     The patch on the game's own shaders. For each:
//      * it patches and the runtime accepts it
//      * depth 0 renders what the original renders
//      * height 1 everywhere (nothing to go down to) renders what the original renders
//      * height 0 everywhere renders what the original renders with layer 0's UV moved by the expected shift, while
//        lightmap UVs, other layers and lighting positions stay put
//      * everything on, relief shadows and crevices, changes nothing on a surface with no relief
//      * everything on at height 0, the shadow lookup equals the original's moved to where the ray went
//
//   pom_test.exe --picture <lit ps .cso> <color .dds> <normal .dds> <.pom> <out prefix> [floor|wall] [depth scale]
//                [shadows|noshadows|relief|crevices|pbr] [sun height] [--hardware]
//     The shader with real textures on a plane, without and with POM, as .ppm files to look at. Also times both. pbr puts
//     Brdf's patch in place of POM's, with the sun ahead, where highlights are, and the constants at b12 unbound.
//
//   pom_test.exe --bake <folder of .pom from tools/bake_pom.py> <folders of OAT-dumped .dds>
//     The plugin's own decoders and baker against the numpy one, on real textures.
//
//   pom_test.exe --read <.dds> [--hardware]
//     Reading textures back from the GPU, as the tool does: all mips, equal to the file, skipping mips not streamed in yet.
//
//   pom_test.exe --pick [--hardware]
//     The picker on a staged scene: surfaces behind each other, an alpha-tested hole, glass, something it doesn't know. It
//     has to find what's visible at the pixel and leave the frame exactly as it was. The highlight has to change the
//     selected surface's visible pixels and nothing else.

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "../src/Bake.h"
#include "../src/Brdf.h"
#include "../src/Decode.h"
#include "../src/Dxbc.h"
#include "../src/Picker.h"
#include "../src/PomFile.h"
#include "../src/prologue/p_0_0_0.h"
#include "../src/prologue/p_1_0_0.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

using Microsoft::WRL::ComPtr;
using namespace Pom;

namespace
{
	constexpr UINT CB_BYTES = 4096 * 16;

	struct Vec3
	{
		float x, y, z;
	};

	Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
	Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
	Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
	float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
	Vec3 Normalize(Vec3 a) { return a * (1.0f / std::sqrt(Dot(a, a))); }

	std::vector<uint8_t> ReadFile(const std::string& path)
	{
		std::ifstream f(path, std::ios::binary);
		return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
	}

	void Check(HRESULT hr, const char* what)
	{
		if (FAILED(hr))
		{
			printf("FAILED %s: 0x%08X\n", what, unsigned(hr));
			exit(1);
		}
	}

	std::string VectorText(Vec3 v)
	{
		std::ostringstream s;
		s.precision(9);
		s << "float3(" << v.x << "," << v.y << "," << v.z << ")";
		return s.str();
	}

	struct Lab
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> context;
		ComPtr<ID3D11Texture2D> target, readback, depth;
		ComPtr<ID3D11RenderTargetView> rtv;
		ComPtr<ID3D11DepthStencilView> dsv;
		ComPtr<ID3D11Buffer> buffers[4];
		ComPtr<ID3D11SamplerState> sampler, compare, aniso;
		UINT width = 0, height = 0;
	};

	ComPtr<ID3D11ShaderResourceView> MakeTexture(ID3D11Device* device, UINT width, UINT height, const uint32_t* pixels, UINT mips = 1)
	{
		D3D11_TEXTURE2D_DESC d{};
		d.Width = width;
		d.Height = height;
		d.MipLevels = mips;
		d.ArraySize = 1;
		d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_IMMUTABLE;
		d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		std::vector<D3D11_SUBRESOURCE_DATA> data(mips);
		const uint32_t* at = pixels;
		for (UINT i = 0; i < mips; ++i)
		{
			data[i] = {at, width * 4, 0};
			at += width * height;
			width = (std::max)(width / 2, 1u);
			height = (std::max)(height / 2, 1u);
		}
		ComPtr<ID3D11Texture2D> texture;
		Check(device->CreateTexture2D(&d, data.data(), &texture), "texture");
		ComPtr<ID3D11ShaderResourceView> view;
		Check(device->CreateShaderResourceView(texture.Get(), nullptr, &view), "srv");
		return view;
	}

	void CreateLab(Lab& lab, UINT width, UINT height, bool hardware)
	{
		lab.width = width;
		lab.height = height;
		const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
		Check(D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1,
			D3D11_SDK_VERSION, &lab.device, nullptr, &lab.context), "device");
		D3D11_TEXTURE2D_DESC d{};
		d.Width = width;
		d.Height = height;
		d.MipLevels = d.ArraySize = 1;
		d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		d.SampleDesc.Count = 1;
		d.BindFlags = D3D11_BIND_RENDER_TARGET;
		Check(lab.device->CreateTexture2D(&d, nullptr, &lab.target), "target");
		Check(lab.device->CreateRenderTargetView(lab.target.Get(), nullptr, &lab.rtv), "rtv");
		d.BindFlags = 0;
		d.Usage = D3D11_USAGE_STAGING;
		d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		Check(lab.device->CreateTexture2D(&d, nullptr, &lab.readback), "readback");

		D3D11_BUFFER_DESC b{};
		b.ByteWidth = CB_BYTES;
		b.Usage = D3D11_USAGE_DEFAULT;
		b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		for (auto& buffer : lab.buffers)
			Check(lab.device->CreateBuffer(&b, nullptr, &buffer), "cb");

		D3D11_SAMPLER_DESC sd{};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		Check(lab.device->CreateSamplerState(&sd, &lab.sampler), "sampler");
		sd.Filter = D3D11_FILTER_ANISOTROPIC;
		sd.MaxAnisotropy = 4;
		Check(lab.device->CreateSamplerState(&sd, &lab.aniso), "aniso");
		sd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
		sd.ComparisonFunc = D3D11_COMPARISON_GREATER;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		Check(lab.device->CreateSamplerState(&sd, &lab.compare), "compare sampler");

		D3D11_RASTERIZER_DESC raster{};
		raster.FillMode = D3D11_FILL_SOLID;
		raster.CullMode = D3D11_CULL_NONE;
		raster.DepthClipEnable = TRUE;
		ComPtr<ID3D11RasterizerState> rasterState;
		Check(lab.device->CreateRasterizerState(&raster, &rasterState), "raster");
		lab.context->RSSetState(rasterState.Get());

		const D3D11_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
		lab.context->RSSetViewports(1, &viewport);
		lab.context->OMSetRenderTargets(1, lab.rtv.GetAddressOf(), nullptr);
		lab.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	}

	ComPtr<ID3D11VertexShader> CompileVertexShader(Lab& lab, const std::string& source)
	{
		ComPtr<ID3DBlob> code, errors;
		if (FAILED(D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, "main", "vs_4_0", 0, 0, &code, &errors)))
		{
			printf("vertex shader didn't compile: %s\n%s\n", errors ? static_cast<char*>(errors->GetBufferPointer()) : "?", source.c_str());
			exit(1);
		}
		ComPtr<ID3D11VertexShader> shader;
		Check(lab.device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader), "vs");
		return shader;
	}

	std::vector<float> Render(Lab& lab, ID3D11PixelShader* shader, UINT vertices)
	{
		const float clear[4] = {0, 0, 0, 0};
		lab.context->ClearRenderTargetView(lab.rtv.Get(), clear);
		lab.context->PSSetShader(shader, nullptr, 0);
		lab.context->Draw(vertices, 0);
		lab.context->CopyResource(lab.readback.Get(), lab.target.Get());
		D3D11_MAPPED_SUBRESOURCE map{};
		Check(lab.context->Map(lab.readback.Get(), 0, D3D11_MAP_READ, 0, &map), "map");
		std::vector<float> pixels(size_t(lab.width) * lab.height * 4);
		for (UINT y = 0; y < lab.height; ++y)
			memcpy(pixels.data() + size_t(y) * lab.width * 4, static_cast<uint8_t*>(map.pData) + y * map.RowPitch, lab.width * 16);
		lab.context->Unmap(lab.readback.Get(), 0);
		return pixels;
	}

	// ---------------------------------------------------------------------------------------------------- --ray

	struct RayScene
	{
		const char* name;
		Vec3 eye, origin, axisA, axisB;  // The plane is origin + a * axisA + b * axisB.
		float u[3], v[3];                // uv = (u[0] a + u[1] b + u[2], v[0] a + v[1] b + v[2])
	};

	// A camera looking at the plane; the vertex shader hands the prologue what the game's does.
	std::string RayVertexShader(const RayScene& s)
	{
		const Vec3 normal = Normalize(Cross(s.axisA, s.axisB));
		const Vec3 towards = Normalize(s.origin - s.eye);
		const Vec3 right = Normalize(Cross(towards, Vec3{0.31f, 0.2f, 0.93f}));
		const Vec3 up = Cross(right, towards);
		std::ostringstream o;
		o.precision(9);
		o << "struct Out { float4 uv : TEXCOORD0; float4 n : TEXCOORD1; float4 p : TEXCOORD5; float4 pos : SV_Position; };\n"
		  << "Out main(uint id : SV_VertexID) {\n"
		  << " float2 corners[6] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(-1,1), float2(1,-1), float2(1,1) };\n"
		  << " float2 c = corners[id] * 60; float3 world = " << VectorText(s.origin) << " + c.x * " << VectorText(s.axisA) << " + c.y * " << VectorText(s.axisB) << ";\n"
		  << " float3 p = world - " << VectorText(s.eye) << ";\n"
		  << " Out o; o.p = float4(p, 1); o.n = float4(" << VectorText(normal) << ", 1);\n"
		  << " o.uv = float4(" << s.u[0] << " * c.x + " << s.u[1] << " * c.y + " << s.u[2] << ", " << s.v[0] << " * c.x + " << s.v[1] << " * c.y + " << s.v[2] << ", 0, 0);\n"
		  << " float w = dot(p, " << VectorText(towards) << ");\n"
		  << " o.pos = float4(dot(p, " << VectorText(right) << ") * 2.0, dot(p, " << VectorText(up) << ") * 2.0, 0.5 * w, w); return o; }\n";
		return o.str();
	}

	bool RayTest(bool hardware)
	{
		Lab lab;
		CreateLab(lab, 64, 64, hardware);
		ComPtr<ID3D11PixelShader> prologue;
		Check(lab.device->CreatePixelShader(g_Prologue_0_0_0, sizeof(g_Prologue_0_0_0), nullptr, &prologue), "prologue");

		D3D11_BUFFER_DESC b{};
		b.ByteWidth = sizeof(Dxbc::Literals);
		b.Usage = D3D11_USAGE_DEFAULT;
		b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		ComPtr<ID3D11Buffer> literalBuffer;
		Check(lab.device->CreateBuffer(&b, nullptr, &literalBuffer), "literals");
		lab.context->PSSetConstantBuffers(12, 1, literalBuffer.GetAddressOf());
		lab.context->PSSetSamplers(1, 1, lab.sampler.GetAddressOf());

		const Vec3 slant = Normalize({0.2f, 0.6f, -0.7f});
		const Vec3 wallA = Normalize({0.8f, 0.6f, 0}), wallB = {0, 0, 1};
		const Vec3 slantedA = Normalize(Cross(slant, {1, 0, 0})), slantedB = Normalize(Cross(slant, slantedA));
		const RayScene scenes[] = {
			{"floor, plain uv", {0, -40, 60}, {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1 / 128.f, 0, 0}, {0, 1 / 128.f, 0}},
			{"floor, v mirrored", {10, -40, 60}, {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1 / 128.f, 0, 0.3f}, {0, -1 / 128.f, 0.1f}},
			{"floor, u mirrored", {-10, -40, 60}, {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {-1 / 128.f, 0, 0}, {0, 1 / 128.f, 0}},
			{"floor, rotated 30 degrees", {5, -30, 50}, {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0.866f / 128, 0.5f / 128, 0}, {-0.5f / 128, 0.866f / 128, 0}},
			{"floor, u scaled twice v", {5, -30, 50}, {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1 / 64.f, 0, 0}, {0, 1 / 128.f, 0}},
			{"wall", {-70, -80, 20}, {0, 0, 0}, wallA, wallB, {1 / 96.f, 0, 0}, {0, -1 / 96.f, 0}},
			{"slanted plane", slantedA * 30.0f + slantedB * -20.0f + Normalize(Cross(slantedA, slantedB)) * 70.0f, {0, 0, 0}, slantedA, slantedB, {1 / 100.f, 0, 0}, {0, 1 / 100.f, 0}},
		};

		bool passed = true;
		Dxbc::Literals literals;
		literals.depthUv = 0.04f;
		literals.mostSteps = 32;
		literals.fewestSteps = 32;
		literals.steepest = 0.02f;
		literals.textureWidth = 8;
		literals.fadeEnd = 100000;
		literals.fadeScale = 1;
		lab.context->UpdateSubresource(literalBuffer.Get(), 0, nullptr, &literals, 0, 0);

		const auto flatHeight = [&](uint8_t height)
		{
			std::vector<uint32_t> pixels(64, 0xFF000000u | (uint32_t(height) << 16));
			return MakeTexture(lab.device.Get(), 8, 8, pixels.data());
		};
		const uint8_t heights[] = {255, 128, 0};
		for (const RayScene& s : scenes)
		{
			const std::string source = RayVertexShader(s);
			ComPtr<ID3D11VertexShader> vs = CompileVertexShader(lab, source);
			lab.context->VSSetShader(vs.Get(), nullptr, 0);

			const Vec3 normal = Normalize(Cross(s.axisA, s.axisB));
			if (Dot(normal, s.eye - s.origin) <= 0)
			{
				printf("scene '%s' faces away from its camera\n", s.name);
				return false;
			}
			const float gradientU = std::sqrt(s.u[0] * s.u[0] + s.u[1] * s.u[1]), gradientV = std::sqrt(s.v[0] * s.v[0] + s.v[1] * s.v[1]);
			const float worldDepth = 0.04f / std::sqrt(gradientU * gradientV);

			// Two orders of magnitude of UV shift below what a wrong sign or scale would give. A negative depth is Invert: the
			// same shift the other way.
			for (const float depthSign : {1.0f, -1.0f})
			for (const uint8_t height : heights)
			{
				literals.depthUv = 0.04f * depthSign;
				lab.context->UpdateSubresource(literalBuffer.Get(), 0, nullptr, &literals, 0, 0);
				auto heightView = flatHeight(height);
				lab.context->PSSetShaderResources(1, 1, heightView.GetAddressOf());
				const std::vector<float> first = Render(lab, prologue.Get(), 6);
				// Corners of each covered pixel aren't known here, so read the plane back from the unshifted UV.
				literals.depthUv = 0;
				lab.context->UpdateSubresource(literalBuffer.Get(), 0, nullptr, &literals, 0, 0);
				const std::vector<float> flat = Render(lab, prologue.Get(), 6);

				const float det = s.u[0] * s.v[1] - s.u[1] * s.v[0];
				float worst = 0;
				int covered = 0;
				for (size_t i = 0; i < first.size(); i += 4)
				{
					if (flat[i + 3] == 0 && flat[i] == 0 && flat[i + 1] == 0)
						continue;
					++covered;
					const float du = flat[i] - s.u[2], dv = flat[i + 1] - s.v[2];
					const float a = (du * s.v[1] - dv * s.u[1]) / det, b = (dv * s.u[0] - du * s.v[0]) / det;
					const Vec3 point = s.origin + s.axisA * a + s.axisB * b;
					const Vec3 view = Normalize(point - s.eye);
					const float along = -Dot(view, normal);
					// Straight line from the camera, through the plane, down to the height it was given.
					const Vec3 hit = point + view * (depthSign * worldDepth * (1 - height / 255.0f) / along);
					const float hitA = Dot(hit - s.origin, s.axisA), hitB = Dot(hit - s.origin, s.axisB);
					const float wantU = s.u[0] * hitA + s.u[1] * hitB + s.u[2], wantV = s.v[0] * hitA + s.v[1] * hitB + s.v[2];
					worst = (std::max)(worst, (std::max)(std::fabs(first[i] - wantU), std::fabs(first[i + 1] - wantV)));
				}
				const bool ok = covered > 100 && worst < 0.04f / 32;
				printf("%-28s height %3u%s: %d pixels, worst UV error %.6f (one march step is %.6f) %s\n", s.name, height, depthSign < 0 ? " inverted" : "", covered,
					worst, 0.04f / 32, ok ? "ok" : "WRONG");
				passed &= ok;
			}
		}

		// Stripes: the first thing a ray meets stops it, however deep the next stripe is. Marched on the CPU in much
		// finer steps, on the same bilinear height.
		const UINT stripeWidth = 64;
		std::vector<uint32_t> stripes(stripeWidth * 4);
		std::vector<float> stripeHeight(stripeWidth);
		for (UINT x = 0; x < stripeWidth; ++x)
		{
			const uint32_t value = (x / 8) % 2 ? 40 : 255;
			stripeHeight[x] = value / 255.0f;
			for (UINT y = 0; y < 4; ++y)
				stripes[y * stripeWidth + x] = 0xFF000000u | (value << 16);
		}
		D3D11_TEXTURE2D_DESC d{};
		auto stripeView = [&]
		{
			std::vector<uint32_t> square(stripeWidth * stripeWidth);
			for (UINT y = 0; y < stripeWidth; ++y)
				memcpy(&square[y * stripeWidth], stripes.data(), stripeWidth * 4);
			return MakeTexture(lab.device.Get(), stripeWidth, stripeWidth, square.data());
		}();
		const auto sampleStripe = [&](float u)
		{
			const float x = (u - std::floor(u)) * stripeWidth - 0.5f;
			const float base = std::floor(x);
			const float t = x - base;
			const int i0 = (int(base) + stripeWidth) % stripeWidth, i1 = (i0 + 1) % stripeWidth;
			return stripeHeight[i0] * (1 - t) + stripeHeight[i1] * t;
		};
		lab.context->PSSetShaderResources(1, 1, stripeView.GetAddressOf());
		literals.textureWidth = float(stripeWidth);
		literals.mostSteps = literals.fewestSteps = 64;
		literals.depthUv = 0.04f;
		lab.context->UpdateSubresource(literalBuffer.Get(), 0, nullptr, &literals, 0, 0);
		const RayScene stripeScene = {"stripes", {0, -60, 40}, {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 1 / 128.f, 0}, {1 / 128.f, 0, 0}};
		ComPtr<ID3D11VertexShader> vs = CompileVertexShader(lab, RayVertexShader(stripeScene));
		lab.context->VSSetShader(vs.Get(), nullptr, 0);
		const std::vector<float> marched = Render(lab, prologue.Get(), 6);
		literals.depthUv = 0;
		lab.context->UpdateSubresource(literalBuffer.Get(), 0, nullptr, &literals, 0, 0);
		const std::vector<float> flat = Render(lab, prologue.Get(), 6);
		int covered = 0, close = 0;
		for (size_t i = 0; i < flat.size(); i += 4)
		{
			if (flat[i] == 0 && flat[i + 1] == 0)
				continue;
			++covered;
			const float b = flat[i] * 128, a = flat[i + 1] * 128;
			const Vec3 point = Vec3{a, b, 0};
			const Vec3 view = Normalize(point - stripeScene.eye);
			const float perDepth = 0.04f * 128 / -view.z;  // World units of ray per unit of height.
			float depth = 0, u = flat[i];
			for (int step = 0; step < 20000 && depth < 1 - sampleStripe(u); ++step)
			{
				depth += 1.0f / 20000;
				u = flat[i] + (view.y * perDepth * depth) / 128;
			}
			close += std::fabs(marched[i] - u) < 0.04f;
		}
		const bool stripesOk = covered > 100 && close >= covered * 0.97;
		printf("stripes: %d of %d pixels land where a fine march does %s\n", close, covered, stripesOk ? "ok" : "WRONG");
		return passed && stripesOk;
	}

	// ---------------------------------------------------------------------------------------------------- --shadow

	// The sun's relief shadow on its own, against the same walk on the CPU in far finer steps, over a height made of walls.
	bool ShadowTest(bool hardware)
	{
		Lab lab;
		CreateLab(lab, 96, 96, hardware);
		ComPtr<ID3D11PixelShader> prologue;
		Check(lab.device->CreatePixelShader(g_Prologue_1_0_0, sizeof(g_Prologue_1_0_0), nullptr, &prologue), "prologue");

		D3D11_TEXTURE2D_DESC d{};
		lab.target->GetDesc(&d);
		ComPtr<ID3D11Texture2D> lightTarget;
		ComPtr<ID3D11RenderTargetView> lightView;
		Check(lab.device->CreateTexture2D(&d, nullptr, &lightTarget), "light target");
		Check(lab.device->CreateRenderTargetView(lightTarget.Get(), nullptr, &lightView), "light rtv");
		ID3D11RenderTargetView* targets[5] = {lab.rtv.Get(), nullptr, nullptr, nullptr, lightView.Get()};
		lab.context->OMSetRenderTargets(5, targets, nullptr);

		D3D11_BUFFER_DESC b{};
		b.ByteWidth = sizeof(Dxbc::Literals);
		b.Usage = D3D11_USAGE_DEFAULT;
		b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		ComPtr<ID3D11Buffer> literalBuffer;
		Check(lab.device->CreateBuffer(&b, nullptr, &literalBuffer), "literals");
		lab.context->PSSetConstantBuffers(12, 1, literalBuffer.GetAddressOf());
		lab.context->PSSetConstantBuffers(0, 1, lab.buffers[0].GetAddressOf());
		lab.context->PSSetSamplers(1, 1, lab.sampler.GetAddressOf());

		const UINT width = 64;
		std::vector<uint32_t> walls(size_t(width) * width);
		std::vector<float> heights(width);
		for (UINT x = 0; x < width; ++x)
		{
			const uint32_t value = (x / 16) % 2 ? 40 : 255;
			heights[x] = value / 255.0f;
			for (UINT y = 0; y < width; ++y)
				walls[size_t(y) * width + x] = 0xFF000000u | (value << 16);
		}
		auto wallView = MakeTexture(lab.device.Get(), width, width, walls.data());
		lab.context->PSSetShaderResources(1, 1, wallView.GetAddressOf());
		const auto sampleWall = [&](float u)
		{
			const float x = (u - std::floor(u)) * width - 0.5f;
			const float base = std::floor(x);
			const float t = x - base;
			const int i0 = (int(base) + width) % width, i1 = (i0 + 1) % width;
			return heights[i0] * (1 - t) + heights[i1] * t;
		};

		const RayScene scene = {"walls", {0, -30, 90}, {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1 / 128.f, 0, 0}, {0, 1 / 128.f, 0}};
		ComPtr<ID3D11VertexShader> vs = CompileVertexShader(lab, RayVertexShader(scene));
		lab.context->VSSetShader(vs.Get(), nullptr, 0);

		Dxbc::Literals literals;
		literals.depthUv = 0.04f;
		literals.mostSteps = literals.fewestSteps = 64;
		literals.steepest = 0.02f;
		literals.textureWidth = float(width);
		literals.fadeEnd = 100000;
		literals.fadeScale = 1;
		literals.traceSteps = 64;
		literals.reliefShadows = 1;

		const auto render = [&](Vec3 sun)
		{
			std::vector<float> constants(CB_BYTES / 4, 0.0f);
			memcpy(&constants[18 * 4], &sun, 12);
			constants[19 * 4] = constants[19 * 4 + 1] = constants[19 * 4 + 2] = 1;
			lab.context->UpdateSubresource(lab.buffers[0].Get(), 0, nullptr, constants.data(), 0, 0);
			lab.context->UpdateSubresource(literalBuffer.Get(), 0, nullptr, &literals, 0, 0);
			const float clear[4] = {0, 0, 0, 0};
			lab.context->ClearRenderTargetView(lab.rtv.Get(), clear);
			lab.context->ClearRenderTargetView(lightView.Get(), clear);
			lab.context->PSSetShader(prologue.Get(), nullptr, 0);
			lab.context->Draw(6, 0);
		};
		const auto read = [&](ID3D11Texture2D* texture)
		{
			lab.context->CopyResource(lab.readback.Get(), texture);
			D3D11_MAPPED_SUBRESOURCE map{};
			Check(lab.context->Map(lab.readback.Get(), 0, D3D11_MAP_READ, 0, &map), "map");
			std::vector<float> pixels(size_t(lab.width) * lab.height * 4);
			for (UINT y = 0; y < lab.height; ++y)
				memcpy(pixels.data() + size_t(y) * lab.width * 4, static_cast<uint8_t*>(map.pData) + y * map.RowPitch, lab.width * 16);
			lab.context->Unmap(lab.readback.Get(), 0);
			return pixels;
		};

		const float depth = 0.04f;
		const auto expectedLight = [&](float u, Vec3 sun, float flip)
		{
			const float start = std::fabs(flip - sampleWall(u));
			const float above = 1 - start;
			if (sun.z <= 0 || above < 0.004f)
				return 1.0f;
			const float perHeight = sun.x / (std::max)(sun.z, 0.05f) * depth;
			float arriving = 1;
			const int steps = 4000;
			for (int i = 1; i <= steps; ++i)
			{
				const float rise = above * i / steps;
				const float surface = std::fabs(flip - sampleWall(u + perHeight * rise));
				arriving = (std::min)(arriving, 1 - (std::min)((std::max)((surface - (start + rise)) * 14, 0.0f), 1.0f));
			}
			return arriving;
		};

		bool passed = true;
		for (const float sign : {1.0f, -1.0f})
		{
			literals.depthUv = depth * sign;
			const float flip = sign < 0 ? 1.0f : 0.0f;
			for (const Vec3 sun : {Normalize({0.8f, 0.1f, 0.5f}), Normalize({-0.9f, 0.2f, 0.35f}), Normalize({0.0f, 0.1f, 1.0f})})
			{
				render(sun);
				const std::vector<float> hit = read(lab.target.Get()), light = read(lightTarget.Get());
				int covered = 0, close = 0, dark = 0, lit = 0;
				for (size_t i = 0; i < hit.size(); i += 4)
				{
					if (hit[i] == 0 && hit[i + 1] == 0)
						continue;
					++covered;
					const float want = expectedLight(hit[i], sun, flip);
					close += std::fabs(light[i] - want) < 0.1f;
					dark += want < 0.5f;
					lit += want > 0.5f;
				}
				// A sun overhead is the control: nothing to shadow. Low suns have to give a real mix of both.
				const bool mixed = sun.z > 0.9f || (dark > covered / 40 && lit > covered / 40);
				const bool ok = covered > 100 && close >= covered * 0.97 && mixed;
				printf("%s sun (%.2f %.2f %.2f): %d of %d pixels match a fine walk, %d in shadow %s\n", sign < 0 ? "inverted," : "        ", sun.x, sun.y, sun.z, close, covered, dark,
					ok ? "ok" : "WRONG");
				passed &= ok;
			}
		}

		// Turned off, and the sun below the surface: all of the light gets through.
		literals.depthUv = depth;
		literals.reliefShadows = 0;
		render(Normalize({0.8f, 0.1f, 0.5f}));
		std::vector<float> light = read(lightTarget.Get());
		float lowest = 1;
		for (size_t i = 0; i < light.size(); i += 4)
		{
			if (light[i + 3] != 0)
				lowest = (std::min)(lowest, light[i]);
		}
		const bool offOk = lowest == 1;
		printf("relief shadows at 0: darkest pixel %.3f %s\n", lowest, offOk ? "ok" : "WRONG");
		literals.reliefShadows = 1;
		render(Normalize({0.8f, 0.1f, -0.5f}));
		light = read(lightTarget.Get());
		lowest = 1;
		for (size_t i = 0; i < light.size(); i += 4)
		{
			if (light[i + 3] != 0)
				lowest = (std::min)(lowest, light[i]);
		}
		const bool belowOk = lowest == 1;
		printf("sun behind the surface: darkest pixel %.3f %s\n", lowest, belowOk ? "ok" : "WRONG");
		return passed && offOk && belowOk;
	}

	// ------------------------------------------------------------------------------------------------ --shaders

	// A patch of plane far enough from the camera for the shift to be close to linear across it, so the original can be
	// rendered with its UV moved by the shift worked out at the vertices. `shiftScale` is the depth in UV units, signed,
	// 0 for no shift. Every other input has a different smooth ramp.
	std::string ShaderVertexShader(ID3D11ShaderReflection* reflection, Vec3 toPlane, Vec3 tangent, Vec3 binormal, Vec3 normal, float shiftScale,
		const float* lookupMatrix = nullptr, float lookupDepth = 0)
	{
		D3D11_SHADER_DESC desc{};
		reflection->GetDesc(&desc);
		std::ostringstream body, fields;
		body.precision(9);
		int n = 0;
		bool hasPosition = false;
		for (UINT i = 0; i < desc.InputParameters; ++i)
		{
			D3D11_SIGNATURE_PARAMETER_DESC p{};
			reflection->GetInputParameterDesc(i, &p);
			if (p.SystemValueType == D3D_NAME_POSITION)
			{
				fields << "float4 pos : SV_Position;\n";
				hasPosition = true;
				continue;
			}
			if (p.SystemValueType != D3D_NAME_UNDEFINED)
				continue;
			int width = 0;
			for (BYTE mask = p.Mask; mask; mask >>= 1)
				width += mask & 1;
			fields << "float" << width << " f" << n << " : " << p.SemanticName << p.SemanticIndex << ";\n";
			const std::string swizzle = std::string("xyzw").substr(0, width);
			const bool texcoord = std::string(p.SemanticName) == "TEXCOORD";
			if (texcoord && p.SemanticIndex == 0)
				body << "o.f" << n << " = float4(uv * 0.25 + float2(dot(view, " << VectorText(tangent) << "), dot(view, " << VectorText(binormal) << ")) / -dot(view, "
					 << VectorText(normal) << ") * " << shiftScale << ", uv.yx * 0.4 + 0.1)." << swizzle << ";\n";
			else if (texcoord && p.SemanticIndex == 1)
				body << "o.f" << n << " = float4(" << VectorText(normal) << ", 0.8)." << swizzle << ";\n";
			else if (texcoord && p.SemanticIndex == 2)
				body << "o.f" << n << " = float4(" << VectorText(binormal) << ", 0)." << swizzle << ";\n";
			else if (texcoord && p.SemanticIndex == 3)
				body << "o.f" << n << " = float4(" << VectorText(tangent) << ", 0)." << swizzle << ";\n";
			else if (texcoord && p.SemanticIndex == 5)
				body << "o.f" << n << " = float4(toPixel, 1)." << swizzle << ";\n";
			else
			{
				body << "o.f" << n << " = (float4(uv.x - 0.5, uv.y - 0.5, 0.7 - uv.x * 0.4, 1) * float4(1.7, 1.3, 1.1, 1) + float4(sin(" << n << " * 1.3), cos(" << n << " * 2.1), sin(" << n << " * 0.7), 0.5) * 0.4";
				// The lookup at the point the ray reached: it stops `lookupDepth` world units below the surface.
				if (texcoord && p.SemanticIndex == 4 && lookupMatrix)
				{
					body << " + float4(";
					for (int row = 0; row < 4; ++row)
					{
						const float* m = lookupMatrix + row * 4;
						body << (row ? ", " : "") << "dot(" << VectorText({m[0], m[1], m[2]}) << ", view * " << lookupDepth << " / -dot(view, " << VectorText(normal) << "))";
					}
					body << ")";
				}
				body << ")." << swizzle << ";\n";
			}
			++n;
		}
		if (!hasPosition)
			fields << "float4 pos : SV_Position;\n";
		return "struct Out {\n" + fields.str() + "};\nOut main(uint id : SV_VertexID) { Out o; float2 uv = float2((id << 1) & 2, id & 2);\n"
			"o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
			"float3 toPixel = " + VectorText(toPlane) + " + uv.x * 100 * " + VectorText(tangent) + " + uv.y * 100 * " + VectorText(binormal) + ";\n"
			"float3 view = normalize(toPixel);\n" + body.str() + "return o; }\n";
	}

	struct Compared
	{
		float worst = 0;
		int nonFinite = 0;
	};

	Compared Compare(const std::vector<float>& a, const std::vector<float>& b)
	{
		Compared c;
		for (size_t i = 0; i < a.size(); ++i)
		{
			if (i % 4 == 3)
				continue;
			const bool finiteA = std::isfinite(a[i]), finiteB = std::isfinite(b[i]);
			if (finiteA != finiteB)
				++c.nonFinite;
			else if (finiteA)
				c.worst = (std::max)(c.worst, std::fabs(a[i] - b[i]) / (1.0f + std::fabs(a[i])));
		}
		return c;
	}

	int ShaderTest(const std::string& list, bool hardware, int limit)
	{
		Lab lab;
		CreateLab(lab, 16, 16, hardware);

		std::mt19937 random(1234);
		// Random alpha too: a solid layer 1 would hide layer 0, and then nothing could show whether it moved.
		std::vector<uint32_t> base(16 * 16);
		for (auto& p : base)
			p = random() & 0xFF00FFFFu;
		const auto withBlue = [&](uint32_t blue, bool open)
		{
			std::vector<uint32_t> pixels = base;
			for (auto& p : pixels)
				p |= (blue << 16) | (open ? 0xFF000000u : 0);
			return MakeTexture(lab.device.Get(), 16, 16, pixels.data());
		};
		// A normal map's alpha is crevice shading, here wide open.
		auto colors = withBlue(128, false);
		auto heightTop = withBlue(255, true), heightBottom = withBlue(0, true);

		D3D11_TEXTURE2D_DESC s{};
		s.Width = s.Height = 16;
		s.MipLevels = s.ArraySize = 1;
		s.Format = DXGI_FORMAT_R32_FLOAT;
		s.SampleDesc.Count = 1;
		s.Usage = D3D11_USAGE_IMMUTABLE;
		s.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		std::vector<float> depth(16 * 16);
		for (auto& v : depth)
			v = (random() % 1000) / 1000.0f;
		D3D11_SUBRESOURCE_DATA data{depth.data(), 16 * 4, 0};
		ComPtr<ID3D11Texture2D> shadowTexture;
		Check(lab.device->CreateTexture2D(&s, &data, &shadowTexture), "shadow");
		ComPtr<ID3D11ShaderResourceView> shadow;
		Check(lab.device->CreateShaderResourceView(shadowTexture.Get(), nullptr, &shadow), "shadow srv");

		std::vector<uint32_t> faces(4 * 4 * 6);
		for (auto& p : faces)
			p = random() | 0xFF000000u;
		D3D11_TEXTURE2D_DESC cube{};
		cube.Width = cube.Height = 4;
		cube.MipLevels = 1;
		cube.ArraySize = 6;
		cube.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		cube.SampleDesc.Count = 1;
		cube.Usage = D3D11_USAGE_IMMUTABLE;
		cube.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		cube.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
		D3D11_SUBRESOURCE_DATA cubeData[6];
		for (int i = 0; i < 6; ++i)
			cubeData[i] = {faces.data() + i * 16, 16, 0};
		ComPtr<ID3D11Texture2D> cubeTexture;
		Check(lab.device->CreateTexture2D(&cube, cubeData, &cubeTexture), "cube");
		ComPtr<ID3D11ShaderResourceView> cubeView;
		Check(lab.device->CreateShaderResourceView(cubeTexture.Get(), nullptr, &cubeView), "cube srv");

		// Model lighting is a 3D texture: a 2D one in its place reads black, and a black surface can't show what moved.
		std::vector<uint32_t> volume(4 * 4 * 4);
		for (auto& p : volume)
			p = random() | 0xFF000000u;
		D3D11_TEXTURE3D_DESC volumeDesc{};
		volumeDesc.Width = volumeDesc.Height = volumeDesc.Depth = 4;
		volumeDesc.MipLevels = 1;
		volumeDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		volumeDesc.Usage = D3D11_USAGE_IMMUTABLE;
		volumeDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA volumeData{volume.data(), 16, 64};
		ComPtr<ID3D11Texture3D> volumeTexture;
		Check(lab.device->CreateTexture3D(&volumeDesc, &volumeData, &volumeTexture), "volume");
		ComPtr<ID3D11ShaderResourceView> volumeView;
		Check(lab.device->CreateShaderResourceView(volumeTexture.Get(), nullptr, &volumeView), "volume srv");

		std::uniform_real_distribution<float> value(0.05f, 0.95f);
		std::vector<float> scene(CB_BYTES / 4), object(CB_BYTES / 4);
		for (auto& buffer : lab.buffers)
		{
			std::vector<float> values(CB_BYTES / 4);
			for (auto& v : values)
				v = value(random);
			values[20 * 4] = 1.0f;
			lab.context->UpdateSubresource(buffer.Get(), 0, nullptr, values.data(), 0, 0);
			if (&buffer == &lab.buffers[0])
				scene = values;
			if (&buffer == &lab.buffers[3])
				object = values;
		}
		ID3D11Buffer* buffers[4] = {lab.buffers[0].Get(), lab.buffers[1].Get(), lab.buffers[2].Get(), lab.buffers[3].Get()};
		lab.context->PSSetConstantBuffers(0, 4, buffers);
		// Random bits in b3 turn the placed lights' blocks on. One check needs them off.
		D3D11_BUFFER_DESC quietDesc{};
		quietDesc.ByteWidth = CB_BYTES;
		quietDesc.Usage = D3D11_USAGE_DEFAULT;
		quietDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		ComPtr<ID3D11Buffer> quietObject;
		Check(lab.device->CreateBuffer(&quietDesc, nullptr, &quietObject), "quiet object");
		std::vector<float> quiet = object;
		quiet[11 * 4] = quiet[11 * 4 + 1] = 0;
		lab.context->UpdateSubresource(quietObject.Get(), 0, nullptr, quiet.data(), 0, 0);

		const float depthUv = 0.03f;
		const Vec3 tangent = {1, 0, 0}, binormal = {0, 1, 0}, normal = {0, 0, 1};
		const Vec3 toPlane = Normalize({0.45f, 0.62f, -0.64f}) * 4000.0f;

		std::ifstream names(list);
		std::string path;
		int total = 0, failed = 0, unaccepted = 0;
		float worstFlat = 0, worstTop = 0, worstShifted = 0, smallestWrongWay = 1e9f;
		std::map<unsigned, int> sampleCounts;
		float worstAllOn = 0, worstLookup = 0;
		int lit = 0, moved = 0, movedShows = 0, spots = 0, omnis = 0;
		while (std::getline(names, path) && (limit <= 0 || total < limit))
		{
			if (path.empty())
				continue;
			const std::vector<uint8_t> code = ReadFile(path);
			if (code.empty())
			{
				printf("can't read %s\n", path.c_str());
				++failed;
				continue;
			}
			++total;
			ComPtr<ID3D11ShaderReflection> reflection;
			Check(D3DReflect(code.data(), code.size(), IID_PPV_ARGS(&reflection)), "reflect");
			ComPtr<ID3D11PixelShader> original;
			Check(lab.device->CreatePixelShader(code.data(), code.size(), nullptr, &original), "original");

			Dxbc::Literals literals;
			literals.steepest = 0.05f;
			literals.fadeEnd = 1e7f;
			literals.fadeScale = 1;
			literals.textureWidth = 16;
			literals.mostSteps = literals.fewestSteps = 16;
			const auto patchWith = [&](float depthValue, ComPtr<ID3D11PixelShader>& out)
			{
				literals.depthUv = depthValue;
				const Dxbc::Result result = Dxbc::Patch(code.data(), code.size(), literals);
				if (!result.ok)
				{
					printf("%s: %s\n", path.c_str(), result.error.c_str());
					return false;
				}
				++sampleCounts[result.samples];
				if (FAILED(lab.device->CreatePixelShader(result.shader.data(), result.shader.size(), nullptr, &out)))
				{
					printf("%s: the runtime refused it\n", path.c_str());
					++unaccepted;
					return false;
				}
				return true;
			};
			ComPtr<ID3D11PixelShader> flatShader, deepShader, allOnShader;
			if (!patchWith(0, flatShader) || !patchWith(depthUv, deepShader))
			{
				++failed;
				continue;
			}
			// With relief shadows and crevices on: the same shader that goes in the game.
			literals.traceSteps = 8;
			literals.crevice = 1;
			literals.reliefShadows = 1;
			if (!patchWith(depthUv, allOnShader))
			{
				++failed;
				continue;
			}
			const Dxbc::Result features = Dxbc::Patch(code.data(), code.size(), literals);
			lit += features.light != 0;
			moved += features.movedLookup;
			spots += features.spotBlock;
			omnis += features.omniBlock;

			D3D11_SHADER_DESC desc{};
			reflection->GetDesc(&desc);
			for (UINT i = 0; i < desc.BoundResources; ++i)
			{
				D3D11_SHADER_INPUT_BIND_DESC r{};
				reflection->GetResourceBindingDesc(i, &r);
				if (r.Type == D3D_SIT_SAMPLER)
				{
					ID3D11SamplerState* state = (r.uFlags & D3D_SIF_COMPARISON_SAMPLER) ? lab.compare.Get() : lab.sampler.Get();
					lab.context->PSSetSamplers(r.BindPoint, 1, &state);
				}
				else if (r.Type == D3D_SIT_TEXTURE)
				{
					ID3D11ShaderResourceView* view2 = colors.Get();
					if (r.Dimension == D3D_SRV_DIMENSION_TEXTURECUBE)
						view2 = cubeView.Get();
					else if (r.Dimension == D3D_SRV_DIMENSION_TEXTURE3D)
						view2 = volumeView.Get();
					else if (r.BindPoint == 9 || r.BindPoint == 10)
						view2 = shadow.Get();
					lab.context->PSSetShaderResources(r.BindPoint, 1, &view2);
				}
			}

			const auto render = [&](ID3D11PixelShader* shader, ID3D11ShaderResourceView* normalMap, float shift, const float* lookupMatrix = nullptr, float lookupDepth = 0)
			{
				ComPtr<ID3D11VertexShader> vs = CompileVertexShader(lab, ShaderVertexShader(reflection.Get(), toPlane, tangent, binormal, normal, shift, lookupMatrix, lookupDepth));
				lab.context->VSSetShader(vs.Get(), nullptr, 0);
				lab.context->PSSetShaderResources(1, 1, &normalMap);
				return Render(lab, shader, 3);
			};

			const std::vector<float> plain = render(original.Get(), heightTop.Get(), 0);
			const Compared flat = Compare(plain, render(flatShader.Get(), heightBottom.Get(), 0));
			const Compared top = Compare(plain, render(deepShader.Get(), heightTop.Get(), 0));
			const std::vector<float> deep = render(deepShader.Get(), heightBottom.Get(), 0);
			const Compared shifted = Compare(render(original.Get(), heightBottom.Get(), depthUv), deep);
			// The same picture with the wrong shift, opposite way: if this matched too, "shifted" would prove nothing.
			const Compared wrongWay = Compare(render(original.Get(), heightBottom.Get(), -depthUv), deep);
			worstFlat = (std::max)(worstFlat, flat.worst);
			worstTop = (std::max)(worstTop, top.worst);
			worstShifted = (std::max)(worstShifted, shifted.worst);
			smallestWrongWay = (std::min)(smallestWrongWay, wrongWay.worst);

			// Everything on, on a surface with no relief: there's nothing to shadow, so it has to change nothing.
			const Compared allOn = Compare(plain, render(allOnShader.Get(), heightTop.Get(), 0));
			worstAllOn = (std::max)(worstAllOn, allOn.worst);

			// Everything on, at the bottom of the depth, with the placed lights off: the shadow lookup has to come from
			// where the ray really went. Rendering the original with both moved must give the same picture.
			Compared lookup, lookupOff;
			if (features.movedLookup)
			{
				ID3D11Buffer* quietBuffer = quietObject.Get();
				lab.context->PSSetConstantBuffers(3, 1, &quietBuffer);
				const std::vector<float> expected = render(original.Get(), heightBottom.Get(), depthUv, &scene[48 * 4], depthUv * 400);
				const std::vector<float> got = render(allOnShader.Get(), heightBottom.Get(), 0);
				const std::vector<float> stayed = render(original.Get(), heightBottom.Get(), depthUv);
				lookup = Compare(expected, got);
				lookupOff = Compare(stayed, got);
				ID3D11Buffer* objectBuffer = lab.buffers[3].Get();
				lab.context->PSSetConstantBuffers(3, 1, &objectBuffer);
				worstLookup = (std::max)(worstLookup, lookup.worst);
				movedShows += lookupOff.worst > 0.01f;
			}

			const float tolerance = 2e-3f;
			if (flat.worst > tolerance || top.worst > tolerance || shifted.worst > 4e-3f || wrongWay.worst < 0.01f || flat.nonFinite || top.nonFinite ||
				shifted.nonFinite || allOn.worst > tolerance || lookup.worst > 4e-3f)
			{
				if (failed < 12)
					printf("%s: all on %.5f, moved lookup %.5f (not moving it %.5f)\n", path.c_str(), allOn.worst, lookup.worst, lookupOff.worst);
				++failed;
				if (failed < 12)
					printf("%s: depth 0 %.5f, height 1 %.5f, shifted %.5f, wrong way %.5f, non-finite %d/%d/%d\n", path.c_str(), flat.worst, top.worst,
						shifted.worst, wrongWay.worst, flat.nonFinite, top.nonFinite, shifted.nonFinite);
			}
		}
		printf("%d shaders, %d failed, %d refused by the runtime. Worst difference: depth 0 %.6f, height 1 %.6f, shifted %.6f; smallest with the wrong shift %.4f\n",
			total, failed, unaccepted, worstFlat, worstTop, worstShifted, smallestWrongWay);
		printf("  all on with no relief: %.6f. Moved lookup: %.6f, in %d shaders; in %d of them leaving it out shows, so the check can fail there\n", worstAllOn, worstLookup, moved,
			movedShows);
		printf("  %d shaders with the sun or a primary light, %d with the placed spot, %d with the omnis\n", lit, spots, omnis);
		for (const auto& count : sampleCounts)
			printf("  %d patches moved %u samples\n", count.second, count.first);
		return failed ? 1 : 0;
	}

	// ------------------------------------------------------------------------------------------------ --picture and --bake

	struct Dds
	{
		UINT width = 0, height = 0, mips = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		std::vector<uint8_t> data;
		size_t offset = 0;
	};

	Dds ReadDds(const std::string& path)
	{
		Dds dds;
		dds.data = ReadFile(path);
		if (dds.data.size() < 148 || memcmp(dds.data.data(), "DDS ", 4) != 0)
		{
			printf("not a DDS: %s\n", path.c_str());
			exit(1);
		}
		const auto u32 = [&](size_t at)
		{
			uint32_t v;
			memcpy(&v, dds.data.data() + at, 4);
			return v;
		};
		dds.height = u32(12);
		dds.width = u32(16);
		dds.mips = (std::max)(u32(28), 1u);
		dds.offset = 128;
		if (memcmp(dds.data.data() + 84, "DX10", 4) == 0)
		{
			dds.format = DXGI_FORMAT(u32(128));
			dds.offset = 148;
		}
		else if (memcmp(dds.data.data() + 84, "DXT1", 4) == 0)
			dds.format = DXGI_FORMAT_BC1_UNORM;
		else if (memcmp(dds.data.data() + 84, "DXT5", 4) == 0)
			dds.format = DXGI_FORMAT_BC3_UNORM;
		else if (memcmp(dds.data.data() + 84, "ATI2", 4) == 0)
			dds.format = DXGI_FORMAT_BC5_UNORM;
		return dds;
	}

	ComPtr<ID3D11ShaderResourceView> MakeDdsTexture(ID3D11Device* device, const Dds& dds, bool srgb)
	{
		const UINT block = dds.format == DXGI_FORMAT_BC1_UNORM ? 8 : 16;
		D3D11_TEXTURE2D_DESC d{};
		d.Width = dds.width;
		d.Height = dds.height;
		d.MipLevels = dds.mips;
		d.ArraySize = 1;
		d.Format = srgb && dds.format == DXGI_FORMAT_BC3_UNORM ? DXGI_FORMAT_BC3_UNORM_SRGB : srgb && dds.format == DXGI_FORMAT_BC1_UNORM ? DXGI_FORMAT_BC1_UNORM_SRGB : dds.format;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_IMMUTABLE;
		d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		std::vector<D3D11_SUBRESOURCE_DATA> data(dds.mips);
		size_t at = dds.offset;
		UINT width = dds.width, height = dds.height;
		for (UINT i = 0; i < dds.mips; ++i)
		{
			const UINT wide = (std::max)((width + 3) / 4, 1u), high = (std::max)((height + 3) / 4, 1u);
			data[i] = {dds.data.data() + at, wide * block, 0};
			at += size_t(wide) * high * block;
			width = (std::max)(width / 2, 1u);
			height = (std::max)(height / 2, 1u);
		}
		ComPtr<ID3D11Texture2D> texture;
		Check(device->CreateTexture2D(&d, data.data(), &texture), "dds texture");
		ComPtr<ID3D11ShaderResourceView> view;
		Check(device->CreateShaderResourceView(texture.Get(), nullptr, &view), "dds srv");
		return view;
	}

	void WritePpm(const std::string& path, const std::vector<float>& pixels, UINT width, UINT height)
	{
		std::ofstream out(path, std::ios::binary);
		out << "P6\n" << width << " " << height << "\n255\n";
		std::vector<uint8_t> row(size_t(width) * 3);
		for (UINT y = 0; y < height; ++y)
		{
			for (UINT x = 0; x < width; ++x)
			{
				for (int c = 0; c < 3; ++c)
					row[size_t(x) * 3 + c] = uint8_t((std::min)(pixels[(size_t(y) * width + x) * 4 + c], 1.0f) * 255.0f + 0.5f);
			}
			out.write(reinterpret_cast<const char*>(row.data()), row.size());
		}
	}

	std::vector<Bake::Pixels> DecodeDds(const Dds& dds)
	{
		std::vector<Bake::Pixels> levels;
		size_t at = dds.offset;
		UINT width = dds.width, height = dds.height;
		const bool bc1 = dds.format == DXGI_FORMAT_BC1_UNORM;
		for (UINT i = 0; i < dds.mips; ++i)
		{
			const UINT wide = (std::max)((width + 3) / 4, 1u), high = (std::max)((height + 3) / 4, 1u);
			const UINT pitch = wide * (bc1 ? 8 : 16);
			Bake::Pixels level;
			if (!Decode::Level(dds.format, width, height, dds.data.data() + at, pitch, level))
			{
				printf("format %d isn't decoded\n", int(dds.format));
				exit(1);
			}
			levels.push_back(std::move(level));
			at += size_t(pitch) * high;
			width = (std::max)(width / 2, 1u);
			height = (std::max)(height / 2, 1u);
		}
		return levels;
	}

	// Decode::Read on a real device: a BC texture out of the GPU has to equal the file, and a big mip the streamer hasn't
	// filled yet has to be left out, not baked as an empty surface.
	int ReadTest(const std::string& ddsPath, bool hardware)
	{
		Lab lab;
		CreateLab(lab, 4, 4, hardware);
		const Dds dds = ReadDds(ddsPath);
		const std::vector<Bake::Pixels> expected = DecodeDds(dds);
		auto view = MakeDdsTexture(lab.device.Get(), dds, false);
		std::vector<Bake::Pixels> read;
		std::string error;
		int failed = 0;
		if (!Decode::Read(lab.context.Get(), view.Get(), read, error) || read.size() != expected.size())
		{
			printf("read: %s, %zu mips of %zu WRONG\n", error.c_str(), read.size(), expected.size());
			++failed;
		}
		else
		{
			bool same = true;
			for (size_t i = 0; i < read.size(); ++i)
				same &= read[i].width == expected[i].width && read[i].height == expected[i].height && read[i].rgba == expected[i].rgba;
			failed += !same;
			printf("read back through the GPU: %zu mips, %ux%u, %s\n", read.size(), read[0].width, read[0].height, same ? "same as the file ok" : "DIFFERENT WRONG");
		}

		const UINT size = 16;
		D3D11_TEXTURE2D_DESC d{};
		d.Width = d.Height = size;
		d.MipLevels = 3;
		d.ArraySize = 1;
		d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		d.SampleDesc.Count = 1;
		d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		std::vector<uint32_t> blank(size * size, 0xFF000000u), level1(64, 0xFF804020u), level2(16, 0xFF804020u);
		const D3D11_SUBRESOURCE_DATA data[3] = {{blank.data(), size * 4, 0}, {level1.data(), 32, 0}, {level2.data(), 16, 0}};
		ComPtr<ID3D11Texture2D> partial;
		Check(lab.device->CreateTexture2D(&d, data, &partial), "partial");
		ComPtr<ID3D11ShaderResourceView> partialView;
		Check(lab.device->CreateShaderResourceView(partial.Get(), nullptr, &partialView), "partial view");
		const bool ok = Decode::Read(lab.context.Get(), partialView.Get(), read, error) && read.size() == 2 && read[0].width == 8;
		failed += !ok;
		printf("a texture with its top mip not streamed in yet: starts at %ux%u %s\n", read.empty() ? 0 : read[0].width, read.empty() ? 0 : read[0].height, ok ? "ok" : "WRONG");
		return failed ? 1 : 0;
	}

	struct BakeCase
	{
		const char* normal;
		const char* color;
		float depth, gap, blur, flattenTexels, radius, soften, tops, bottoms;
		bool plain;
		float grain;
	};

	// The same textures and settings as TARGETS in tools/bake_pom.py, whose results these are compared with.
	int BakeTest(const std::vector<std::string>& folders, const std::string& reference)
	{
		const BakeCase cases[] = {
			{"rock_cobble_stone_pak_n", "~~-grock_cobble_stone_pak_c-r~1934443d", 0.017f, 0.75f, 0.7f, 26, 6, 0.35f, 92, 3, false, 0},
			{"eb_art_floor_brickpath_dry_n", "~~-geb_art_floor_brickpath_dr~d4ba7479", 0.015f, 0.8f, 0.8f, 40, 6, 0.35f, 92, 3, false, 0},
			{"stone_wall_blocks_metal_accent_n", "~-gstone_wall_blocks_metal_accent_c", 0.022f, 0.8f, 0.8f, 40, 6, 0.35f, 92, 3, false, 0},
			{"t6_wood_panel_n", "~~-gt6_wood_panel_c-rgb&~-rt6~f2d3e628", 0.03f, 0.7f, 0.4f, 60, 2, 0.35f, 92, 3, false, 0},
			{"ma_tile_wood_01_nml", "~~-gma_tile_wood_01_col-rgb&~~c3478d68", 0.02f, 1, 0.35f, 40, 1.5f, 0.35f, 92, 3, true, 0.12f},
			{"wood_panel_off_white_n", "~~-gwood_panel_off_white_c-rg~23f1d40b", 0.02f, 1, 0.4f, 60, 2, 0.35f, 92, 3, true, 0},
			{"us_art_wall_woodplank_clean_n", "~~-gus_art_wall_woodplank_cle~1b722b33", 0.025f, 1, 0.4f, 60, 2, 0.35f, 92, 3, true, 0.1f},
			{"tree_oak_trunk_nml", "~-gtree_oak_trunk_col", 0.016f, 0.25f, 0.6f, 60, 3, 0.9f, 92, 3, false, 0},
			{"p6_tree_palm_date_bark_n", "~-gp6_tree_palm_date_bark_c", 0.016f, 0.25f, 0.6f, 60, 3, 0.9f, 92, 3, false, 0},
			{"mtl_t6_wpn_pistol_fiveseven_n", "~-gmtl_t6_wpn_pistol_fiveseven_c", 0.012f, 0, 0.5f, 24, 6, 0.35f, 50, 0.5f, false, 0},
		};
		const auto find = [&](const std::string& name)
		{
			for (const std::string& folder : folders)
			{
				const std::string path = folder + "\\" + name + ".dds";
				if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES)
					return path;
			}
			printf("no %s\n", name.c_str());
			exit(1);
			return std::string();
		};

		int failed = 0;
		for (const BakeCase& c : cases)
		{
			const std::vector<Bake::Pixels> normal = DecodeDds(ReadDds(find(c.normal)));
			const std::vector<Bake::Pixels> color = DecodeDds(ReadDds(find(c.color)));
			const float scale = normal[0].width / 256.0f;
			Bake::Recipe recipe;
			recipe.depth = c.depth;
			recipe.gap = c.gap;
			recipe.blur = c.blur;
			recipe.flatten = c.flattenTexels / scale;
			recipe.radius = c.radius;
			recipe.soften = c.soften;
			recipe.tops = c.tops;
			recipe.bottoms = c.bottoms;
			recipe.plain = c.plain;
			recipe.grain = c.grain;

			PomFile ours, theirs;
			std::string error;
			const auto start = std::chrono::steady_clock::now();
			if (!Bake::Build(normal, color[0], recipe, ours, error))
			{
				printf("%s: %s\n", c.normal, error.c_str());
				++failed;
				continue;
			}
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			if (!ReadPomFile(reference + "\\" + c.normal + ".pom", theirs, error) || ours.pixels.size() != theirs.pixels.size())
			{
				printf("%s: reference: %s, %zu bytes against %zu\n", c.normal, error.c_str(), theirs.pixels.size(), ours.pixels.size());
				++failed;
				continue;
			}
			const size_t texels = size_t(ours.header.width) * ours.header.height;
			double blueSum = 0;
			int blueWorst = 0, rgWorst = 0;
			for (size_t i = 0; i < ours.pixels.size() / 4; ++i)
			{
				rgWorst = (std::max)(rgWorst, (std::max)(std::abs(ours.pixels[i * 4] - theirs.pixels[i * 4]), std::abs(ours.pixels[i * 4 + 1] - theirs.pixels[i * 4 + 1])));
				if (i < texels)
				{
					const int difference = std::abs(ours.pixels[i * 4 + 2] - theirs.pixels[i * 4 + 2]);
					blueSum += difference;
					blueWorst = (std::max)(blueWorst, difference);
				}
			}
			const double blueMean = blueSum / double(texels);
			// BC decoding can differ by one in rounding; height is rounded after a lot of floating point.
			const bool ok = rgWorst <= 1 && blueMean < 1.0 && blueWorst <= 12;
			failed += !ok;
			printf("%-36s %4ux%-4u %3zu mips  normal off by at most %d, height off by %.3f on average, %d at worst (of 255), %4.0f ms %s\n", c.normal,
				ours.header.width, ours.header.height, size_t(ours.header.mips), rgWorst, blueMean, blueWorst, ms, ok ? "ok" : "WRONG");
		}
		return failed ? 1 : 0;
	}

	// ---------------------------------------------------------------------------------------------------- --pick

	struct Quad
	{
		float left, top, right, bottom, depth;
		ID3D11ShaderResourceView* color;
		ID3D11ShaderResourceView* normal;
		bool blended;
		int post = 0;  // Not geometry: a fullscreen pass after the scene. 1 with depth off, 2 with no depth buffer bound.
	};

	struct PickScene
	{
		Lab lab;
		ComPtr<ID3D11Texture2D> depthTexture;
		ComPtr<ID3D11DepthStencilView> depth;
		ComPtr<ID3D11DepthStencilState> depthState, noDepth;
		ComPtr<ID3D11BlendState> glass, unseen;
		ComPtr<ID3D11VertexShader> vs;
		ComPtr<ID3D11PixelShader> ps;
		ComPtr<ID3D11Buffer> constants;
		ComPtr<ID3D11RasterizerState> raster;
		ComPtr<ID3D11SamplerState> point;
		std::vector<Quad> quads;

		// Draws everything, handing each draw to `after`. Like the game: state set per draw, and left where it ends up.
		template <typename After>
		std::vector<float> Frame(After after)
		{
			ID3D11DeviceContext* context = lab.context.Get();
			const float clear[4] = {0, 0, 0, 1};
			context->ClearRenderTargetView(lab.rtv.Get(), clear);
			context->ClearDepthStencilView(depth.Get(), D3D11_CLEAR_DEPTH, 1, 0);
			context->OMSetRenderTargets(1, lab.rtv.GetAddressOf(), depth.Get());
			context->OMSetDepthStencilState(depthState.Get(), 0);
			context->RSSetState(raster.Get());
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
			context->VSSetShader(vs.Get(), nullptr, 0);
			context->PSSetShader(ps.Get(), nullptr, 0);
			context->VSSetConstantBuffers(0, 1, constants.GetAddressOf());
			context->PSSetSamplers(0, 1, point.GetAddressOf());
			for (const Quad& quad : quads)
			{
				const float values[8] = {quad.left, quad.top, quad.right, quad.bottom, quad.depth, 0, 0, 0};
				context->UpdateSubresource(constants.Get(), 0, nullptr, values, 0, 0);
				ID3D11ShaderResourceView* views[2] = {quad.color, quad.normal};
				context->PSSetShaderResources(0, 2, views);
				context->OMSetBlendState(quad.post ? unseen.Get() : quad.blended ? glass.Get() : nullptr, nullptr, 0xFFFFFFFF);
				context->OMSetDepthStencilState(quad.post ? noDepth.Get() : depthState.Get(), 0);
				context->OMSetRenderTargets(1, lab.rtv.GetAddressOf(), quad.post == 2 ? nullptr : depth.Get());
				context->Draw(4, 0);
				after([&] { context->Draw(4, 0); });
			}
			context->CopyResource(lab.readback.Get(), lab.target.Get());
			D3D11_MAPPED_SUBRESOURCE map{};
			Check(context->Map(lab.readback.Get(), 0, D3D11_MAP_READ, 0, &map), "map");
			std::vector<float> pixels(size_t(lab.width) * lab.height * 4);
			for (UINT y = 0; y < lab.height; ++y)
				memcpy(pixels.data() + size_t(y) * lab.width * 4, static_cast<uint8_t*>(map.pData) + y * map.RowPitch, lab.width * 16);
			context->Unmap(lab.readback.Get(), 0);
			return pixels;
		}
	};

	int PickTest(bool hardware)
	{
		PickScene scene;
		Lab& lab = scene.lab;
		CreateLab(lab, 64, 64, hardware);
		ID3D11Device* device = lab.device.Get();
		ID3D11DeviceContext* context = lab.context.Get();

		D3D11_TEXTURE2D_DESC dd{};
		dd.Width = dd.Height = 64;
		dd.MipLevels = dd.ArraySize = 1;
		dd.Format = DXGI_FORMAT_D32_FLOAT;
		dd.SampleDesc.Count = 1;
		dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		Check(device->CreateTexture2D(&dd, nullptr, &scene.depthTexture), "depth");
		Check(device->CreateDepthStencilView(scene.depthTexture.Get(), nullptr, &scene.depth), "dsv");
		D3D11_DEPTH_STENCIL_DESC ds{};
		ds.DepthEnable = TRUE;
		ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		ds.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		Check(device->CreateDepthStencilState(&ds, &scene.depthState), "depth state");
		D3D11_BLEND_DESC bd{};
		bd.RenderTarget[0].BlendEnable = TRUE;
		bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
		bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		Check(device->CreateBlendState(&bd, &scene.glass), "glass");
		// Writes nothing, so the staged image stays readable; what matters to the picker is that blending is off.
		D3D11_BLEND_DESC ud{};
		ud.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
		ud.RenderTarget[0].DestBlend = D3D11_BLEND_ZERO;
		ud.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		ud.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
		ud.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
		ud.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		Check(device->CreateBlendState(&ud, &scene.unseen), "unseen");
		D3D11_DEPTH_STENCIL_DESC nd{};
		nd.DepthEnable = FALSE;
		nd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		nd.DepthFunc = D3D11_COMPARISON_ALWAYS;
		Check(device->CreateDepthStencilState(&nd, &scene.noDepth), "no depth");
		D3D11_RASTERIZER_DESC rd{};
		rd.FillMode = D3D11_FILL_SOLID;
		rd.CullMode = D3D11_CULL_NONE;
		rd.DepthClipEnable = TRUE;
		rd.ScissorEnable = FALSE;
		Check(device->CreateRasterizerState(&rd, &scene.raster), "raster");
		D3D11_SAMPLER_DESC sd{};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		Check(device->CreateSamplerState(&sd, &scene.point), "point sampler");
		D3D11_BUFFER_DESC b{};
		b.ByteWidth = 32;
		b.Usage = D3D11_USAGE_DEFAULT;
		b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		Check(device->CreateBuffer(&b, nullptr, &scene.constants), "constants");

		scene.vs = CompileVertexShader(lab,
			"cbuffer C : register(b0) { float4 rect; float z; };\n"
			"struct Out { float2 uv : TEXCOORD0; float4 pos : SV_Position; };\n"
			"Out main(uint id : SV_VertexID) { Out o; float2 c = float2(id & 1, id >> 1); o.uv = c;\n"
			" float2 p = lerp(rect.xy, rect.zw, c); o.pos = float4(p.x * 2 - 1, 1 - p.y * 2, z, 1); return o; }\n");
		const char pixelSource[] =
			"Texture2D colorMap : register(t0); Texture2D normalMap : register(t1); SamplerState s : register(s0);\n"
			"float4 main(float2 uv : TEXCOORD0) : SV_Target { float4 c = colorMap.Sample(s, uv); clip(c.a - 0.1); return float4(c.rgb + normalMap.Sample(s, uv).rgb * 0.1, c.a); }\n";
		ComPtr<ID3DBlob> code, errors;
		if (FAILED(D3DCompile(pixelSource, sizeof(pixelSource) - 1, nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &code, &errors)))
		{
			printf("%s\n", errors ? static_cast<char*>(errors->GetBufferPointer()) : "?");
			return 1;
		}
		Check(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &scene.ps), "ps");

		const auto solid = [&](uint32_t abgr) { return MakeTexture(device, 1, 1, &abgr); };
		auto colorA = solid(0xFF000040), colorB = solid(0xFF004000), colorC = solid(0xFF400000), colorE = solid(0xFF404040), colorG = solid(0x60FFFFFF);
		auto colorF = solid(0xFF008080);
		auto normalA = solid(0xFF808001), normalB = solid(0xFF808002), normalC = solid(0xFF808003), normalE = solid(0xFF808004), normalG = solid(0xFF808005);
		// Solid on the right half, a hole on the left: what an alpha-tested fence or leaf is.
		const uint32_t fence[2] = {0x00404040, 0xFF400000};
		auto colorFence = MakeTexture(device, 2, 1, fence);

		const uintptr_t MATERIAL_A = 0xA0, MATERIAL_B = 0xB0, MATERIAL_C = 0xC0, MATERIAL_F = 0xF0, MATERIAL_G = 0x60;
		auto table = std::make_shared<Picker::Table>();
		table->surfaces[Picker::Table::Key(colorA.Get(), normalA.Get())] = MATERIAL_A;
		table->surfaces[Picker::Table::Key(colorB.Get(), normalB.Get())] = MATERIAL_B;
		table->surfaces[Picker::Table::Key(colorFence.Get(), normalC.Get())] = MATERIAL_C;
		table->surfaces[Picker::Table::Key(colorG.Get(), normalG.Get())] = MATERIAL_G;
		table->surfaces[Picker::Table::Key(colorF.Get(), nullptr)] = MATERIAL_F;

		// Back to front, so nothing but depth order decides. E isn't in the table; F has no normal map of its own.
		scene.quads = {
			{0, 0, 1, 1, 0.8f, colorA.Get(), normalA.Get(), false},
			{0.25f, 0.25f, 0.75f, 1, 0.5f, colorB.Get(), normalB.Get(), false},
			{0.25f, 0, 0.75f, 0.5f, 0.3f, colorFence.Get(), normalC.Get(), false},
			{0, 0.75f, 0.2f, 1, 0.3f, colorE.Get(), normalE.Get(), false},
			{0.8f, 0.75f, 1, 1, 0.3f, colorF.Get(), normalE.Get(), false},
			{0, 0, 1, 0.5f, 0.1f, colorG.Get(), normalG.Get(), true},
			// What ends every real frame: passes covering the whole screen, unknown to the table and not blended.
			{0, 0, 1, 1, 0, colorE.Get(), normalE.Get(), false, 1},
			{0, 0, 1, 1, 0, colorE.Get(), normalE.Get(), false, 2},
		};

		Picker::Core core;
		std::string error;
		if (!core.Init(device, error))
		{
			printf("%s\n", error.c_str());
			return 1;
		}
		core.SetTable(table);
		core.SetScreenSize(64, 64);

		const auto after = [&](auto replay) { core.AfterDraw(context, replay); };
		const std::vector<float> clean = scene.Frame([](auto) {});

		struct Case
		{
			const char* what;
			float u, v;
			uintptr_t want;
		};
		const Case cases[] = {
			{"the far surface, under glass", 0.1f, 0.2f, MATERIAL_A},
			{"the far surface, in the open", 0.1f, 0.6f, MATERIAL_A},
			{"a surface in front of it", 0.5f, 0.8f, MATERIAL_B},
			{"the solid half of a fence", 0.6f, 0.4f, MATERIAL_C},
			{"through the hole in the fence", 0.35f, 0.4f, MATERIAL_B},
			{"something unknown in front", 0.1f, 0.9f, 0},
			{"a surface with no normal map", 0.9f, 0.9f, MATERIAL_F},
		};
		int failed = 0;
		for (const Case& c : cases)
		{
			core.RequestPick(c.u, c.v);
			Picker::Hit hit;
			bool done = false;
			bool frameChanged = false;
			for (int frame = 0; frame < 20 && !done; ++frame)
			{
				done = core.EndFrame(context, hit);
				if (!done)
				{
					const std::vector<float> pixels = scene.Frame(after);
					frameChanged |= Compare(clean, pixels).worst != 0;
				}
			}
			const bool ok = done && hit.material == c.want && !frameChanged;
			failed += !ok;
			printf("%-34s got %#x, want %#x%s %s\n", c.what, unsigned(hit.material), unsigned(c.want), frameChanged ? ", FRAME CHANGED" : "", ok ? "ok" : "WRONG");
			if (hit.color)
				hit.color->Release();
			if (hit.normal)
				hit.normal->Release();
		}

		// Highlighting: only the selected surface's own visible pixels get brighter.
		core.SetHighlight(normalB.Get());
		const std::vector<float> marked = scene.Frame(after);
		core.SetHighlight(nullptr);
		int brighter = 0, otherChanged = 0;
		for (int y = 0; y < 64; ++y)
		{
			for (int x = 0; x < 64; ++x)
			{
				const size_t at = (size_t(y) * 64 + x) * 4;
				const bool changed = marked[at] != clean[at] || marked[at + 1] != clean[at + 1] || marked[at + 2] != clean[at + 2];
				const bool onB = x >= 16 && x < 48 && y >= 16;
				const bool hiddenByFence = y < 32 && x >= 32;
				const bool visible = onB && !hiddenByFence;
				brighter += changed && visible;
				otherChanged += changed && !visible;
			}
		}
		const int want = 32 * 48 - 16 * 16;
		const bool highlightOk = brighter == want && !otherChanged;
		failed += !highlightOk;
		printf("highlight: %d of %d pixels of the selected surface, %d anywhere else %s\n", brighter, want, otherChanged, highlightOk ? "ok" : "WRONG");

		const std::vector<float> after2 = scene.Frame(after);
		const bool restored = Compare(clean, after2).worst == 0;
		failed += !restored;
		printf("highlight off: the frame is back to the original %s\n", restored ? "ok" : "WRONG");
		core.Release();
		return failed ? 1 : 0;
	}

	double TimeMs(Lab& lab, ID3D11PixelShader* shader, UINT vertices)
	{
		D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
		ComPtr<ID3D11Query> disjoint, start, stop;
		Check(lab.device->CreateQuery(&q, &disjoint), "query");
		q.Query = D3D11_QUERY_TIMESTAMP;
		Check(lab.device->CreateQuery(&q, &start), "query");
		Check(lab.device->CreateQuery(&q, &stop), "query");
		double best = 1e9;
		for (int attempt = 0; attempt < 12; ++attempt)
		{
			lab.context->Begin(disjoint.Get());
			lab.context->PSSetShader(shader, nullptr, 0);
			lab.context->End(start.Get());
			lab.context->Draw(vertices, 0);
			lab.context->End(stop.Get());
			lab.context->End(disjoint.Get());
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT info{};
			UINT64 a = 0, b = 0;
			while (lab.context->GetData(disjoint.Get(), &info, sizeof(info), 0) == S_FALSE) {}
			while (lab.context->GetData(start.Get(), &a, sizeof(a), 0) == S_FALSE) {}
			while (lab.context->GetData(stop.Get(), &b, sizeof(b), 0) == S_FALSE) {}
			if (!info.Disjoint)
				best = (std::min)(best, double(b - a) * 1000.0 / double(info.Frequency));
		}
		return best;
	}

	int Picture(const std::vector<std::string>& args, bool hardware)
	{
		const std::string psPath = args[0], colorPath = args[1], normalPath = args[2], pomPath = args[3], prefix = args[4];
		const bool wall = args.size() > 5 && args[5] == "wall";
		const float depthScale = args.size() > 6 ? float(atof(args[6].c_str())) : 1.0f;
		const std::string mode = args.size() > 7 ? args[7] : "shadows";
		const bool crevices = mode == "shadows" || mode == "crevices";
		const bool shadows = mode == "shadows" || mode == "relief";
		const bool pbr = mode == "pbr";
		const float sunHeight = args.size() > 8 ? float(atof(args[8].c_str())) : 0.62f;

		const UINT width = 1280, height = 720;
		Lab lab;
		CreateLab(lab, width, height, hardware);

		PomFile pom;
		std::string error;
		if (!ReadPomFile(pomPath, pom, error))
		{
			printf("%s: %s\n", pomPath.c_str(), error.c_str());
			return 1;
		}
		const std::vector<uint8_t> code = ReadFile(psPath);
		ComPtr<ID3D11PixelShader> original, patched;
		Check(lab.device->CreatePixelShader(code.data(), code.size(), nullptr, &original), "original");
		Dxbc::Literals literals;
		literals.depthUv = pom.header.depthUv * depthScale;
		literals.textureWidth = float(pom.header.width);
		literals.fadeEnd = 1100;
		literals.fadeScale = 1.0f / 400;
		literals.traceSteps = 32;
		literals.crevice = crevices ? 0.6f : 0;
		literals.reliefShadows = shadows ? 1.0f : 0;
		if (!Bake::HasShading(pom))
			Bake::AddShading(pom);
		const Dxbc::Result result = Dxbc::Patch(code.data(), code.size(), literals);
		if (!result.ok)
		{
			printf("patch: %s\n", result.error.c_str());
			return 1;
		}
		printf("patched: light %u, moved lookup %d, placed spot %d, placed omnis %d, %u samples\n", result.light, result.movedLookup, result.spotBlock, result.omniBlock, result.samples);
		const Brdf::Result brdf = Brdf::Patch(code.data(), code.size());
		if (pbr)
			Check(lab.device->CreatePixelShader(brdf.shader.data(), brdf.shader.size(), nullptr, &patched), "pbr");
		else
			Check(lab.device->CreatePixelShader(result.shader.data(), result.shader.size(), nullptr, &patched), "patched");

		const Dds colorDds = ReadDds(colorPath), normalDds = ReadDds(normalPath);
		auto color = MakeDdsTexture(lab.device.Get(), colorDds, true);
		auto nativeNormal = MakeDdsTexture(lab.device.Get(), normalDds, false);
		auto pomNormal = MakeTexture(lab.device.Get(), pom.header.width, pom.header.height, reinterpret_cast<const uint32_t*>(pom.pixels.data()), pom.header.mips);

		// Ambient, then sun, then direction, one texel each: the shader reads three rows of the lightmap.
		const uint32_t lightmap[3] = {0xFF5A5650u, 0xFF40403Cu, 0xFFC08080u};
		D3D11_TEXTURE2D_DESC ld{};
		ld.Width = 1;
		ld.Height = 3;
		ld.MipLevels = ld.ArraySize = 1;
		ld.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		ld.SampleDesc.Count = 1;
		ld.Usage = D3D11_USAGE_IMMUTABLE;
		ld.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA ldata{lightmap, 4, 0};
		ComPtr<ID3D11Texture2D> lightmapTexture;
		Check(lab.device->CreateTexture2D(&ld, &ldata, &lightmapTexture), "lightmap");
		ComPtr<ID3D11ShaderResourceView> lightmapView;
		Check(lab.device->CreateShaderResourceView(lightmapTexture.Get(), nullptr, &lightmapView), "lightmap srv");

		// Comparison sampling needs a depth-like format: an RGBA8 one reads as fully in shadow, and the sun goes out.
		const float nothingNear = 0;
		D3D11_TEXTURE2D_DESC sd{};
		sd.Width = sd.Height = 1;
		sd.MipLevels = sd.ArraySize = 1;
		sd.Format = DXGI_FORMAT_R32_FLOAT;
		sd.SampleDesc.Count = 1;
		sd.Usage = D3D11_USAGE_IMMUTABLE;
		sd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA shadowData{&nothingNear, 4, 0};
		ComPtr<ID3D11Texture2D> shadowTexture;
		Check(lab.device->CreateTexture2D(&sd, &shadowData, &shadowTexture), "shadow");
		ComPtr<ID3D11ShaderResourceView> noShadow;
		Check(lab.device->CreateShaderResourceView(shadowTexture.Get(), nullptr, &noShadow), "shadow view");
		std::vector<uint32_t> black(6, 0xFF0A0A0Au);
		D3D11_TEXTURE2D_DESC cube{};
		cube.Width = cube.Height = 1;
		cube.MipLevels = 1;
		cube.ArraySize = 6;
		cube.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		cube.SampleDesc.Count = 1;
		cube.Usage = D3D11_USAGE_IMMUTABLE;
		cube.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		cube.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
		D3D11_SUBRESOURCE_DATA cubeData[6];
		for (int i = 0; i < 6; ++i)
			cubeData[i] = {black.data() + i, 4, 0};
		ComPtr<ID3D11Texture2D> cubeTexture;
		Check(lab.device->CreateTexture2D(&cube, cubeData, &cubeTexture), "cube");
		ComPtr<ID3D11ShaderResourceView> cubeView;
		Check(lab.device->CreateShaderResourceView(cubeTexture.Get(), nullptr, &cubeView), "cube srv");

		std::vector<float> constants(CB_BYTES / 4, 0.0f);
		const Vec3 sun = pbr ? Normalize({-0.05f, 0.9f, sunHeight}) : Normalize({-0.6f, 0.5f, sunHeight});
		memcpy(&constants[18 * 4], &sun, 12);
		constants[19 * 4] = 3.6f;
		constants[19 * 4 + 1] = 3.3f;
		constants[19 * 4 + 2] = 2.9f;
		constants[20 * 4] = 1.0f;
		lab.context->UpdateSubresource(lab.buffers[0].Get(), 0, nullptr, constants.data(), 0, 0);
		lab.context->PSSetConstantBuffers(0, 1, lab.buffers[0].GetAddressOf());

		ID3D11ShaderResourceView* colorView = color.Get();
		lab.context->PSSetShaderResources(0, 1, &colorView);
		lab.context->PSSetShaderResources(9, 1, noShadow.GetAddressOf());
		lab.context->PSSetShaderResources(13, 1, lightmapView.GetAddressOf());
		lab.context->PSSetShaderResources(15, 1, cubeView.GetAddressOf());
		lab.context->PSSetSamplers(0, 1, lab.aniso.GetAddressOf());
		lab.context->PSSetSamplers(1, 1, lab.aniso.GetAddressOf());
		lab.context->PSSetSamplers(9, 1, lab.compare.GetAddressOf());
		lab.context->PSSetSamplers(13, 1, lab.sampler.GetAddressOf());
		lab.context->PSSetSamplers(15, 1, lab.sampler.GetAddressOf());

		const Vec3 eye = wall ? Vec3{-150, -170, 55} : Vec3{0, 0, 62};
		const Vec3 look = wall ? Normalize({0.7f, 0.7f, 0.02f}) : Normalize({0.0f, 1.0f, -0.42f});
		const Vec3 right = Normalize(Cross(look, {0, 0, 1}));
		const Vec3 up = Cross(right, look);
		const float focal = 1.5f;
		const float tile = 128;
		const Vec3 axisU = wall ? Vec3{-1, 0, 0} : Vec3{1, 0, 0};
		const Vec3 axisV = wall ? Vec3{0, 0, -1} : Vec3{0, 1, 0};
		const Vec3 normal = Cross(axisV, axisU) * -1.0f;
		const Vec3 origin = {0, 0, 0};

		std::ostringstream vs;
		vs.precision(9);
		vs << "struct Out { float4 pos : SV_Position; float4 color : COLOR0; float4 uv : TEXCOORD0; float4 n : TEXCOORD1; float3 fog : TEXCOORD8; float3 b : TEXCOORD2; float3 t : TEXCOORD3;"
		   << " float4 shadow : TEXCOORD4; float3 p : TEXCOORD5; float3 tint : TEXCOORD9; };\n"
		   << "Out main(uint id : SV_VertexID) {\n"
		   << " float2 corners[6] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(-1,1), float2(1,-1), float2(1,1) };\n"
		   << " float2 c = corners[id] * 3000; float3 p = " << VectorText(origin) << " + c.x * " << VectorText(axisU) << " + c.y * " << VectorText(axisV) << " - " << VectorText(eye) << ";\n"
		   << " Out o; o.color = 1; o.uv = float4(c / " << tile << ", 0.5, 0.5); o.n = float4(" << VectorText(normal) << ", 1); o.fog = 0; o.b = " << VectorText(axisV) << "; o.t = " << VectorText(axisU) << ";\n"
		   << " o.shadow = float4(0.5, 0.5, 0.5, 0.5); o.p = p; o.tint = 1;\n"
		   << " float w = dot(p, " << VectorText(look) << ");\n"
		   << " o.pos = float4(dot(p, " << VectorText(right) << ") * " << focal << " * 9 / 16, dot(p, " << VectorText(up) << ") * " << focal << ", 0.5 * w, w); return o; }\n";
		ComPtr<ID3D11VertexShader> vertexShader = CompileVertexShader(lab, vs.str());
		lab.context->VSSetShader(vertexShader.Get(), nullptr, 0);

		ID3D11ShaderResourceView* normalView = nativeNormal.Get();
		lab.context->PSSetShaderResources(1, 1, &normalView);
		const std::vector<float> before = Render(lab, original.Get(), 6);
		const double beforeMs = TimeMs(lab, original.Get(), 6);
		normalView = pbr ? nativeNormal.Get() : pomNormal.Get();
		lab.context->PSSetShaderResources(1, 1, &normalView);
		const std::vector<float> after = Render(lab, patched.Get(), 6);
		const double afterMs = TimeMs(lab, patched.Get(), 6);

		WritePpm(prefix + "_native.ppm", before, width, height);
		WritePpm(prefix + "_pom.ppm", after, width, height);
		printf("%s: depth %.4f uv (%.2f units on a %.0f unit tile), %.2f ms -> %.2f ms for this plane at %ux%u\n", prefix.c_str(), literals.depthUv,
			literals.depthUv * tile, tile, beforeMs, afterMs, width, height);
		return 0;
	}
}

int main(int argc, char** argv)
{
	std::vector<std::string> args;
	bool hardware = false;
	int limit = 0;
	for (int i = 1; i < argc; ++i)
	{
		const std::string arg = argv[i];
		if (arg == "--hardware")
			hardware = true;
		else if (arg == "--limit" && i + 1 < argc)
			limit = atoi(argv[++i]);
		else
			args.push_back(arg);
	}
	if (args.size() >= 1 && args[0] == "--ray")
		return RayTest(hardware) ? 0 : 1;
	if (args.size() >= 2 && args[0] == "--shaders")
		return ShaderTest(args[1], hardware, limit);
	if (args.size() >= 6 && args[0] == "--picture")
		return Picture(std::vector<std::string>(args.begin() + 1, args.end()), hardware);
	if (args.size() >= 3 && args[0] == "--bake")
		return BakeTest(std::vector<std::string>(args.begin() + 2, args.end()), args[1]);
	if (args.size() >= 1 && args[0] == "--pick")
		return PickTest(hardware);
	if (args.size() >= 1 && args[0] == "--shadow")
		return ShadowTest(hardware) ? 0 : 1;
	if (args.size() >= 2 && args[0] == "--read")
		return ReadTest(args[1], hardware);
	puts("pom_test --ray | --shadow | --pick | --shaders <list> | --picture <ps.cso> <color.dds> <normal.dds> <.pom> <out prefix> [floor|wall] [depth scale]\n"
		 "         --bake <folder of reference .pom> <folders of .dds> | --read <.dds>   [--hardware] [--limit n]");
	return 2;
}
