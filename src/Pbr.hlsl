// Physically based highlights and reflections, in pieces that src/Brdf.cpp splices into the game's own lit pixel shaders.
// build-shaders.bat compiles one per SITE, into src/pbr/.
//
// BO2's lighting is further along than it looks. Its specular is already D * F * V, with Schlick's Fresnel on a real F0 and
// gloss driving everything, written by the people who brought that to the series:
//     power      = 2 ^ (13 * gloss)
//     D          = pow(NoH, power) * (power + 2) / 8               normalized Blinn-Phong, the pi and the 4 cancelled in
//     V          = NoL / (a * LoH * LoH + 1.01 - a)                a = saturate(gloss + 0.545)
//     F          = F0 + (1 - F0) * 2 ^ (-10 * LoH)                 F0 = spec map squared, or 0.04 without one
// F stays as it is: Schlick is what it should be. Authored gloss and spec maps are the roughness and metal inputs, which is
// why there's no extra map to load. What's replaced is what makes highlights look like a hard dot on a surface:
//
//   D  Blinn-Phong falls off like a Gaussian: bright core, nothing around it. GGX has the long tail real surfaces have.
//      With alpha^2 = 2 / (power + 2), the usual match between the two, the whole of GGX reduces to one line:
//          pow(NoH, power)  ->  1 / (1 + power * (1 - NoH * NoH) / 2) ^ 2
//      and the game's own (power + 2) / 8 stays the normalization. Same peak, same energy, new shape.
//   V  Smith with Schlick-GGX. The game's term only knows LoH, so it stays dim where Smith flares: grazing views.
//   Sparkle. Where the normal changes faster than a pixel, one sample can't see the highlight properly and it flickers.
//      The change in normal across the pixel widens the lobe for that pixel (Kaplanyan et al.).
//
// Probes are 64x64 a face, which no BRDF can make sharp, so reflections march the last frame's depth and color first, and
// the probe fills in whatever the screen doesn't have.
//
// Spliced, inputs and outputs turn into temps, or the game's own registers, and the rest of the registers are this code's
// own. Inputs are float4 so the compiler can't pack two into one register.

#define SITE_SURFACE 0     // After the normal is finished: what the rest needs to know about this pixel.
#define SITE_POWER 1       // After 2 ^ (13 * gloss).
#define SITE_LOBE_IN 2     // In place of log(NoH).
#define SITE_LOBE_OUT 3    // In place of exp(power * log(NoH)). The game's multiply by power is left between the two.
#define SITE_VISIBILITY 4  // In place of the division that gives V.
#define SITE_REFLECT 5     // After the probe is decoded.
#define SITE_MIX 6         // Where the reflection is added to the rest.
#define SITE_OMNI 7        // After the omni lights' diffuse: the game gives them no highlight at all.

// An unbound buffer reads as zeros, and that has to look like nothing went wrong, so all of these are offsets.
cbuffer PbrConstants : register(b12)
{
	float roughnessOffset;   // Added to 1, scales alpha^2.
	float highlightsOffset;  // Added to 1.
	float reflectionsOffset; // Added to 1.
	float screenAmount;      // 0 turns screen reflections off.
	float2 screenSize;
	float screenSteps;
	float screenReach;       // In world units.
};

static const float SHALLOWEST_VIEW = 0.03;   // Keeps V from exploding on normals that lean away from the camera.
static const float MOST_VARIANCE = 0.18;
static const float SMALLEST_AREA = 2.0 / 8194;  // alpha^2 at full gloss.

#if SITE == SITE_SURFACE

// x how much the surface faces the view, y how much wider the lobe has to be for this pixel.
float4 main(float4 normal : TEXCOORD0, float4 toPixel : TEXCOORD1) : SV_Target0
{
	const float3 acrossX = ddx(normal.xyz), acrossY = ddy(normal.xyz);
	const float variance = min(0.5 * (dot(acrossX, acrossX) + dot(acrossY, acrossY)), MOST_VARIANCE);
	const float facing = saturate(-dot(normal.xyz, normalize(toPixel.xyz)));
	return float4(max(facing, SHALLOWEST_VIEW), variance, 0, 0);
}

#elif SITE == SITE_POWER

// The power again, from a lobe widened by the pixel. Plus z, Smith's k, and w, the power, for the omni lights.
void main(float4 power : TEXCOORD0, float4 surface : TEXCOORD1, out float4 powerOut : SV_Target0, out float4 surfaceOut : SV_Target1)
{
	const float area = clamp(2 / (power.x + 2) * (1 + roughnessOffset) + surface.y, SMALLEST_AREA, 1);
	const float widened = 2 / area - 2;
	powerOut = widened;
	surfaceOut = float4(surface.xy, sqrt(area) * 0.5, widened);
}

#elif SITE == SITE_LOBE_IN

float main(float4 cosine : TEXCOORD0) : SV_Target0
{
	return 1 - cosine.x * cosine.x;
}

#elif SITE == SITE_LOBE_OUT

float main(float4 spread : TEXCOORD0) : SV_Target0
{
	const float falloff = 1 + spread.x * 0.5;
	return (1 + highlightsOffset) / (falloff * falloff);
}

#elif SITE == SITE_VISIBILITY

// G / (NoL * NoV) * NoL, which is what the game's V stands for.
float main(float4 facingLight : TEXCOORD0, float4 surface : TEXCOORD1) : SV_Target0
{
	const float k = surface.z;
	return facingLight.x / ((facingLight.x * (1 - k) + k) * (surface.x * (1 - k) + k));
}

#elif SITE == SITE_REFLECT

cbuffer PerSceneConsts : register(b0)
{
	float4 hdrScale : packoffset(c20);
	float4 toClipX : packoffset(c36);   // One vector per output, of a position relative to the camera.
	float4 toClipY : packoffset(c37);
	float4 toClipZ : packoffset(c38);
	float4 toClipW : packoffset(c39);
	float4 zNear : packoffset(c58);     // The depth buffer is reversed and infinite: w = zNear.x / depth.
};

Texture2D sceneColor : register(t16);
Texture2D<float> sceneDepth : register(t17);
SamplerState probeSampler : register(s15);

static const float HORIZON = 1.3;        // How soon a reflection fades as a bent normal sends it under the surface.
static const float ROUGHEST_MIP = 3.2;   // From here on the surface is too rough for the screen to add anything.
static const float PIXELS_PER_STEP = 3;
static const float NEAREST_W = 4;
static const float EDGE_FADE = 12;       // 1 / the share of the screen the fade at its edges takes.
static const float THICKNESS = 10;       // World units a surface is taken to go back.
static const float BIAS = 0.3;           // Keeps a surface from hitting itself.
static const float BIAS_PER_W = 0.003;
static const float NEAREST_SURFACE = 32;  // Closer than this is the first person gun, drawn with a field of view of its own.

float2 ToScreen(float4 clip)
{
	return float2(0.5 + 0.5 * clip.x / clip.w, 0.5 - 0.5 * clip.y / clip.w);
}

float4 ToClip(float3 position)
{
	return float4(dot(toClipX.xyz, position) + toClipX.w, dot(toClipY.xyz, position) + toClipY.w, dot(toClipZ.xyz, position) + toClipZ.w,
		dot(toClipW.xyz, position) + toClipW.w);
}

void main(float4 probe : TEXCOORD0, float4 direction : TEXCOORD1, float4 mip : TEXCOORD2, float4 toPixel : TEXCOORD3, float4 normalIn : TEXCOORD4,
	out float4 probeOut : SV_Target0, out float4 screen : SV_Target1)
{
	const float3 ray = normalize(direction.xyz);
	const float3 surfaceNormal = normalize(normalIn.xyz);
	float horizon = saturate(1 + HORIZON * dot(ray, surfaceNormal));
	horizon *= horizon;
	const float gain = 1 + reflectionsOffset;
	probeOut = float4(probe.rgb * horizon * gain, 0);
	screen = 0;

	const float sharpness = saturate(1 - mip.x / ROUGHEST_MIP);
	if (screenAmount > 0 && sharpness > 0 && horizon > 0)
	{
		const float4 clipStart = ToClip(toPixel.xyz);
		// Towards the camera the ray has to stop before it goes through the near plane.
		const float closing = dot(toClipW.xyz, ray);
		const float reach = closing < 0 ? min(screenReach, (NEAREST_W - clipStart.w) / closing) : screenReach;
		if (reach > 0 && clipStart.w > NEAREST_SURFACE)
		{
			const float4 clipEnd = ToClip(toPixel.xyz + ray * reach);
			const float2 from = ToScreen(clipStart), across = ToScreen(clipEnd) - from;
			const float2 inverseW = float2(1 / clipStart.w, 1 / clipEnd.w);

			// As far as the screen goes.
			float limit = 1;
			if (across.x > 0)
				limit = min(limit, (1 - from.x) / across.x);
			else if (across.x < 0)
				limit = min(limit, -from.x / across.x);
			if (across.y > 0)
				limit = min(limit, (1 - from.y) / across.y);
			else if (across.y < 0)
				limit = min(limit, -from.y / across.y);

			const float2 pixelsAcross = across * screenSize;
			const float pixels = length(pixelsAcross) * limit;
			const float steps = clamp(pixels / PIXELS_PER_STEP, 4, max(screenSteps, 4));

			// Noise per pixel in place of the bands equal steps leave.
			const float2 pixel = floor(from * screenSize);
			const float jitter = frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));

			float before = 0, hit = -1, wBefore = clipStart.w;
			[loop] for (float i = 0; i < steps; i += 1)
			{
				const float t = (i + jitter * 0.75 + 0.25) / steps * limit;
				const float2 uv = from + across * t;
				const float w = 1 / lerp(inverseW.x, inverseW.y, t);
				const float depth = sceneDepth.Load(int3(uv * screenSize, 0));
				if (depth > 0)
				{
					const float behind = w - zNear.x / depth;
					if (behind > BIAS + w * BIAS_PER_W && behind < abs(w - wBefore) + THICKNESS)
					{
						hit = t;
						break;
					}
				}
				before = t;
				wBefore = w;
			}

			if (hit >= 0)
			{
				// Between the last step in front of the surface and the first behind it.
				[loop] for (int i = 0; i < 4; ++i)
				{
					const float t = (before + hit) * 0.5;
					const float2 uv = from + across * t;
					const float w = 1 / lerp(inverseW.x, inverseW.y, t);
					const float depth = sceneDepth.Load(int3(uv * screenSize, 0));
					if (depth > 0 && w > zNear.x / depth)
						hit = t;
					else
						before = t;
				}

				const float2 uv = from + across * hit;
				const float2 edge = min(uv, 1 - uv) * EDGE_FADE;
				const float along = hit / limit;

				// A rough surface blurs what it reflects more the further that is. Same lobe as the highlights.
				const float gloss = 1 - mip.x * 0.25;
				const float spread = sqrt(2 / (exp(gloss * 13 * 0.693147) + 2));
				const float lod = log2(max(spread * length(pixelsAcross) * hit, 1));
				const float3 color = sceneColor.SampleLevel(probeSampler, uv, lod).rgb;

				screen.rgb = color * color / max(hdrScale.x, 1e-3) * gain;
				const float gun = saturate((clipStart.w - NEAREST_SURFACE) / NEAREST_SURFACE);
				screen.a = screenAmount * sharpness * horizon * gun * saturate(min(edge.x, edge.y)) * (1 - along * along);
			}
		}
	}
}

#elif SITE == SITE_MIX

// `reflected` went through what the probe's color went through, except for the lightmap tint, which it doesn't need: it's
// what's really there, already lit.
float4 main(float4 probe : TEXCOORD0, float4 reflected : TEXCOORD1) : SV_Target0
{
	// Past 1 what's reflected gets stronger, instead of the probe going negative.
	return float4(lerp(probe.rgb, reflected.rgb * max(reflected.a, 1), saturate(reflected.a)), 0);
}

#elif SITE == SITE_OMNI

cbuffer PerSceneConsts : register(b0)
{
	float4 omniXs : packoffset(c60);
	float4 omniYs : packoffset(c61);
	float4 omniZs : packoffset(c62);
	float4 omniFallOffs : packoffset(c63);
	float4 omniReds : packoffset(c64);
	float4 omniGreens : packoffset(c65);
	float4 omniBlues : packoffset(c66);
};

cbuffer PerObject : register(b3)
{
	uint4 lightFlags : packoffset(c11);  // y: the omnis.
};

float4 main(float4 normalIn : TEXCOORD0, float4 toPixel : TEXCOORD1, float4 surface : TEXCOORD2, float4 mirror : TEXCOORD3) : SV_Target0
{
	float3 total = 0;
	// Through a float: POM finds the shader's own block as an `if` on these flags.
	const float lit = lightFlags.y != 0 ? 1 : 0;
	if (lit > 0.5)
	{
		const float3 normal = normalIn.xyz;
		const float3 view = -normalize(toPixel.xyz);
		const float4 toX = omniXs - toPixel.x, toY = omniYs - toPixel.y, toZ = omniZs - toPixel.z;
		const float4 reach = sqrt(toX * toX + toY * toY + toZ * toZ);
		const float4 inverse = 1 / max(reach, 1e-3);
		const float4 lightX = toX * inverse, lightY = toY * inverse, lightZ = toZ * inverse;

		float4 weight = saturate(1 + reach * omniFallOffs);
		weight *= weight;
		const float4 facingLight = saturate(lightX * normal.x + lightY * normal.y + lightZ * normal.z);

		const float4 halfX = lightX + view.x, halfY = lightY + view.y, halfZ = lightZ + view.z;
		const float4 inverseHalf = 1 / max(sqrt(halfX * halfX + halfY * halfY + halfZ * halfZ), 1e-3);
		const float4 facingHalf = saturate((halfX * normal.x + halfY * normal.y + halfZ * normal.z) * inverseHalf);
		const float4 lightHalf = saturate((halfX * lightX + halfY * lightY + halfZ * lightZ) * inverseHalf);

		const float power = surface.w, k = surface.z;
		const float4 falloff = 1 + power * (1 - facingHalf * facingHalf) * 0.5;
		const float4 lobe = (power + 2) * 0.125 / (falloff * falloff);
		const float4 visibility = 1 / ((facingLight * (1 - k) + k) * (surface.x * (1 - k) + k));
		const float4 fresnel = exp(lightHalf * -6.93147);  // 2 ^ (-10 * LoH), as the game's own.
		const float4 strength = lobe * visibility * facingLight * weight * (1 + highlightsOffset);

		total = float3(dot(omniReds, strength * (mirror.r + (1 - mirror.r) * fresnel)), dot(omniGreens, strength * (mirror.g + (1 - mirror.g) * fresnel)),
			dot(omniBlues, strength * (mirror.b + (1 - mirror.b) * fresnel)));
	}
	return float4(total, 0);
}

#endif
