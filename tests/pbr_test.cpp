// Checks the highlights and reflections patch without the game, on a real D3D11 device, through the game's own shaders.
//
//   pbr_test.exe --brdf <lit sun shader .cso> [spec mapped lit sun shader .cso] [--hardware]
//   pbr_test.exe --primary <lit primary light shader .cso> [--hardware]
//   pbr_test.exe --omni <spec mapped twin .cso> [--hardware]
//     What the shaders really output against the formulas. The native shader has to match BO2's own formula first: that's
//     what shows this measures the right thing. Then the patched one has to match GGX and Smith, for many glosses and
//     angles, and show the same peak. --primary is for the shaders that do things in the other order: V before the power.
//     --omni is the highlight the game never gave omni lights: on a black surface the native shader shows nothing at all.
//
//   pbr_test.exe --capture [--hardware]
//     Reflect.cpp on a staged frame, in the formats the game uses: the scene opens at a patched shader, what's bound there is
//     copied when the targets change, the next frame reads it at t16 and t17, an extra camera doesn't disturb it.
//
//   pbr_test.exe --mirror <lit sun shader .cso> <out prefix> [--hardware]
//     Screen reflections in a staged scene: a mirror floor and a wall with its height and side written in its color. A
//     reflection has to show the point of the wall geometry says it should, nothing where the wall ends, and the rest has
//     to be left to the probe.
//
//   pbr_test.exe --all <list of .cso> [--hardware] [--limit n]
//     Every shader: it patches, the runtime takes it, POM's patch takes the result, it stays finite under random input,
//     and with nothing specular in the scene (no sun, no probe, no screen, no placed lights) it renders what the original
//     renders: only highlights and reflections may change.

#include <Windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "../src/Brdf.h"
#include "../src/Dxbc.h"
#include "../src/Reflect.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxguid.lib")

using Microsoft::WRL::ComPtr;
using namespace Pom;

namespace
{
	constexpr UINT CB_BYTES = 4096 * 16;
	constexpr float PI = 3.14159265f;

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

	std::string Text(Vec3 v)
	{
		std::ostringstream s;
		s.precision(9);
		s << "float3(" << v.x << "," << v.y << "," << v.z << ")";
		return s.str();
	}

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

	struct Lab
	{
		ComPtr<ID3D11Device> device;
		ComPtr<ID3D11DeviceContext> context;
		ComPtr<ID3D11Texture2D> target, readback;
		ComPtr<ID3D11RenderTargetView> rtv;
		ComPtr<ID3D11Buffer> scene, material, object, pbr;
		ComPtr<ID3D11SamplerState> sampler, compare;
		UINT width = 0, height = 0;
	};

	ComPtr<ID3D11Buffer> MakeBuffer(ID3D11Device* device, UINT bytes)
	{
		D3D11_BUFFER_DESC b{};
		b.ByteWidth = bytes;
		b.Usage = D3D11_USAGE_DEFAULT;
		b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		ComPtr<ID3D11Buffer> buffer;
		Check(device->CreateBuffer(&b, nullptr, &buffer), "cb");
		return buffer;
	}

	void CreateLab(Lab& lab, UINT width, UINT height, bool hardware)
	{
		lab.width = width;
		lab.height = height;
		const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
		Check(D3D11CreateDevice(nullptr, hardware ? D3D_DRIVER_TYPE_HARDWARE : D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &lab.device, nullptr, &lab.context),
			"device");
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

		lab.scene = MakeBuffer(lab.device.Get(), CB_BYTES);
		lab.material = MakeBuffer(lab.device.Get(), CB_BYTES);
		lab.object = MakeBuffer(lab.device.Get(), CB_BYTES);
		lab.pbr = MakeBuffer(lab.device.Get(), 256);
		ID3D11Buffer* buffers[4] = {lab.scene.Get(), lab.material.Get(), lab.material.Get(), lab.object.Get()};
		lab.context->PSSetConstantBuffers(0, 4, buffers);
		lab.context->PSSetConstantBuffers(Brdf::CONSTANT_SLOT, 1, lab.pbr.GetAddressOf());

		D3D11_SAMPLER_DESC sd{};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		Check(lab.device->CreateSamplerState(&sd, &lab.sampler), "sampler");
		sd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
		sd.ComparisonFunc = D3D11_COMPARISON_GREATER;
		Check(lab.device->CreateSamplerState(&sd, &lab.compare), "compare sampler");
		for (UINT slot = 0; slot < 16; ++slot)
			lab.context->PSSetSamplers(slot, 1, lab.sampler.GetAddressOf());

		D3D11_RASTERIZER_DESC raster{};
		raster.FillMode = D3D11_FILL_SOLID;
		raster.CullMode = D3D11_CULL_NONE;
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

	ComPtr<ID3D11ShaderResourceView> MakeTexture(Lab& lab, UINT width, UINT height, const uint32_t* pixels)
	{
		D3D11_TEXTURE2D_DESC d{};
		d.Width = width;
		d.Height = height;
		d.MipLevels = d.ArraySize = 1;
		d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_IMMUTABLE;
		d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA data{pixels, width * 4, 0};
		ComPtr<ID3D11Texture2D> texture;
		Check(lab.device->CreateTexture2D(&d, &data, &texture), "texture");
		ComPtr<ID3D11ShaderResourceView> view;
		Check(lab.device->CreateShaderResourceView(texture.Get(), nullptr, &view), "srv");
		return view;
	}

	ComPtr<ID3D11ShaderResourceView> MakeFloatTexture(Lab& lab, UINT width, UINT height, DXGI_FORMAT format, const void* pixels, UINT pitch)
	{
		D3D11_TEXTURE2D_DESC d{};
		d.Width = width;
		d.Height = height;
		d.MipLevels = d.ArraySize = 1;
		d.Format = format;
		d.SampleDesc.Count = 1;
		d.Usage = D3D11_USAGE_IMMUTABLE;
		d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA data{pixels, pitch, 0};
		ComPtr<ID3D11Texture2D> texture;
		Check(lab.device->CreateTexture2D(&d, &data, &texture), "float texture");
		ComPtr<ID3D11ShaderResourceView> view;
		Check(lab.device->CreateShaderResourceView(texture.Get(), nullptr, &view), "float srv");
		return view;
	}

	ComPtr<ID3D11ShaderResourceView> MakeCube(Lab& lab, uint32_t color)
	{
		const uint32_t faces[6] = {color, color, color, color, color, color};
		D3D11_TEXTURE2D_DESC cube{};
		cube.Width = cube.Height = 1;
		cube.MipLevels = 1;
		cube.ArraySize = 6;
		cube.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		cube.SampleDesc.Count = 1;
		cube.Usage = D3D11_USAGE_IMMUTABLE;
		cube.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		cube.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
		D3D11_SUBRESOURCE_DATA data[6];
		for (int i = 0; i < 6; ++i)
			data[i] = {faces + i, 4, 0};
		ComPtr<ID3D11Texture2D> texture;
		Check(lab.device->CreateTexture2D(&cube, data, &texture), "cube");
		ComPtr<ID3D11ShaderResourceView> view;
		Check(lab.device->CreateShaderResourceView(texture.Get(), nullptr, &view), "cube srv");
		return view;
	}

	// What's in the way of a sun: a shadow map nothing's on, and a lightmap whose alpha lets it through.
	struct Stage
	{
		ComPtr<ID3D11ShaderResourceView> shadow, lightmap, cube;
	};

	void SetStage(Lab& lab, Stage& stage, uint32_t cubeColor)
	{
		const float nothingNear = 0;
		stage.shadow = MakeFloatTexture(lab, 1, 1, DXGI_FORMAT_R32_FLOAT, &nothingNear, 4);
		const uint32_t lightmap[3] = {0xFF000000u, 0xFF000000u, 0xFFC08080u};
		stage.lightmap = MakeTexture(lab, 1, 3, lightmap);
		stage.cube = MakeCube(lab, cubeColor);
		lab.context->PSSetShaderResources(9, 1, stage.shadow.GetAddressOf());
		lab.context->PSSetShaderResources(10, 1, stage.shadow.GetAddressOf());
		lab.context->PSSetShaderResources(13, 1, stage.lightmap.GetAddressOf());
		lab.context->PSSetShaderResources(15, 1, stage.cube.GetAddressOf());
		lab.context->PSSetSamplers(9, 1, lab.compare.GetAddressOf());
		lab.context->PSSetSamplers(10, 1, lab.compare.GetAddressOf());
	}

	void SetPbr(Lab& lab, const Brdf::Constants& constants)
	{
		uint8_t bytes[256] = {};
		memcpy(bytes, &constants, sizeof(constants));
		lab.context->UpdateSubresource(lab.pbr.Get(), 0, nullptr, bytes, 0, 0);
	}

	// A full-screen triangle sending what the shader asks for, in the order it asks for it. `value` gives the expression
	// for a TEXCOORD, as a float4, or empty for the rest.
	template <typename Value>
	std::string VertexShaderFor(ID3D11ShaderReflection* reflection, const std::string& setup, const std::string& position, Value value)
	{
		D3D11_SHADER_DESC desc{};
		reflection->GetDesc(&desc);
		std::ostringstream body, fields;
		bool hasPosition = false;
		int n = 0;
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
			const std::string expression = value(std::string(p.SemanticName) == "TEXCOORD" ? int(p.SemanticIndex) : -1, n);
			body << "o.f" << n << " = (" << expression << ")." << swizzle << ";\n";
			++n;
		}
		if (!hasPosition)
			fields << "float4 pos : SV_Position;\n";
		return "struct Out {\n" + fields.str() + "};\nOut main(uint id : SV_VertexID) { Out o; float2 uv = float2((id << 1) & 2, id & 2);\n" + setup + "\no.pos = " + position + ";\n" + body.str() +
			"return o; }\n";
	}

	const std::string FULL_SCREEN = "float4(uv * float2(2, -2) + float2(-1, 1), 0, 1)";

	void SetNormalMaps(Lab& lab, ComPtr<ID3D11ShaderResourceView> maps[2])
	{
		const uint32_t flat = 0xFFFF8080u;
		maps[1] = MakeTexture(lab, 1, 1, &flat);
		lab.context->PSSetShaderResources(1, 1, maps[1].GetAddressOf());
	}

	// ----------------------------------------------------------------------------------------------------------- --brdf

	struct Sample
	{
		float gloss;
		Vec3 view, light;
	};

	float Saturate(float x)
	{
		return (std::min)((std::max)(x, 0.0f), 1.0f);
	}

	// N = z. What BO2 computes: returns the light reflected, per channel of F0.
	float NativeSpecular(const Sample& s, float mirror)
	{
		const Vec3 half = Normalize(s.view + s.light);
		const float NoH = Saturate(half.z), NoL = Saturate(s.light.z), LoH = Saturate(Dot(half, s.light));
		const float power = std::pow(2.0f, 13 * s.gloss);
		const float D = std::pow(NoH, power) * (power * 0.125f + 0.25f);
		const float a = Saturate(s.gloss + 0.545f);
		const float V = NoL / (a * LoH * LoH + 1.01f - a);
		const float F = mirror + (1 - mirror) * std::pow(2.0f, -10 * LoH);
		return D * V * F;
	}

	// GGX, Smith with Schlick-GGX, the same F.
	float PbrSpecular(const Sample& s, float mirror)
	{
		const Vec3 half = Normalize(s.view + s.light);
		const float NoH = Saturate(half.z), NoL = Saturate(s.light.z), NoV = (std::max)(Saturate(s.view.z), 0.03f), LoH = Saturate(Dot(half, s.light));
		const float power = std::pow(2.0f, 13 * s.gloss);
		const float area = (std::max)(2 / (power + 2), 2.0f / 8194);
		const float k = std::sqrt(area) * 0.5f;
		const float cosine2 = NoH * NoH;
		const float d = cosine2 * (area - 1) + 1;
		// alpha^2 / (pi * d^2), with the pi and the 4 taken out the way the game's constants have them.
		const float D = area / (4 * d * d);
		const float V = NoL / ((NoL * (1 - k) + k) * (NoV * (1 - k) + k));
		const float F = mirror + (1 - mirror) * std::pow(2.0f, -10 * LoH);
		return D * V * F;
	}

	Vec3 Around(Vec3 axis, float angle, Vec3 towards)
	{
		const Vec3 side = Normalize(Cross(axis, towards));
		const Vec3 other = Cross(side, axis);
		return Normalize(axis * std::cos(angle) + other * std::sin(angle));
	}

	struct Measured
	{
		float native, patched;
	};

	enum class Light
	{
		Sun,
		Primary,
		Omni,
	};

	int BrdfTest(const std::string& path, bool specMap, Light kind, bool hardware)
	{
		const bool primary = kind == Light::Primary;
		const bool omni = kind == Light::Omni;
		Lab lab;
		CreateLab(lab, 4, 4, hardware);
		Stage stage;
		SetStage(lab, stage, 0xFF000000u);  // A probe with nothing in it.
		ComPtr<ID3D11ShaderResourceView> maps[2];
		SetNormalMaps(lab, maps);

		const std::vector<uint8_t> code = ReadFile(path);
		ComPtr<ID3D11ShaderReflection> reflection;
		Check(D3DReflect(code.data(), code.size(), IID_PPV_ARGS(&reflection)), "reflect");
		const Brdf::Result result = Brdf::Patch(code.data(), code.size());
		if (!result.ok || !result.changed)
		{
			printf("%s: not patched: %s\n", path.c_str(), result.error.c_str());
			return 1;
		}
		printf("%s: %u lobes, %u visibility, widened %d, probe %d, screen %d, omni %d\n", path.c_str(), result.lobes, result.visibilities, result.widened, result.probe, result.screen, result.omni);

		ComPtr<ID3D11PixelShader> native, patched;
		Check(lab.device->CreatePixelShader(code.data(), code.size(), nullptr, &native), "native");
		Check(lab.device->CreatePixelShader(result.shader.data(), result.shader.size(), nullptr, &patched), "patched");
		SetPbr(lab, {});

		std::vector<float> constants(CB_BYTES / 4, 0.0f);
		constants[19 * 4] = constants[19 * 4 + 1] = constants[19 * 4 + 2] = 1;
		constants[20 * 4] = 1;
		if (primary)
		{
			// Every attenuation term of a primary light at 1: a huge range, and a cookie of plain white.
			constants[19 * 4] = constants[19 * 4 + 1] = constants[19 * 4 + 2] = 0;
			constants[78 * 4] = constants[78 * 4 + 1] = constants[78 * 4 + 2] = 1;
			constants[79 * 4 + 3] = 1e9f;
			for (int c = 0; c < 4; ++c)
				constants[83 * 4 + c] = 1;
			constants[86 * 4 + 3] = 1;
			const uint32_t white = 0xFFFFFFFFu;
			static ComPtr<ID3D11ShaderResourceView> cookie;
			cookie = MakeTexture(lab, 1, 1, &white);
			lab.context->PSSetShaderResources(12, 1, cookie.GetAddressOf());
		}

		if (omni)
		{
			constants[19 * 4] = constants[19 * 4 + 1] = constants[19 * 4 + 2] = 0;
			constants[64 * 4] = constants[65 * 4] = constants[66 * 4] = 1;  // Only the first of the four, white, with no falloff.
			std::vector<uint32_t> flags(CB_BYTES / 4, 0);
			flags[11 * 4 + 1] = 1;
			lab.context->UpdateSubresource(lab.object.Get(), 0, nullptr, flags.data(), 0, 0);
		}

		const float mirror = specMap ? 0.25f : 0.04f;
		const uint32_t specByte = uint32_t(std::lround(std::sqrt(mirror) * 255));
		float worstNative = 0, worstPatched = 0;
		int samples = 0;
		printf("  angle from the mirror direction, then what's reflected, native -> patched, at gloss 0.6:\n");
		std::map<int, Measured> tail;
		for (const float wantedGloss : {0.25f, 0.45f, 0.6f, 0.75f, 0.95f})
		{
			const uint32_t glossByte = uint32_t(std::lround(wantedGloss * 255));
			const float gloss = glossByte / 255.0f;
			const uint32_t materialPixel = (glossByte << 24);
			auto color = MakeTexture(lab, 1, 1, &materialPixel);
			lab.context->PSSetShaderResources(0, 1, color.GetAddressOf());
			// Materials keep gloss in the color's alpha, the normal map's, or the spec map's: all of them say the same here.
			const uint32_t normalPixel = (glossByte << 24) | 0x00FF8080u;
			auto normal = MakeTexture(lab, 1, 1, &normalPixel);
			lab.context->PSSetShaderResources(1, 1, normal.GetAddressOf());
			if (specMap)
			{
				const uint32_t specPixel = (glossByte << 24) | (specByte << 16) | (specByte << 8) | specByte;
				auto spec = MakeTexture(lab, 1, 1, &specPixel);
				lab.context->PSSetShaderResources(2, 1, spec.GetAddressOf());
			}
			for (const float viewAngle : {5.0f, 30.0f, 60.0f, 80.0f})
			{
				const Vec3 view = Normalize({std::sin(viewAngle * PI / 180), 0, std::cos(viewAngle * PI / 180)});
				const Vec3 mirrored = {-view.x, -view.y, view.z};
				for (const int degrees : {0, 1, 2, 4, 8, 15, 30})
				{
					const Vec3 light = Around(mirrored, degrees * PI / 180, Vec3{0.3f, 1, 0.2f});
					if (light.z < 0.02f)
						continue;
					memcpy(&constants[18 * 4], &light, 12);
					const Vec3 lightAt = view * -300.0f + light * 40;
					memcpy(&constants[77 * 4], &lightAt, 12);
					constants[60 * 4] = lightAt.x;
					constants[61 * 4] = lightAt.y;
					constants[62 * 4] = lightAt.z;
					lab.context->UpdateSubresource(lab.scene.Get(), 0, nullptr, constants.data(), 0, 0);

					ComPtr<ID3D11VertexShader> vs = CompileVertexShader(lab, VertexShaderFor(reflection.Get(), "", FULL_SCREEN, [&](int texcoord, int)
					{
						switch (texcoord)
						{
						case 0: return std::string("float4(0.5, 0.5, 0.5, 0.5)");
						case 1: return std::string("float4(0, 0, 1, 1)");
						case 2: return std::string("float4(0, 1, 0, 0)");
						case 3: return std::string("float4(1, 0, 0, 0)");
						case 4: return std::string("float4(0.5, 0.5, 0.5, 0.5)");
						case 5: return "float4(" + Text(view * -300.0f) + ", 1)";
						case 8: return std::string("float4(0, 0, 0, 0)");
						default: return std::string("float4(1, 1, 1, 1)");
						}
					}));
					lab.context->VSSetShader(vs.Get(), nullptr, 0);
					const float nativeOut = Render(lab, native.Get(), 3)[0];
					const float patchedOut = Render(lab, patched.Get(), 3)[0];
					const float nativeLight = nativeOut * nativeOut, patchedLight = patchedOut * patchedOut;

					const Sample s{gloss, view, light};
					const float wantedNative = omni ? 0 : NativeSpecular(s, mirror), wantedPatched = PbrSpecular(s, mirror);
					if (omni)
						worstNative = (std::max)(worstNative, nativeLight);
					// Below this it's a rounding error of a value no eye can see.
					// A primary light has terms of its own, the same for both shaders: the native one measures them.
					const float lightStrength = primary && wantedNative > 1e-3f ? nativeLight / wantedNative : 1.0f;
					if (wantedNative > 1e-3f)
						worstNative = (std::max)(worstNative, std::fabs(nativeLight - wantedNative * lightStrength) / (wantedNative * lightStrength));
					if (wantedPatched > 1e-3f && (!primary || wantedNative > 1e-3f))
						worstPatched = (std::max)(worstPatched, std::fabs(patchedLight - wantedPatched * lightStrength) / (wantedPatched * lightStrength));
					++samples;
					if (wantedGloss == 0.6f && viewAngle == 30.0f)
						tail[degrees] = {nativeLight, patchedLight};
				}
			}
		}
		for (const auto& row : tail)
			printf("    %2d deg  %10.5f -> %10.5f\n", row.first, row.second.native, row.second.patched);
		printf("  %d samples. Native against BO2's formula: worst %.3f%%. Patched against GGX + Smith: worst %.3f%%\n", samples, worstNative * 100, worstPatched * 100);
		return worstNative < 0.01f && worstPatched < 0.01f ? 0 : 1;
	}

	// -------------------------------------------------------------------------------------------------------- --capture

	struct CaptureRig
	{
		ComPtr<ID3D11Texture2D> color, depth;
		ComPtr<ID3D11RenderTargetView> rtv;
		ComPtr<ID3D11DepthStencilView> dsv;
	};

	// Like the game's: 8 bits a channel, and a D32S8 that's typeless with a shader resource bind.
	CaptureRig MakeRig(Lab& lab, UINT width, UINT height)
	{
		CaptureRig rig;
		D3D11_TEXTURE2D_DESC d{};
		d.Width = width;
		d.Height = height;
		d.MipLevels = d.ArraySize = 1;
		d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		d.SampleDesc.Count = 1;
		d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		Check(lab.device->CreateTexture2D(&d, nullptr, &rig.color), "rig color");
		Check(lab.device->CreateRenderTargetView(rig.color.Get(), nullptr, &rig.rtv), "rig rtv");
		d.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
		d.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
		Check(lab.device->CreateTexture2D(&d, nullptr, &rig.depth), "rig depth");
		D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
		dv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
		dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
		Check(lab.device->CreateDepthStencilView(rig.depth.Get(), &dv, &rig.dsv), "rig dsv");
		return rig;
	}

	bool Near(float a, float b)
	{
		return std::fabs(a - b) < 0.006f;
	}

	int CaptureTest(bool hardware)
	{
		const UINT width = 64, height = 36;
		Lab lab;
		CreateLab(lab, width, height, hardware);
		CaptureRig scene = MakeRig(lab, width, height), extra = MakeRig(lab, 16, 16);

		// One shader marks the scene, as a patched one does in the game; the other shows what t16 and t17 hold.
		const char* marker = "float4 main() : SV_Target { return float4(0.25, 0.5, 0.75, 1); }";
		const char* reader =
			"Texture2D color : register(t16); Texture2D<float> depth : register(t17);\n"
			"cbuffer Pbr : register(b12) { float a, b, c, screen; float2 size; float steps, reach; };\n"
			"float4 main(float4 p : SV_Position) : SV_Target { return float4(color.Load(int3(p.xy, 0)).r, depth.Load(int3(p.xy, 0)), screen, size.x); }";
		const auto compile = [&](const char* source)
		{
			ComPtr<ID3DBlob> code, errors;
			if (FAILED(D3DCompile(source, strlen(source), nullptr, nullptr, nullptr, "main", "ps_4_0", 0, 0, &code, &errors)))
			{
				printf("%s\n", errors ? static_cast<char*>(errors->GetBufferPointer()) : "?");
				exit(1);
			}
			ComPtr<ID3D11PixelShader> shader;
			Check(lab.device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader), "ps");
			return shader;
		};
		ComPtr<ID3D11PixelShader> markerShader = compile(marker), readerShader = compile(reader), otherShader = compile(marker);
		ComPtr<ID3D11VertexShader> vs = CompileVertexShader(lab, "float4 main(uint id : SV_VertexID) : SV_Position { float2 uv = float2((id << 1) & 2, id & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }");
		lab.context->VSSetShader(vs.Get(), nullptr, 0);

		Reflect::SetShaders(new Reflect::Shaders{markerShader.Get()});
		Brdf::Constants constants;
		constants.screenAmount = 0.5f;
		Reflect::SetConstants(constants);
		Reflect::EndFrame(lab.context.Get(), width, height);

		ID3D11Buffer* noBuffer = nullptr;
		lab.context->PSSetConstantBuffers(Brdf::CONSTANT_SLOT, 1, &noBuffer);

		// Sets things the way the game does, through what the hooks see.
		const auto setTargets = [&](ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv)
		{
			Reflect::BeforeTargets(lab.context.Get(), 1, &rtv, dsv);
			lab.context->OMSetRenderTargets(1, &rtv, dsv);
		};
		const auto setShader = [&](ID3D11PixelShader* shader)
		{
			Reflect::BeforeShader(lab.context.Get(), shader);
			lab.context->PSSetShader(shader, nullptr, 0);
		};
		const auto drawScene = [&](float colorValue, float depthValue)
		{
			setTargets(scene.rtv.Get(), scene.dsv.Get());
			const float clear[4] = {colorValue, 0, 0, 1};
			lab.context->ClearRenderTargetView(scene.rtv.Get(), clear);
			lab.context->ClearDepthStencilView(scene.dsv.Get(), D3D11_CLEAR_DEPTH, depthValue, 0);
			setShader(otherShader.Get());
			setShader(markerShader.Get());
		};
		const auto read = [&]
		{
			lab.context->OMSetRenderTargets(1, lab.rtv.GetAddressOf(), nullptr);
			return Render(lab, readerShader.Get(), 3);
		};
		int failed = 0;
		const auto expect = [&](const char* what, const std::vector<float>& frame, float a, float b, float c, float d)
		{
			const float* p = frame.data();
			const bool good = Near(p[0], a) && Near(p[1], b) && Near(p[2], c) && Near(p[3], d);
			printf("  %-52s %.3f %.3f %.3f %.0f (wanted %.3f %.3f %.3f %.0f) %s\n", what, p[0], p[1], p[2], p[3], a, b, c, d, good ? "ok" : "WRONG");
			failed += !good;
		};

		// First frame: nothing to show yet, but the constants are there. It ends when the post effects change the targets.
		drawScene(0.25f, 0.75f);
		expect("scene open, no frame yet", read(), 0, 0, 0.5f, float(width));
		Reflect::BeforeTargets(lab.context.Get(), 1, lab.rtv.GetAddressOf(), nullptr);

		// An extra camera, small: no scene, and nothing of the last one to read.
		drawScene(0.5f, 0.25f);
		setTargets(extra.rtv.Get(), extra.dsv.Get());
		setShader(markerShader.Get());
		expect("extra camera doesn't read the scene", read(), 0, 0, 0.5f, float(width));
		Reflect::EndFrame(lab.context.Get(), width, height);

		// Next frame: the frame before is what's read, and the last that was copied is the one from the scene.
		drawScene(0.1f, 0.1f);
		expect("second frame reads what the first drew", read(), 0.5f, 0.25f, 0.5f, float(width));
		Reflect::EndFrame(lab.context.Get(), width, height);

		// A live change.
		constants.screenAmount = 0.25f;
		Reflect::SetConstants(constants);
		drawScene(0.75f, 0.5f);
		expect("a slider shows at the next scene", read(), 0.1f, 0.1f, 0.25f, float(width));
		Reflect::EndFrame(lab.context.Get(), width, height);

		// Never leaving its targets: copied at Present.
		drawScene(0.9f, 0.9f);
		expect("copies at the end of the frame if nothing else does", read(), 0.75f, 0.5f, 0.25f, float(width));
		Reflect::Release();
		Reflect::SetShaders(nullptr);
		return failed;
	}

	// --------------------------------------------------------------------------------------------------------- --mirror

	int MirrorTest(const std::string& path, const std::string& prefix, bool hardware)
	{
		const UINT width = 960, height = 540;
		Lab lab;
		CreateLab(lab, width, height, hardware);
		Stage stage;
		SetStage(lab, stage, 0xFF000000u);
		ComPtr<ID3D11ShaderResourceView> maps[2];
		SetNormalMaps(lab, maps);

		const std::vector<uint8_t> code = ReadFile(path);
		ComPtr<ID3D11ShaderReflection> reflection;
		Check(D3DReflect(code.data(), code.size(), IID_PPV_ARGS(&reflection)), "reflect");
		const Brdf::Result result = Brdf::Patch(code.data(), code.size());
		if (!result.screen)
		{
			printf("%s: no screen reflections\n", path.c_str());
			return 1;
		}
		ComPtr<ID3D11PixelShader> patched;
		Check(lab.device->CreatePixelShader(result.shader.data(), result.shader.size(), nullptr, &patched), "patched");

		// Full gloss, no albedo: all that's on screen is what's reflected.
		const uint32_t materialPixel = 0xFF000000u;
		auto color = MakeTexture(lab, 1, 1, &materialPixel);
		lab.context->PSSetShaderResources(0, 1, color.GetAddressOf());

		const Vec3 eye = {0, 0, 60};
		const Vec3 look = Normalize({0.1f, 1, -0.16f});
		const Vec3 right = Normalize(Cross(look, {0, 0, 1}));
		const Vec3 up = Cross(right, look);
		const float focalY = 1.5f, focalX = focalY * height / width;
		const float zNear = 3.9375f;
		const float wallY = 500, wallTop = 170;

		std::vector<float> constants(CB_BYTES / 4, 0.0f);
		constants[20 * 4] = 1;
		const auto setRow = [&](int vector, Vec3 v, float w)
		{
			memcpy(&constants[vector * 4], &v, 12);
			constants[vector * 4 + 3] = w;
		};
		setRow(36, right * focalX, 0);
		setRow(37, up * focalY, 0);
		setRow(38, {0, 0, 0}, zNear);
		setRow(39, look, 0);
		constants[58 * 4] = zNear;
		lab.context->UpdateSubresource(lab.scene.Get(), 0, nullptr, constants.data(), 0, 0);

		// The frame before: depth and color of the wall and the floor, from the same camera.
		const auto rayOf = [&](float x, float y)
		{
			const float ndcX = (x + 0.5f) / width * 2 - 1, ndcY = 1 - (y + 0.5f) / height * 2;
			return look + right * (ndcX / focalX) + up * (ndcY / focalY);
		};
		const auto wallLight = [&](Vec3 at, float out[3])
		{
			out[0] = Saturate(at.z / 200);
			out[1] = 0.5f;
			out[2] = Saturate(0.5f + at.x / 1600);
		};
		std::vector<float> depth(size_t(width) * height, 0.0f), scene(size_t(width) * height * 4, 0.0f);
		for (UINT y = 0; y < height; ++y)
		{
			for (UINT x = 0; x < width; ++x)
			{
				const Vec3 ray = rayOf(float(x), float(y));
				const size_t at = size_t(y) * width + x;
				const float toWall = (wallY - eye.y) / ray.y;
				const Vec3 wallAt = eye + ray * toWall;
				if (ray.y > 0 && wallAt.z >= 0 && wallAt.z <= wallTop)
				{
					depth[at] = zNear / toWall;  // ray has 1 along the look direction, so toWall is w.
					float light[3];
					wallLight(wallAt, light);
					for (int c = 0; c < 3; ++c)
						scene[at * 4 + c] = std::sqrt(light[c]);
				}
				else if (ray.z < 0)
				{
					depth[at] = zNear / (-eye.z / ray.z);
				}
				scene[at * 4 + 3] = 1;
			}
		}
		auto depthView = MakeFloatTexture(lab, width, height, DXGI_FORMAT_R32_FLOAT, depth.data(), width * 4);
		auto colorView = MakeFloatTexture(lab, width, height, DXGI_FORMAT_R32G32B32A32_FLOAT, scene.data(), width * 16);
		lab.context->PSSetShaderResources(Brdf::SCENE_COLOR_SLOT, 1, colorView.GetAddressOf());
		lab.context->PSSetShaderResources(Brdf::SCENE_DEPTH_SLOT, 1, depthView.GetAddressOf());

		Brdf::Constants pbr;
		pbr.screenAmount = 1;
		pbr.screenWidth = float(width);
		pbr.screenHeight = float(height);
		pbr.screenSteps = 96;
		pbr.screenReach = 2400;
		SetPbr(lab, pbr);

		std::ostringstream setup;
		setup.precision(9);
		setup << "float2 corners[6] = { float2(-1,-1), float2(1,-1), float2(-1,1), float2(-1,1), float2(1,-1), float2(1,1) };\n"
			  << "float2 c = corners[id] * 6000; float3 p = float3(c.x, c.y, 0) - " << Text(eye) << ";\n"
			  << "float w = dot(p, " << Text(look) << ");\n";
		const std::string position = "float4(dot(p, " + Text(right * focalX) + "), dot(p, " + Text(up * focalY) + "), " + std::to_string(zNear) + ", w)";
		ComPtr<ID3D11VertexShader> vs = CompileVertexShader(lab, VertexShaderFor(reflection.Get(), setup.str(), position, [&](int texcoord, int)
		{
			switch (texcoord)
			{
			case 0: return std::string("float4(0.5, 0.5, 0.5, 0.5)");
			case 1: return std::string("float4(0, 0, 1, 1)");
			case 2: return std::string("float4(0, 1, 0, 0)");
			case 3: return std::string("float4(1, 0, 0, 0)");
			case 4: return std::string("float4(0.5, 0.5, 0.5, 0.5)");
			case 5: return std::string("float4(p, 1)");
			case 8: return std::string("float4(0, 0, 0, 0)");
			default: return std::string("float4(1, 1, 1, 1)");
			}
		}));
		lab.context->VSSetShader(vs.Get(), nullptr, 0);
		const std::vector<float> frame = Render(lab, patched.Get(), 6);

		D3D11_QUERY_DESC queryDesc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
		ComPtr<ID3D11Query> disjoint, before, after;
		Check(lab.device->CreateQuery(&queryDesc, &disjoint), "query");
		queryDesc.Query = D3D11_QUERY_TIMESTAMP;
		Check(lab.device->CreateQuery(&queryDesc, &before), "query");
		Check(lab.device->CreateQuery(&queryDesc, &after), "query");
		const auto timed = [&](float amount)
		{
			pbr.screenAmount = amount;
			SetPbr(lab, pbr);
			double best = 1e9;
			for (int run = 0; run < 12; ++run)
			{
				lab.context->Begin(disjoint.Get());
				lab.context->End(before.Get());
				lab.context->Draw(6, 0);
				lab.context->End(after.Get());
				lab.context->End(disjoint.Get());
				lab.context->Flush();
				D3D11_QUERY_DATA_TIMESTAMP_DISJOINT rate{};
				while (lab.context->GetData(disjoint.Get(), &rate, sizeof(rate), 0) != S_OK)
					Sleep(0);
				UINT64 start = 0, end = 0;
				while (lab.context->GetData(before.Get(), &start, sizeof(start), 0) != S_OK)
					Sleep(0);
				while (lab.context->GetData(after.Get(), &end, sizeof(end), 0) != S_OK)
					Sleep(0);
				if (!rate.Disjoint && run)
					best = (std::min)(best, double(end - start) * 1000 / double(rate.Frequency));
			}
			return best;
		};
		const double without = timed(0), with = timed(1);
		printf("GPU time for the floor at %ux%u: %.3f ms, with screen reflections at %.0f steps %.3f ms\n", width, height, without, pbr.screenSteps, with);

		int shouldHit = 0, didHit = 0, wrongOverSky = 0;
		double totalHeight = 0, totalSide = 0, worstHeight = 0;
		std::vector<double> errors;
		std::ofstream ppm(prefix + "_mirror.ppm", std::ios::binary);
		ppm << "P6\n" << width << " " << height << "\n255\n";
		for (UINT y = 0; y < height; ++y)
		{
			for (UINT x = 0; x < width; ++x)
			{
				const float* out = &frame[(size_t(y) * width + x) * 4];
				for (int c = 0; c < 3; ++c)
					ppm.put(char(uint8_t(Saturate(out[c] * 2) * 255)));

				const Vec3 ray = rayOf(float(x), float(y));
				if (ray.z >= 0)
					continue;
				const Vec3 floorAt = eye + ray * (-eye.z / ray.z);
				if (floorAt.y >= wallY)
					continue;
				const Vec3 away = {ray.x, ray.y, -ray.z};
				const Vec3 hit = floorAt + away * ((wallY - floorAt.y) / away.y);
				// Where on screen that point of the wall is: it has to be there to be reflected.
				const Vec3 fromEye = hit - eye;
				const float w = Dot(fromEye, look);
				const float screenX = Dot(fromEye, right) * focalX / w, screenY = Dot(fromEye, up) * focalY / w;
				const bool onScreen = std::fabs(screenX) < 0.9f && std::fabs(screenY) < 0.9f;
				const bool onWall = hit.z >= 0 && hit.z <= wallTop;
				const float strength = out[1] * out[1];
				const bool seen = strength > 1e-4f;
				if (onWall && onScreen && hit.z < wallTop - 12)
				{
					++shouldHit;
					if (seen)
					{
						++didHit;
						// Height and side, read out of the reflected color, whatever Fresnel did to its brightness.
						const float height2 = (out[0] * out[0] / (out[1] * out[1])) * 0.5f * 200;
						const float side = ((out[2] * out[2] / (out[1] * out[1])) * 0.5f - 0.5f) * 1600;
						const double error = std::fabs(height2 - hit.z);
						totalHeight += error;
						totalSide += std::fabs(side - hit.x);
						worstHeight = (std::max)(worstHeight, error);
						errors.push_back(error);
					}
				}
				else if (!onWall && hit.z > wallTop + 12 && onScreen)
				{
					wrongOverSky += seen;
				}
			}
		}
		std::sort(errors.begin(), errors.end());
		const double p99 = errors.empty() ? 0 : errors[size_t(errors.size() * 0.99)];
		printf("floor pixels that should reflect the wall: %d, did: %d (%.1f%%). Height off by %.2f units on average, 99%% within %.2f, side by %.2f. Past the top of the wall: %d wrongly reflect\n",
			shouldHit, didHit, 100.0 * didHit / (std::max)(shouldHit, 1), didHit ? totalHeight / didHit : 0, p99, didHit ? totalSide / didHit : 0, wrongOverSky);
		return didHit > shouldHit * 0.95 && p99 < 4 && wrongOverSky < shouldHit / 100 ? 0 : 1;
	}

	// ------------------------------------------------------------------------------------------------------------ --all

	int AllTest(const std::string& list, bool hardware, int limit)
	{
		Lab lab;
		CreateLab(lab, 16, 16, hardware);
		Stage quiet, busy;
		SetStage(lab, busy, 0xFF8090A0u);
		SetStage(lab, quiet, 0xFF000000u);

		std::mt19937 random(1234);
		std::vector<uint32_t> noise(16 * 16);
		for (auto& p : noise)
			p = random();
		auto colors = MakeTexture(lab, 16, 16, noise.data());
		for (auto& p : noise)
			p = (random() & 0xFF00FFFFu) | 0xFF000000u;
		auto normals = MakeTexture(lab, 16, 16, noise.data());

		std::vector<float> depth(16 * 16);
		for (auto& v : depth)
			v = (random() % 1000) / 1000.0f;
		auto shadow = MakeFloatTexture(lab, 16, 16, DXGI_FORMAT_R32_FLOAT, depth.data(), 16 * 4);

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

		// Random constants, then the same with everything specular comes from taken out.
		std::uniform_real_distribution<float> value(0.05f, 0.95f);
		std::vector<float> scene(CB_BYTES / 4), material(CB_BYTES / 4), object(CB_BYTES / 4);
		for (auto* buffer : {&scene, &material, &object})
		{
			for (auto& v : *buffer)
				v = value(random);
		}
		scene[20 * 4] = 1;
		const auto setBits = [](std::vector<float>& buffer, int index, uint32_t bits) { memcpy(&buffer[index], &bits, 4); };
		setBits(object, 11 * 4, 1);
		setBits(object, 11 * 4 + 1, 1);
		std::vector<float> dark = scene, still = object;
		for (const int vector : {19, 68, 78})
		{
			for (int c = 0; c < 3; ++c)
				dark[vector * 4 + c] = 0;
		}
		for (const int vector : {64, 65, 66})
		{
			for (int c = 0; c < 4; ++c)
				dark[vector * 4 + c] = 0;
		}
		setBits(still, 11 * 4, 0);
		setBits(still, 11 * 4 + 1, 0);
		lab.context->UpdateSubresource(lab.material.Get(), 0, nullptr, material.data(), 0, 0);
		SetPbr(lab, {});

		std::ifstream names(list);
		std::string path;
		int total = 0, failed = 0, changed = 0, lobes = 0, smith = 0, widened = 0, probes = 0, screens = 0, omnis = 0, oneLobe = 0, twoLobes = 0, stacked = 0;
		float worstQuiet = 0;
		double patchMs = 0, createMs = 0;
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
			const auto start = std::chrono::steady_clock::now();
			const Brdf::Result result = Brdf::Patch(code.data(), code.size());
			const auto patchedAt = std::chrono::steady_clock::now();
			patchMs += std::chrono::duration<double, std::milli>(patchedAt - start).count();
			if (!result.ok)
			{
				printf("%s: %s\n", path.c_str(), result.error.c_str());
				++failed;
				continue;
			}
			if (!result.changed)
				continue;
			++changed;
			lobes += result.lobes != 0;
			oneLobe += result.lobes == 1;
			twoLobes += result.lobes == 2;
			smith += result.visibilities != 0;
			widened += result.widened;
			probes += result.probe;
			screens += result.screen;
			omnis += result.omni;

			ComPtr<ID3D11PixelShader> original, patched;
			Check(lab.device->CreatePixelShader(code.data(), code.size(), nullptr, &original), "original");
			const auto beforeCreate = std::chrono::steady_clock::now();
			if (FAILED(lab.device->CreatePixelShader(result.shader.data(), result.shader.size(), nullptr, &patched)))
			{
				printf("%s: the runtime refused it\n", path.c_str());
				++failed;
				continue;
			}
			createMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - beforeCreate).count();

			// POM goes on top, on any surface that has both.
			Dxbc::Literals literals;
			literals.fadeEnd = 1e7f;
			literals.fadeScale = 1;
			literals.textureWidth = 16;
			literals.mostSteps = literals.fewestSteps = 16;
			literals.traceSteps = 8;
			literals.reliefShadows = literals.crevice = 1;
			literals.depthUv = 0.03f;
			const Dxbc::Result both = Dxbc::Patch(result.shader.data(), result.shader.size(), literals);
			if (both.ok)
			{
				ComPtr<ID3D11PixelShader> bothShader;
				if (FAILED(lab.device->CreatePixelShader(both.shader.data(), both.shader.size(), nullptr, &bothShader)))
				{
					printf("%s: the runtime refused it with POM\n", path.c_str());
					++failed;
				}
				else
					++stacked;
			}

			ComPtr<ID3D11ShaderReflection> reflection;
			Check(D3DReflect(code.data(), code.size(), IID_PPV_ARGS(&reflection)), "reflect");
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
					ID3D11ShaderResourceView* view = colors.Get();
					if (r.Dimension == D3D_SRV_DIMENSION_TEXTURECUBE)
						view = busy.cube.Get();
					else if (r.Dimension == D3D_SRV_DIMENSION_TEXTURE3D)
						view = volumeView.Get();
					else if (r.BindPoint == 9 || r.BindPoint == 10)
						view = shadow.Get();
					else if (r.BindPoint == 1)
						view = normals.Get();
					lab.context->PSSetShaderResources(r.BindPoint, 1, &view);
				}
			}

			const Vec3 toPlane = Normalize({0.45f, 0.62f, -0.64f}) * 400.0f;
			ComPtr<ID3D11VertexShader> vs = CompileVertexShader(lab, VertexShaderFor(reflection.Get(), "float3 toPixel = " + Text(toPlane) + " + float3(uv * 100, 0);", FULL_SCREEN, [&](int texcoord, int n)
			{
				if (texcoord == 1)
					return std::string("float4(0, 0, 1, 0.8)");
				if (texcoord == 2)
					return std::string("float4(0, 1, 0, 0)");
				if (texcoord == 3)
					return std::string("float4(1, 0, 0, 0)");
				if (texcoord == 5)
					return std::string("float4(toPixel, 1)");
				return "float4(uv.x * 0.8 + 0.1, uv.y * 0.8 + 0.1, 0.7 - uv.x * 0.4, 1) + float4(" + std::to_string(std::sin(n * 1.3f) * 0.1f) + ", " + std::to_string(std::cos(n * 2.1f) * 0.1f) + ", 0, 0)";
			}));
			lab.context->VSSetShader(vs.Get(), nullptr, 0);

			// Lights on: nothing may blow up.
			lab.context->UpdateSubresource(lab.scene.Get(), 0, nullptr, scene.data(), 0, 0);
			lab.context->UpdateSubresource(lab.object.Get(), 0, nullptr, object.data(), 0, 0);
			lab.context->PSSetShaderResources(15, 1, busy.cube.GetAddressOf());
			const std::vector<float> nativeLit = Render(lab, original.Get(), 3), patchedLit = Render(lab, patched.Get(), 3);
			int broken = 0;
			for (size_t i = 0; i < patchedLit.size(); ++i)
				broken += i % 4 != 3 && std::isfinite(nativeLit[i]) && !std::isfinite(patchedLit[i]);

			// Nothing to reflect or highlight: it has to be the original to the last digit.
			lab.context->UpdateSubresource(lab.scene.Get(), 0, nullptr, dark.data(), 0, 0);
			lab.context->UpdateSubresource(lab.object.Get(), 0, nullptr, still.data(), 0, 0);
			lab.context->PSSetShaderResources(15, 1, quiet.cube.GetAddressOf());
			const std::vector<float> nativeQuiet = Render(lab, original.Get(), 3), patchedQuiet = Render(lab, patched.Get(), 3);
			float worst = 0;
			for (size_t i = 0; i < patchedQuiet.size(); ++i)
			{
				if (i % 4 != 3 && std::isfinite(nativeQuiet[i]) && std::isfinite(patchedQuiet[i]))
					worst = (std::max)(worst, std::fabs(nativeQuiet[i] - patchedQuiet[i]) / (1 + std::fabs(nativeQuiet[i])));
			}
			worstQuiet = (std::max)(worstQuiet, worst);
			if (broken || worst > 1e-3f)
			{
				if (failed < 12)
					printf("%s: %d non-finite, %.5f from the original with nothing specular\n", path.c_str(), broken, worst);
				++failed;
			}
		}
		printf("%d shaders, %d changed, %d failed. %d took POM on top. Worst difference with nothing specular: %.6f\n", total, changed, failed, stacked, worstQuiet);
		printf("  GGX in %d (%d one light, %d two), Smith in %d, anti-sparkle in %d, probe %d, screen reflections %d, omni highlights %d\n", lobes, oneLobe, twoLobes, smith, widened, probes, screens,
			omnis);
		printf("  %.3f ms a patch, %.3f ms a CreatePixelShader\n", patchMs / (std::max)(total, 1), createMs / (std::max)(changed, 1));
		return failed ? 1 : 0;
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
	if (args.size() >= 2 && args[0] == "--brdf")
	{
		int failed = BrdfTest(args[1], false, Light::Sun, hardware);
		if (args.size() >= 3)
			failed |= BrdfTest(args[2], true, Light::Sun, hardware);
		return failed;
	}
	if (args.size() >= 2 && args[0] == "--primary")
		return BrdfTest(args[1], false, Light::Primary, hardware);
	if (args.size() >= 2 && args[0] == "--omni")
		return BrdfTest(args[1], true, Light::Omni, hardware);
	if (args.size() >= 1 && args[0] == "--capture")
		return CaptureTest(hardware);
	if (args.size() >= 3 && args[0] == "--mirror")
		return MirrorTest(args[1], args[2], hardware);
	if (args.size() >= 2 && args[0] == "--all")
		return AllTest(args[1], hardware, limit);
	puts("pbr_test --brdf <lit sun ps.cso> [spec mapped one] | --primary <lit primary ps.cso> | --omni <spec mapped twin ps.cso> | --capture | --mirror <lit sun ps.cso> <out prefix> | --all <list> [--hardware] [--limit n]");
	return 2;
}
