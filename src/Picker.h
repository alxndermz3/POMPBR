#pragma once
#include <d3d11.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Finds what's under the cursor and marks what's selected, on the GPU and with the game's own draws, so it needs no idea
// where anything is in the world and works the same on map geometry, models and players.
//
// To pick, every draw of one frame runs a second time, clipped to the one pixel under the cursor, writing no color, in an
// occlusion query. The game's own shaders and depth test decide: the last draw that reaches that pixel is what you see
// there, and a hole in an alpha-tested surface lets the click through. Blended draws are skipped, so glass and decals
// don't get in the way, and so is anything not tested against the scene's depth, like the passes that end a frame. What's
// bound at t0 and t1 then says which material it was (Table).
//
// To highlight, a draw whose normal map is the selected one runs a second time with a flat additive shader.
namespace Pom::Picker
{
	// Which material a draw is, by the views bound as its color map (t0) and normal map (t1): lit shaders all bind them
	// there. Stale bindings can't fool this, since a draw is only known if both belong to one material. Materials without
	// a normal map are in by their color map alone, with 0 for the normal map.
	struct Table
	{
		std::unordered_map<uint64_t, uintptr_t> surfaces;
		static uint64_t Key(const void* color, const void* normal)
		{
			return (uint64_t(reinterpret_cast<uintptr_t>(color)) << 32) | reinterpret_cast<uintptr_t>(normal);
		}
	};

	struct Hit
	{
		uintptr_t material = 0;  // 0: nothing known there.
		ID3D11ShaderResourceView* color = nullptr;  // Each with a reference for whoever takes the hit.
		ID3D11ShaderResourceView* normal = nullptr;
	};

	// What the last pick saw, for the log: if a click finds nothing, this says where it got lost.
	struct Report
	{
		unsigned draws = 0;      // Draws in the view.
		unsigned solid = 0;      // Those that counted.
		unsigned known = 0;      // Those whose material the table knows.
		unsigned reached = 0;    // Those that reached the pixel.
		unsigned tableSize = 0;  // Surfaces in the table.
	};

	class Core
	{
	public:
		bool Init(ID3D11Device* device, std::string& error);
		void Release();

		void SetTable(std::shared_ptr<const Table> table);
		void SetScreenSize(UINT width, UINT height);

		// The view of the normal map to mark, or null. Takes its own reference.
		void SetHighlight(ID3D11ShaderResourceView* normal);
		void SetHighlightColor(float r, float g, float b);

		// u and v run 0..1 over the screen. The next whole frame does the work.
		void RequestPick(float u, float v);

		// At the end of a frame, on the render thread. True once a pick finished, with `out` filled in.
		bool EndFrame(ID3D11DeviceContext* context, Hit& out);
		const Report& LastReport() const { return report_; }

		// After each draw of the game. `replay` issues that draw again.
		template <typename Replay>
		void AfterDraw(ID3D11DeviceContext* context, Replay replay)
		{
			if (!picking_ && !highlight_)
				return;
			Bindings bound;
			if (!ReadBindings(context, bound))
				return;
			if (picking_ && BeginPickDraw(context, bound))
			{
				replay();
				EndPickDraw(context, bound);
			}
			if (highlight_ && bound.normal == highlight_)
			{
				BeginHighlightDraw(context, bound);
				replay();
				EndHighlightDraw(context, bound);
			}
			ReleaseBindings(bound);
		}

	private:
		struct Bindings
		{
			ID3D11ShaderResourceView* color = nullptr;
			ID3D11ShaderResourceView* normal = nullptr;
			ID3D11BlendState* blend = nullptr;
			FLOAT blendFactor[4] = {};
			UINT sampleMask = 0;
			ID3D11RasterizerState* raster = nullptr;
			D3D11_RECT scissors[16] = {};
			UINT scissorCount = 0;
			ID3D11PixelShader* shader = nullptr;
			D3D11_VIEWPORT viewport = {};
		};

		struct Record
		{
			ID3D11ShaderResourceView* color;
			ID3D11ShaderResourceView* normal;
			uintptr_t material;
		};

		enum class Pick
		{
			Idle,
			Requested,
			Drawing,
			Waiting,
		};

		bool ReadBindings(ID3D11DeviceContext* context, Bindings& bound);
		void ReleaseBindings(Bindings& bound);
		bool BeginPickDraw(ID3D11DeviceContext* context, Bindings& bound);
		void EndPickDraw(ID3D11DeviceContext* context, Bindings& bound);
		void BeginHighlightDraw(ID3D11DeviceContext* context, Bindings& bound);
		void EndHighlightDraw(ID3D11DeviceContext* context, Bindings& bound);
		ID3D11RasterizerState* ScissoredRaster(ID3D11RasterizerState* original);
		void ClearRecords();

		ID3D11Device* device_ = nullptr;
		ID3D11BlendState* noColor_ = nullptr;
		ID3D11BlendState* additive_ = nullptr;
		ID3D11PixelShader* flat_ = nullptr;
		std::unordered_map<ID3D11RasterizerState*, ID3D11RasterizerState*> scissored_;

		std::shared_ptr<const Table> table_;
		ID3D11ShaderResourceView* highlight_ = nullptr;
		float highlightColor_[3] = {0.28f, 0.17f, 0.02f};

		bool picking_ = false;
		Pick pick_ = Pick::Idle;
		float pickU_ = 0, pickV_ = 0;
		UINT screenWidth_ = 0, screenHeight_ = 0;
		std::vector<ID3D11Query*> queries_;
		std::vector<Record> records_;
		Report report_;
	};
}
