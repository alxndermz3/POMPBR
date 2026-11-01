#include "Overlay.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

#include "Engine.h"
#include "Log.h"
#include "MinHook.h"
#include "Panel.h"
#include "Reflect.h"
#include "Settings.h"
#include "Tool.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// The window, cursor and catcher handling follows BotModREvamp's Overlay.cpp, which has the reasons in full.
namespace Pom::Overlay
{
namespace
{
	constexpr UINT_PTR TIMER_ID = 0xB02C;
	constexpr UINT TIMER_MS = 50;
	constexpr UINT TOGGLE_MESSAGE = WM_APP + 0x4D;
	constexpr int CATCHER_UI = 0x10;
	constexpr int MENU_CATCHER = 17;  // Console | UI: what T6SDK's blank menu sets. The game stops taking movement and look.
	constexpr uintptr_t KEY_CATCHERS = 0x11C7844;
	constexpr uintptr_t FN_KEY_SET_CATCHER = 0x514950;
	constexpr uintptr_t FN_KEY_REMOVE_CATCHER = 0x6F6380;
	constexpr uint8_t KEY_SET_CATCHER_BYTES[] = {0x56, 0x8B, 0x74, 0x24, 0x08, 0x83, 0xFE, 0xFF};
	constexpr uint8_t KEY_REMOVE_CATCHER_BYTES[] = {0x8B, 0x44, 0x24, 0x04, 0x83, 0xF8, 0xFF, 0x74};

	constexpr int SWAPCHAIN_PRESENT = 8;
	constexpr int SWAPCHAIN_RESIZE_BUFFERS = 13;
	constexpr int CONTEXT_PS_SET_SHADER = 9;
	constexpr int CONTEXT_OM_SET_RENDER_TARGETS = 33;
	constexpr int CONTEXT_DRAW_INDEXED = 12;
	constexpr int CONTEXT_DRAW = 13;
	constexpr int CONTEXT_DRAW_INDEXED_INSTANCED = 20;
	constexpr int CONTEXT_DRAW_INSTANCED = 21;

	using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
	using ResizeBuffers_t = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
	using SetCursorPos_t = BOOL(WINAPI*)(int, int);
	using ClipCursor_t = BOOL(WINAPI*)(const RECT*);
	using DrawIndexed_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
	using Draw_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
	using DrawIndexedInstanced_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
	using DrawInstanced_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
	using PSSetShader_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
	using OMSetRenderTargets_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
	using KeyCatcher_t = void(__cdecl*)(int localClient, int mask);

	Present_t g_Present = nullptr;
	ResizeBuffers_t g_ResizeBuffers = nullptr;
	SetCursorPos_t g_SetCursorPos = nullptr;
	ClipCursor_t g_ClipCursor = nullptr;
	DrawIndexed_t g_DrawIndexed = nullptr;
	Draw_t g_Draw = nullptr;
	DrawIndexedInstanced_t g_DrawIndexedInstanced = nullptr;
	DrawInstanced_t g_DrawInstanced = nullptr;
	PSSetShader_t g_PSSetShader = nullptr;
	OMSetRenderTargets_t g_OMSetRenderTargets = nullptr;

	// ImGui is fed from the window procedure and drawn from Present, and Win32 calls made inside ImGui can re-enter the
	// window procedure on the same thread.
	std::recursive_mutex g_ImGuiLock;
	thread_local bool t_InsideImGui = false;

	struct ImGuiSection
	{
		std::lock_guard<std::recursive_mutex> lock{g_ImGuiLock};
		ImGuiSection() { t_InsideImGui = true; }
		~ImGuiSection() { t_InsideImGui = false; }
	};

	bool g_Initialized = false;
	bool g_CatchersKnown = false;
	HWND g_Window = nullptr;
	WNDPROC g_OriginalWndProc = nullptr;
	IDXGISwapChain* g_SwapChain = nullptr;
	ID3D11Device* g_Device = nullptr;
	ID3D11DeviceContext* g_Context = nullptr;
	ID3D11RenderTargetView* g_RenderTarget = nullptr;
	bool g_DrawHooks = false;

	std::atomic<bool> g_Open{false};
	std::atomic<bool> g_InScene{false};  // Draws are the game's: not while the overlay draws itself.
	std::atomic<bool> g_WantClose{false};
	bool g_TimerStarted = false;
	bool g_EscapeSwallowed = false;
	int g_SavedCatcher = 0;

	int KeyCatchers()
	{
		return *reinterpret_cast<int*>(KEY_CATCHERS);
	}

	// One call adds bits and the other masks them, so both together set exactly `mask`.
	void SetKeyCatchers(int mask)
	{
		if (!g_CatchersKnown)
			return;
		reinterpret_cast<KeyCatcher_t>(FN_KEY_SET_CATCHER)(0, mask);
		reinterpret_cast<KeyCatcher_t>(FN_KEY_REMOVE_CATCHER)(0, mask);
	}

	// On the window thread.
	void SetOpen(bool open)
	{
		if (open == g_Open)
			return;
		if (open && !Engine::MapIsRunning())
			return;
		g_Open = open;
		if (open)
		{
			g_SavedCatcher = g_CatchersKnown ? KeyCatchers() : 0;
			SetKeyCatchers(MENU_CATCHER);
			if (g_ClipCursor)
				g_ClipCursor(nullptr);  // Let the cursor leave the middle of the screen.
			Tool::Open();
		}
		else
		{
			SetKeyCatchers(g_SavedCatcher);
			if (!t_InsideImGui)
			{
				if (g_Window && GetCapture() == g_Window)
					ReleaseCapture();
				ImGuiSection section;
				if (ImGui::GetCurrentContext())
					ImGui::GetIO().AddFocusEvent(false);
			}
			Tool::Close();
		}
		Log::Write("tool %s", open ? "opened" : "closed");
	}

	void WindowTick()
	{
		if (g_WantClose.exchange(false))
			SetOpen(false);
		if (!g_Open)
			return;
		if (!Engine::MapIsRunning())
		{
			SetOpen(false);
			return;
		}
		// A menu closing or a killcam can reset the catcher.
		if (g_CatchersKnown && !(KeyCatchers() & CATCHER_UI))
			SetKeyCatchers(MENU_CATCHER);
	}

	bool GoesToTheGame(UINT msg)
	{
		switch (msg)
		{
		case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_CHAR: case WM_SYSCHAR:
		case WM_MOUSEMOVE:
		case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
		case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
		case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
		case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
		case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
			return false;
		default:
			return true;
		}
	}

	LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (!g_TimerStarted)
		{
			// SetTimer has to come from the thread that owns the window: this one.
			SetTimer(hwnd, TIMER_ID, TIMER_MS, nullptr);
			g_TimerStarted = true;
		}
		if (msg == WM_TIMER && wParam == TIMER_ID)
		{
			WindowTick();
			return 0;
		}

		const bool toolKey = (msg == WM_KEYDOWN || msg == WM_KEYUP) && int(wParam) == Engine::ToolKey();
		if (toolKey)
		{
			if (msg == WM_KEYDOWN && !(lParam & (1 << 30)) && !(GetKeyState(VK_CONTROL) & 0x8000))
				SetOpen(!g_Open);
			return 0;
		}

		if (!g_Open)
		{
			// The Escape key-up that belongs to the press that closed the tool.
			if (g_EscapeSwallowed && msg == WM_KEYUP && wParam == VK_ESCAPE)
			{
				g_EscapeSwallowed = false;
				return 0;
			}
			return CallWindowProcA(g_OriginalWndProc, hwnd, msg, wParam, lParam);
		}

		if (msg == WM_KEYDOWN && wParam == VK_ESCAPE)
		{
			SetOpen(false);
			g_EscapeSwallowed = true;
			return 0;
		}

		if (!t_InsideImGui)
		{
			ImGuiSection section;
			if (ImGui::GetCurrentContext())
				ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
		}

		if (msg == WM_INPUT)
			return DefWindowProcA(hwnd, msg, wParam, lParam);  // Required cleanup; the game never sees it.
		if (!GoesToTheGame(msg))
			return 0;
		return CallWindowProcA(g_OriginalWndProc, hwnd, msg, wParam, lParam);
	}

	BOOL WINAPI HookedSetCursorPos(int x, int y)
	{
		return g_Open ? TRUE : g_SetCursorPos(x, y);
	}

	BOOL WINAPI HookedClipCursor(const RECT* rect)
	{
		return g_Open ? TRUE : g_ClipCursor(rect);
	}

	template <typename Replay>
	void AfterDraw(ID3D11DeviceContext* context, Replay replay)
	{
		if (g_InScene.load(std::memory_order_relaxed))
			Tool::Picking().AfterDraw(context, replay);
	}

	void STDMETHODCALLTYPE DrawIndexed_Hook(ID3D11DeviceContext* context, UINT indexCount, UINT startIndex, INT baseVertex)
	{
		g_DrawIndexed(context, indexCount, startIndex, baseVertex);
		AfterDraw(context, [&] { g_DrawIndexed(context, indexCount, startIndex, baseVertex); });
	}

	void STDMETHODCALLTYPE Draw_Hook(ID3D11DeviceContext* context, UINT vertexCount, UINT startVertex)
	{
		g_Draw(context, vertexCount, startVertex);
		AfterDraw(context, [&] { g_Draw(context, vertexCount, startVertex); });
	}

	void STDMETHODCALLTYPE DrawIndexedInstanced_Hook(ID3D11DeviceContext* context, UINT indexCount, UINT instanceCount, UINT startIndex, INT baseVertex,
		UINT startInstance)
	{
		g_DrawIndexedInstanced(context, indexCount, instanceCount, startIndex, baseVertex, startInstance);
		AfterDraw(context, [&] { g_DrawIndexedInstanced(context, indexCount, instanceCount, startIndex, baseVertex, startInstance); });
	}

	void STDMETHODCALLTYPE DrawInstanced_Hook(ID3D11DeviceContext* context, UINT vertexCount, UINT instanceCount, UINT startVertex, UINT startInstance)
	{
		g_DrawInstanced(context, vertexCount, instanceCount, startVertex, startInstance);
		AfterDraw(context, [&] { g_DrawInstanced(context, vertexCount, instanceCount, startVertex, startInstance); });
	}

	void STDMETHODCALLTYPE PSSetShader_Hook(ID3D11DeviceContext* context, ID3D11PixelShader* shader, ID3D11ClassInstance* const* instances, UINT count)
	{
		Reflect::BeforeShader(context, shader);
		g_PSSetShader(context, shader, instances, count);
	}

	void STDMETHODCALLTYPE OMSetRenderTargets_Hook(ID3D11DeviceContext* context, UINT count, ID3D11RenderTargetView* const* targets, ID3D11DepthStencilView* depth)
	{
		Reflect::BeforeTargets(context, count, targets, depth);
		g_OMSetRenderTargets(context, count, targets, depth);
	}

	bool Hook(void* target, void* detour, void** original, const char* name)
	{
		MH_STATUS status = MH_CreateHook(target, detour, original);
		if (status == MH_OK)
			status = MH_EnableHook(target);
		if (status != MH_OK)
			Log::Write("hooking %s failed: %s", name, MH_StatusToString(status));
		return status == MH_OK;
	}

	// For screen reflections: where the scene starts and ends. These only look at a flag or a shader, unlike the draw hooks,
	// so they stay in all the time.
	void InstallSceneHooks()
	{
		void** table = *reinterpret_cast<void***>(g_Context);
		const bool ok = Hook(table[CONTEXT_PS_SET_SHADER], reinterpret_cast<void*>(&PSSetShader_Hook), reinterpret_cast<void**>(&g_PSSetShader), "PSSetShader") &&
			Hook(table[CONTEXT_OM_SET_RENDER_TARGETS], reinterpret_cast<void*>(&OMSetRenderTargets_Hook), reinterpret_cast<void**>(&g_OMSetRenderTargets), "OMSetRenderTargets");
		Log::Write(ok ? "scene hooks in" : "scene hooks incomplete: no screen reflections");
	}

	// The first time the tool opens, so a session that never uses it never has them.
	void InstallDrawHooks()
	{
		void** table = *reinterpret_cast<void***>(g_Context);
		const bool ok = Hook(table[CONTEXT_DRAW_INDEXED], reinterpret_cast<void*>(&DrawIndexed_Hook), reinterpret_cast<void**>(&g_DrawIndexed), "DrawIndexed") &&
			Hook(table[CONTEXT_DRAW], reinterpret_cast<void*>(&Draw_Hook), reinterpret_cast<void**>(&g_Draw), "Draw") &&
			Hook(table[CONTEXT_DRAW_INDEXED_INSTANCED], reinterpret_cast<void*>(&DrawIndexedInstanced_Hook), reinterpret_cast<void**>(&g_DrawIndexedInstanced), "DrawIndexedInstanced") &&
			Hook(table[CONTEXT_DRAW_INSTANCED], reinterpret_cast<void*>(&DrawInstanced_Hook), reinterpret_cast<void**>(&g_DrawInstanced), "DrawInstanced");
		Log::Write(ok ? "draw hooks in" : "draw hooks incomplete: picking won't work");
		g_DrawHooks = true;
	}

	bool Init(IDXGISwapChain* swapChain)
	{
		DXGI_SWAP_CHAIN_DESC desc{};
		if (FAILED(swapChain->GetDesc(&desc)) || !desc.OutputWindow)
			return false;
		DWORD process = 0;
		GetWindowThreadProcessId(desc.OutputWindow, &process);
		if (process != GetCurrentProcessId())
			return false;
		if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_Device))))
			return false;
		g_Device->GetImmediateContext(&g_Context);
		g_SwapChain = swapChain;
		g_Window = desc.OutputWindow;
		InstallSceneHooks();

		std::string error;
		if (!Tool::Init(g_Device, error))
		{
			Log::Write("tool: %s", error.c_str());
			return true;  // Nothing more to try: the plugin's POM goes on working.
		}

		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.LogFilename = nullptr;
		io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
		io.MouseDrawCursor = true;  // The game hides the real one, and fights over it.
		char windows[MAX_PATH] = {};
		GetWindowsDirectoryA(windows, MAX_PATH);
		const std::string font = std::string(windows) + "\\Fonts\\segoeui.ttf";
		if (GetFileAttributesA(font.c_str()) != INVALID_FILE_ATTRIBUTES)
			io.Fonts->AddFontFromFileTTF(font.c_str(), 17.0f);
		Panel::Style();
		ImGui_ImplWin32_Init(g_Window);
		ImGui_ImplDX11_Init(g_Device, g_Context);
		// No IME: its handler calls Imm* functions on the game's window from inside EndFrame.
		ImGui::GetPlatformIO().Platform_SetImeDataFn = nullptr;

		g_CatchersKnown = memcmp(reinterpret_cast<const void*>(FN_KEY_SET_CATCHER), KEY_SET_CATCHER_BYTES, sizeof(KEY_SET_CATCHER_BYTES)) == 0 &&
			memcmp(reinterpret_cast<const void*>(FN_KEY_REMOVE_CATCHER), KEY_REMOVE_CATCHER_BYTES, sizeof(KEY_REMOVE_CATCHER_BYTES)) == 0;
		if (!g_CatchersKnown)
			Log::Write("the key catcher functions differ: the camera may not freeze");

		g_OriginalWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(g_Window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HookedWndProc)));
		Log::Write("tool ready on window %p (%ux%u)", g_Window, desc.BufferDesc.Width, desc.BufferDesc.Height);
		return true;
	}

	void CreateRenderTarget()
	{
		ID3D11Texture2D* backBuffer = nullptr;
		if (SUCCEEDED(g_SwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) && backBuffer)
		{
			g_Device->CreateRenderTargetView(backBuffer, nullptr, &g_RenderTarget);
			backBuffer->Release();
		}
	}

	void Frame()
	{
		const bool open = g_Open;
		g_InScene.store(false, std::memory_order_relaxed);
		if (open && !g_DrawHooks)
			InstallDrawHooks();

		DXGI_SWAP_CHAIN_DESC desc{};
		g_SwapChain->GetDesc(&desc);
		Reflect::EndFrame(g_Context, desc.BufferDesc.Width, desc.BufferDesc.Height);
		Tool::SetKeyName(SettingsFile::KeyName(Engine::ToolKey()));
		Tool::EndFrame(g_Context, desc.BufferDesc.Width, desc.BufferDesc.Height);

		if (open && !t_InsideImGui)
		{
			if (!g_RenderTarget)
				CreateRenderTarget();
			if (g_RenderTarget)
			{
				ImGuiSection section;
				ImGui_ImplDX11_NewFrame();
				ImGui_ImplWin32_NewFrame();
				ImGui::NewFrame();
				const bool close = Tool::Draw(g_Context);
				ImGui::Render();
				g_Context->OMSetRenderTargets(1, &g_RenderTarget, nullptr);
				ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
				if (close)
					g_WantClose = true;
			}
		}
		g_InScene.store(open, std::memory_order_relaxed);
	}

	HRESULT STDMETHODCALLTYPE Present_Hook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags)
	{
		if (!(flags & DXGI_PRESENT_TEST))
		{
			if (!g_Initialized)
				g_Initialized = Init(swapChain);
			if (g_Initialized && swapChain == g_SwapChain && g_OriginalWndProc)
				Frame();
		}
		return g_Present(swapChain, syncInterval, flags);
	}

	HRESULT STDMETHODCALLTYPE ResizeBuffers_Hook(IDXGISwapChain* swapChain, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags)
	{
		if (g_RenderTarget && swapChain == g_SwapChain)
		{
			g_RenderTarget->Release();
			g_RenderTarget = nullptr;
		}
		return g_ResizeBuffers(swapChain, count, width, height, format, flags);
	}
}

bool Start()
{
	// A throwaway device + swapchain gives the addresses: with ReShade (dxgi.dll) they're ReShade's proxy classes, the same
	// the game's objects are, so the hooks sit in front of ReShade. d3d11 builds vtables per device, so the code is hooked.
	using Create_t = decltype(&D3D11CreateDeviceAndSwapChain);
	const HMODULE d3d11 = GetModuleHandleA("d3d11.dll");
	auto create = d3d11 ? reinterpret_cast<Create_t>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain")) : nullptr;
	if (!create)
	{
		Log::Write("d3d11.dll isn't loaded: no tool this session");
		return false;
	}
	WNDCLASSEXA wc{sizeof(wc), CS_HREDRAW | CS_VREDRAW, DefWindowProcA, 0, 0, GetModuleHandleA(nullptr), nullptr, nullptr, nullptr, nullptr, "PomDummy", nullptr};
	RegisterClassExA(&wc);
	HWND hwnd = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
	DXGI_SWAP_CHAIN_DESC sd{};
	sd.BufferCount = 1;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hwnd;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
	IDXGISwapChain* sc = nullptr;
	ID3D11Device* dev = nullptr;
	ID3D11DeviceContext* ctx = nullptr;
	const HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 3, D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);

	bool ok = false;
	if (SUCCEEDED(hr) && sc)
	{
		void** table = *reinterpret_cast<void***>(sc);
		ok = Hook(table[SWAPCHAIN_PRESENT], reinterpret_cast<void*>(&Present_Hook), reinterpret_cast<void**>(&g_Present), "Present") &&
			Hook(table[SWAPCHAIN_RESIZE_BUFFERS], reinterpret_cast<void*>(&ResizeBuffers_Hook), reinterpret_cast<void**>(&g_ResizeBuffers), "ResizeBuffers");
	}
	else
	{
		Log::Write("throwaway D3D11 device failed (0x%08X)", unsigned(hr));
	}
	if (ctx)
		ctx->Release();
	if (dev)
		dev->Release();
	if (sc)
		sc->Release();
	DestroyWindow(hwnd);
	UnregisterClassA(wc.lpszClassName, wc.hInstance);

	const MH_STATUS a = MH_CreateHookApi(L"user32", "SetCursorPos", reinterpret_cast<void*>(&HookedSetCursorPos), reinterpret_cast<void**>(&g_SetCursorPos));
	const MH_STATUS b = MH_CreateHookApi(L"user32", "ClipCursor", reinterpret_cast<void*>(&HookedClipCursor), reinterpret_cast<void**>(&g_ClipCursor));
	MH_EnableHook(MH_ALL_HOOKS);
	if (a != MH_OK || b != MH_OK)
		Log::Write("cursor hooks: %s, %s", MH_StatusToString(a), MH_StatusToString(b));
	return ok;
}
}
