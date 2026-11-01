// Parallax occlusion: where on the texture does the ray through this pixel really land? This runs first in a patched
// pixel shader (src/Dxbc.cpp splices it in), and the shader's own samples of the material's layer 0 then use the result.
// build-shaders.bat turns this into src/prologue/*.h, one for each kind of shader:
//   LIGHT        which light the shader's own code has: 0 none, 1 the sun, 2 a primary light (a map's own spot or omni)
//   TWIN         it's a "dlight glight" twin, with the blocks for the spot and the omnis placed in the world
//   MOVE_LOOKUP  it reads a shadow map through TEXCOORD4
//
// Spliced, the inputs are the game's own registers, the outputs turn into temps, the Literals cbuffer into literals, and
// t1/s1 stay what they are: the material's normal map, whose replacement carries height in blue (1 = the surface itself)
// and how open the spot is to the sky in alpha. b0 and b3 are the game's own too.

#ifndef LIGHT
#define LIGHT 0
#endif
#ifndef TWIN
#define TWIN 0
#endif
#ifndef MOVE_LOOKUP
#define MOVE_LOOKUP 0
#endif

cbuffer Literals : register(b12)
{
	float depthUv;        // How deep the texture goes at height 0, in UV units. Below 0, it's inverted: it rises instead.
	float mostSteps;      // Looking along the surface.
	float fewestSteps;    // Looking straight at it.
	float steepest;       // Smallest cosine the ray's slant is taken from: keeps grazing views from sliding far away.
	float textureWidth;
	float fadeEnd;        // Past this distance the shader is itself.
	float fadeScale;      // 1 / length of the fade.
	float traceSteps;     // Height samples per light for relief shadows.
	float crevice;        // How much the alpha channel darkens the surface.
	float reliefShadows;  // How much the relief shadows itself. 0 turns shadows off.
	float literalPad0;
	float literalPad1;
};

#if LIGHT || TWIN || MOVE_LOOKUP
cbuffer PerSceneConsts : register(b0)
{
	float4 sunPosition : packoffset(c18);   // The direction to the sun.
	float4 sunDiffuse : packoffset(c19);
	float4 shadowLookup0 : packoffset(c48);
	float4 shadowLookup1 : packoffset(c49);
	float4 shadowLookup2 : packoffset(c50);
	float4 shadowLookup3 : packoffset(c51);
	float4 omniXs : packoffset(c60);
	float4 omniYs : packoffset(c61);
	float4 omniZs : packoffset(c62);
	float4 omniFallOffs : packoffset(c63);
	float4 omniReds : packoffset(c64);
	float4 omniGreens : packoffset(c65);
	float4 omniBlues : packoffset(c66);
	float4 spotPosition : packoffset(c67);
	float4 primaryPosition : packoffset(c77);
	float4 primaryDiffuse : packoffset(c78);
};
#endif

#if TWIN
cbuffer PerObject : register(b3)
{
	uint4 lightFlags : packoffset(c11);  // x: this surface has the spot on it. y: the omnis.
};
#endif

Texture2D normalMap : register(t1);
SamplerState normalSampler : register(s1);

struct Output
{
	float4 uv : SV_Target0;
	float4 uvDdx : SV_Target1;  // The gradients of the unshifted UV, for the shader's samples: the shifted one jumps at
	float4 uvDdy : SV_Target2;  // every edge in the height, which would pick the wrong mip along it.
	float4 shade : SV_Target3;  // x darkens the color, z the placed spot, w the placed omnis.
#if LIGHT
	float4 lightColor : SV_Target4;  // The sun's or primary light's color, with the relief in the way taken off.
#endif
#if MOVE_LOOKUP
	float4 lookup : SV_Target5;      // TEXCOORD4 at the point the ray really hit.
#endif
#if TWIN
	float4 position : SV_Target6;    // The same for the placed spot's shadow lookup, which the shader works out itself.
#endif
};

static const float SHADOW_SOFTNESS = 14;  // 1 / how far past a ridge, in shares of the depth, its shadow takes to go dark.
static const float SHALLOWEST_LIGHT = 0.05;

struct Surface
{
	float2 uv;
	float height;       // Where it sits, 0..1, going up.
	float flip;         // 1 when inverted: what's low in the texture is what rises.
	float lod;
	float3 normal;
	float3 gradientU;   // Which way the texture runs, as worked out in main.
	float3 gradientV;
	float side;
	float scale;
	float depth;        // The UV distance that's the full height.
	float strength;     // How much shadow counts here.
};

// Walks from the point towards the light, up through the height. Anything rising past the ray stands in its way. Returns
// how much of the light arrives: 1 is all of it.
float LightArriving(Surface s, float3 toLight)
{
	const float facing = dot(toLight, s.normal);
	const float above = 1 - s.height;
	if (facing <= 0 || above < 0.004 || traceSteps < 1 || s.scale <= 0)
		return 1;
	const float2 lean = float2(dot(toLight, s.gradientU), dot(toLight, s.gradientV)) * s.side / s.scale;
	const float rise = above / traceSteps;
	const float2 stepUv = lean / max(facing, SHALLOWEST_LIGHT) * s.depth * rise;

	float2 here = s.uv;
	float ray = s.height;
	float arriving = 1;
	[loop] for (float i = 0; i < traceSteps && arriving > 0; i += 1)
	{
		here += stepUv;
		ray += rise;
		const float surface = abs(s.flip - normalMap.SampleLevel(normalSampler, here, s.lod).b);
		arriving = min(arriving, 1 - saturate((surface - ray) * SHADOW_SOFTNESS));
	}
	// Past 1, deeper shadows instead of a negative light.
	return s.strength <= 1 ? lerp(1, arriving, s.strength) : pow(arriving, s.strength);
}

// Full registers, so the compiler can't pack two of them into one.
Output main(float4 texcoord : TEXCOORD0, float4 normalIn : TEXCOORD1, float4 toPixel : TEXCOORD5
#if MOVE_LOOKUP
	, float4 lookupIn : TEXCOORD4
#endif
	)
{
	const float2 uv = texcoord.xy;
	const float2 uvX = ddx(uv), uvY = ddy(uv);
	const float3 positionX = ddx(toPixel.xyz), positionY = ddy(toPixel.xyz);

	Output output;
	output.uv = float4(uv, 0, 0);
	output.uvDdx = float4(uvX, 0, 0);
	output.uvDdy = float4(uvY, 0, 0);
	output.shade = float4(1, 1, 1, 1);
#if LIGHT == 1
	output.lightColor = sunDiffuse;
#elif LIGHT == 2
	output.lightColor = primaryDiffuse;
#endif
#if MOVE_LOOKUP
	output.lookup = lookupIn;
#endif
#if TWIN
	output.position = float4(toPixel.xyz, 1);
#endif

	const float distance = length(toPixel.xyz);
	const float strength = saturate((fadeEnd - distance) * fadeScale);
	if (strength > 0)
	{
		const float3 view = toPixel.xyz / distance;
		const float3 normal = normalize(normalIn.xyz);
		const float facing = -dot(view, normal);

		// The gradients of u and v along the surface, from the screen derivatives. Unlike the vertex tangents these say
		// which way the texture really runs on this surface, mirrored or not, and how much it's scaled along each axis.
		const float3 acrossY = cross(positionY, normal), acrossX = cross(normal, positionX);
		const float3 gradientU = acrossY * uvX.x + acrossX * uvY.x;
		const float3 gradientV = acrossY * uvX.y + acrossX * uvY.y;
		const float scale = sqrt(length(gradientU) * length(gradientV));

		if (facing > 0 && scale > 0)
		{
			const float determinant = dot(positionX, acrossY);
			const float side = sign(determinant);
			const float2 lean = float2(dot(view, gradientU), dot(view, gradientV)) * side / scale;
			const float slant = 1 / max(facing, steepest);
			const float2 shift = lean * slant * depthUv * strength;

			const float steps = lerp(mostSteps, fewestSteps, facing);
			const float stepDepth = 1 / steps;
			const float2 stepUv = shift * stepDepth;

			// Plain trilinear on the sharper axis: a dozen anisotropic taps per step buy nothing here.
			const float2 texelsX = uvX * textureWidth, texelsY = uvY * textureWidth;
			const float lod = max(0.5 * log2(min(dot(texelsX, texelsX), dot(texelsY, texelsY))), 0);

			float2 here = uv, before = uv;
			float rayDepth = 0, rayBefore = 0;
			float surfaceDepth = 1 - normalMap.SampleLevel(normalSampler, here, lod).b;
			float gapBefore = surfaceDepth;
			[loop] for (float i = 0; i < steps && rayDepth < surfaceDepth; i += 1)
			{
				gapBefore = surfaceDepth - rayDepth;
				before = here;
				rayBefore = rayDepth;
				here += stepUv;
				rayDepth += stepDepth;
				surfaceDepth = 1 - normalMap.SampleLevel(normalSampler, here, lod).b;
			}
			// Between the last two points the ray went from above the surface to below it.
			const float gapAfter = rayDepth - surfaceDepth;
			const float crossing = saturate(gapBefore / max(gapBefore + gapAfter, 1e-5));
			const float2 hit = lerp(before, here, crossing);
			output.uv.xy = hit;

			const float4 texel = normalMap.SampleLevel(normalSampler, hit, lod);
			output.shade.x = max(lerp(1, texel.a, crevice * strength), 0);

#if LIGHT || TWIN || MOVE_LOOKUP
			if (reliefShadows > 0)
			{
				// How far the ray went past the surface, in world units: what shadow maps have to be asked about.
				const float3 moved = view * (lerp(rayBefore, rayDepth, crossing) * depthUv * strength * slant * abs(determinant) / scale);
#if MOVE_LOOKUP
				output.lookup = lookupIn + float4(dot(shadowLookup0.xyz, moved), dot(shadowLookup1.xyz, moved), dot(shadowLookup2.xyz, moved),
					dot(shadowLookup3.xyz, moved));
#endif
#if TWIN
				output.position.xyz += moved;
#endif

				Surface surface;
				surface.uv = hit;
				surface.flip = depthUv < 0 ? 1 : 0;
				surface.height = abs(surface.flip - texel.b);
				surface.lod = lod;
				surface.normal = normal;
				surface.gradientU = gradientU;
				surface.gradientV = gradientV;
				surface.side = side;
				surface.scale = scale;
				surface.depth = abs(depthUv) * strength;
				surface.strength = reliefShadows * strength;

#if LIGHT == 1
				output.lightColor.rgb *= LightArriving(surface, normalize(sunPosition.xyz));
#elif LIGHT == 2
				output.lightColor.rgb *= LightArriving(surface, normalize(primaryPosition.xyz - toPixel.xyz));
#endif
#if TWIN
				// Through a float: MoreLights finds the shader's light blocks as the first `if` on these flags.
				const float2 lightsOn = float2(lightFlags.x != 0 ? 1 : 0, lightFlags.y != 0 ? 1 : 0);
				if (lightsOn.x > 0.5)
					output.shade.z = LightArriving(surface, normalize(spotPosition.xyz - toPixel.xyz));
				if (lightsOn.y > 0.5)
				{
					// The block adds its four omnis up, so they share one trace, from where most of their light comes from.
					const float4 toX = omniXs - toPixel.x, toY = omniYs - toPixel.y, toZ = omniZs - toPixel.z;
					const float4 reach = max(sqrt(toX * toX + toY * toY + toZ * toZ), 1e-3);
					float4 weight = saturate(1 + reach * omniFallOffs);
					weight *= weight * saturate((toX * normal.x + toY * normal.y + toZ * normal.z) / reach) * (omniReds * 0.3 + omniGreens * 0.6 + omniBlues * 0.1);
					const float3 toLight = float3(dot(toX / reach, weight), dot(toY / reach, weight), dot(toZ / reach, weight));
					if (dot(toLight, toLight) > 1e-8)
						output.shade.w = LightArriving(surface, normalize(toLight));
				}
#endif
			}
#endif
		}
	}
	return output;
}
