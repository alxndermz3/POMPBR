// Drawn over the selected surface with additive blending; the blend factor sets its color and strength. No inputs, so it
// links with any vertex shader the game uses. build-shaders.bat turns this into src/Highlight.h.
float4 main() : SV_Target
{
	return 1;
}
