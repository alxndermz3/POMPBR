#include "Bake.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>

namespace Pom::Bake
{
namespace
{
	constexpr double SLOPE_SCALE = 4.015748;
	constexpr double SLOPE_BIAS = -2.015748;
	constexpr double PI = 3.14159265358979323846;
	constexpr double SHADE_NEAR = 1.2;     // Texels of a 256 texture.
	constexpr double SHADE_WIDE = 5;
	constexpr double SHADE_DARKEST = 0.9;  // What's left of the light in the deepest spot is 1 - this.

	// Single precision: the height ends up in 8 bits, and this runs inside a 32-bit game where a 2048 texture in doubles
	// would need hundreds of megabytes at once.
	using Real = float;
	using Complex = std::complex<Real>;

	struct Grid
	{
		int width = 0, height = 0;
		std::vector<Real> values;

		Grid() = default;
		Grid(int w, int h) : width(w), height(h), values(size_t(w) * h) {}
		Real& At(int x, int y) { return values[size_t(y) * width + x]; }
		Real At(int x, int y) const { return values[size_t(y) * width + x]; }
	};

	bool IsPowerOfTwo(uint32_t value)
	{
		return value && !(value & (value - 1));
	}

	// In place, radix 2. `direction` is -1 forward, +1 inverse (unscaled).
	void Fft(Complex* data, int count, int stride, int direction, const std::vector<Complex>& twiddles)
	{
		for (int i = 1, j = 0; i < count; ++i)
		{
			int bit = count >> 1;
			for (; j & bit; bit >>= 1)
				j ^= bit;
			j ^= bit;
			if (i < j)
				std::swap(data[size_t(i) * stride], data[size_t(j) * stride]);
		}
		for (int length = 2; length <= count; length <<= 1)
		{
			const int half = length / 2, step = count / length;
			for (int start = 0; start < count; start += length)
			{
				for (int k = 0; k < half; ++k)
				{
					const Complex w = direction < 0 ? twiddles[size_t(k) * step] : std::conj(twiddles[size_t(k) * step]);
					Complex& a = data[size_t(start + k) * stride];
					Complex& b = data[size_t(start + k + half) * stride];
					const Complex t = b * w;
					b = a - t;
					a += t;
				}
			}
		}
	}

	std::vector<Complex> Twiddles(int count)
	{
		std::vector<Complex> table(count / 2 + 1);
		for (size_t k = 0; k < table.size(); ++k)
		{
			const double angle = -2 * PI * double(k) / count;
			table[k] = Complex(Real(std::cos(angle)), Real(std::sin(angle)));
		}
		return table;
	}

	struct Spectrum
	{
		int width = 0, height = 0;
		std::vector<Complex> values;
	};

	void Transform(Spectrum& s, int direction)
	{
		const std::vector<Complex> across = Twiddles(s.width), down = Twiddles(s.height);
		for (int y = 0; y < s.height; ++y)
			Fft(&s.values[size_t(y) * s.width], s.width, 1, direction, across);
		for (int x = 0; x < s.width; ++x)
			Fft(&s.values[x], s.height, s.width, direction, down);
	}

	Spectrum Forward(const Grid& grid)
	{
		Spectrum s{grid.width, grid.height, std::vector<Complex>(grid.values.begin(), grid.values.end())};
		Transform(s, -1);
		return s;
	}

	Grid Inverse(Spectrum s)
	{
		Transform(s, 1);
		Grid grid(s.width, s.height);
		const Real scale = Real(1.0 / (double(s.width) * s.height));
		for (size_t i = 0; i < grid.values.size(); ++i)
			grid.values[i] = s.values[i].real() * scale;
		return grid;
	}

	double Frequency(int index, int count)
	{
		return double(index <= count / 2 ? index : index - count) / count;
	}

	// Wraps around the edges, as the texture does.
	Grid Gaussian(const Grid& grid, double sigma)
	{
		Spectrum s = Forward(grid);
		for (int y = 0; y < s.height; ++y)
		{
			const double fy = Frequency(y, s.height);
			for (int x = 0; x < s.width; ++x)
			{
				const double fx = Frequency(x, s.width);
				s.values[size_t(y) * s.width + x] *= Real(std::exp(-2 * (PI * sigma) * (PI * sigma) * (fx * fx + fy * fy)));
			}
		}
		return Inverse(std::move(s));
	}

	// Least-squares height for a gradient field (Frankot-Chellappa). Periodic, so what tiles stays tiling.
	Grid Integrate(const Grid& dx, const Grid& dy)
	{
		Spectrum sx = Forward(dx), sy = Forward(dy);
		for (int y = 0; y < sx.height; ++y)
		{
			const double fy = 2 * PI * Frequency(y, sx.height);
			for (int x = 0; x < sx.width; ++x)
			{
				const double fx = 2 * PI * Frequency(x, sx.width);
				const double power = fx * fx + fy * fy;
				Complex& out = sx.values[size_t(y) * sx.width + x];
				const Complex wx(0, Real(fx)), wy(0, Real(fy));
				out = power > 0 ? (std::conj(wx) * out + std::conj(wy) * sy.values[size_t(y) * sx.width + x]) / Real(power) : Complex(0);
			}
		}
		return Inverse(std::move(sx));
	}

	// How far a gradient field is from being one: 0 for a real height.
	double Curl(const Grid& dx, const Grid& dy)
	{
		double sum = 0;
		for (int y = 0; y < dx.height; ++y)
		{
			const int below = (y + 1) % dx.height;
			for (int x = 0; x < dx.width; ++x)
			{
				const int right = (x + 1) % dx.width;
				sum += std::fabs((dx.At(x, below) - dx.At(x, y)) - (dy.At(right, y) - dy.At(x, y)));
			}
		}
		return sum / double(dx.values.size());
	}

	double Percentile(std::vector<Real> values, double percent)
	{
		const double position = percent / 100 * double(values.size() - 1);
		const size_t below = size_t(position);
		const size_t above = (std::min)(below + 1, values.size() - 1);
		std::nth_element(values.begin(), values.begin() + below, values.end());
		const double low = values[below];
		if (above == below)
			return low;
		const double high = *std::min_element(values.begin() + above, values.end());
		return low + (high - low) * (position - double(below));
	}

	double Smoothstep(double low, double high, double x)
	{
		const double t = (std::min)((std::max)((x - low) / (high - low), 0.0), 1.0);
		return t * t * (3 - 2 * t);
	}

	double Clip01(double x)
	{
		return (std::min)((std::max)(x, 0.0), 1.0);
	}

	// Luminance of the color map at the normal map's size.
	Grid LumaAt(const Pixels& color, int width, int height)
	{
		Grid out(width, height);
		for (int y = 0; y < height; ++y)
		{
			for (int x = 0; x < width; ++x)
			{
				const uint32_t cx = uint32_t(uint64_t(x) * color.width / width), cy = uint32_t(uint64_t(y) * color.height / height);
				const uint8_t* p = &color.rgba[(size_t(cy) * color.width + cx) * 4];
				out.At(x, y) = Real((p[0] * 0.299 + p[1] * 0.587 + p[2] * 0.114) / 255);
			}
		}
		return out;
	}


	Grid HeightOf(const Pixels& normal, const Pixels& color, const Recipe& recipe)
	{
		const int width = int(normal.width), height = int(normal.height);
		const double scale = width / 256.0;
		Grid sx(width, height), sy(width, height);
		double meanX = 0, meanY = 0;
		for (size_t i = 0; i < sx.values.size(); ++i)
		{
			sx.values[i] = Real(normal.rgba[i * 4] / 255.0 * SLOPE_SCALE + SLOPE_BIAS);
			sy.values[i] = Real(normal.rgba[i * 4 + 1] / 255.0 * SLOPE_SCALE + SLOPE_BIAS);
			meanX += sx.values[i];
			meanY += sy.values[i];
		}
		meanX /= double(sx.values.size());
		meanY /= double(sx.values.size());
		for (size_t i = 0; i < sx.values.size(); ++i)
		{
			// A bump's normal leans away from it, so slope = -gradient.
			sx.values[i] = Real(meanX - sx.values[i]);
			sy.values[i] = Real(meanY - sy.values[i]);
		}

		// Which way the map's channels run: only the right way round is the gradient of anything. Green can be flipped,
		// and in custom textures red and green can be swapped. The shading stays as it is; this only finds the height.
		Grid flippedY = sy;
		for (Real& v : flippedY.values)
			v = -v;
		const Grid* candidates[4][2] = {{&sx, &sy}, {&sx, &flippedY}, {&sy, &sx}, {&flippedY, &sx}};
		const Grid* bestX = &sx;
		const Grid* bestY = &sy;
		double bestCurl = 0;
		for (int i = 0; i < 4; ++i)
		{
			const double curl = Curl(*candidates[i][0], *candidates[i][1]);
			if (i == 0 || curl < bestCurl)
			{
				bestCurl = curl;
				bestX = candidates[i][0];
				bestY = candidates[i][1];
			}
		}

		Grid raw = Integrate(*bestX, *bestY);
		const Grid drift = Gaussian(raw, recipe.flatten * scale * 0.5);
		Grid flat(width, height);
		for (size_t i = 0; i < flat.values.size(); ++i)
			flat.values[i] = raw.values[i] - drift.values[i];

		const double tops = Percentile(flat.values, recipe.tops), bottoms = Percentile(flat.values, recipe.bottoms);
		Grid shape(width, height);
		for (size_t i = 0; i < shape.values.size(); ++i)
			shape.values[i] = Real(Clip01((flat.values[i] - bottoms) / (tops - bottoms)));

		// Seams: dark against their surroundings in the color map.
		const Grid luma = LumaAt(color, width, height);
		const Grid local = Gaussian(luma, recipe.radius * scale);
		Grid darkness(width, height);
		for (size_t i = 0; i < darkness.values.size(); ++i)
			darkness.values[i] = local.values[i] - luma.values[i];
		const double darkest = (std::max)(Percentile(darkness.values, 97), 1e-3);
		Grid seam(width, height);
		for (size_t i = 0; i < seam.values.size(); ++i)
			seam.values[i] = Real(Smoothstep(0.35, 1.0, darkness.values[i] / darkest));
		const Grid softSeam = Gaussian(seam, recipe.blur * scale);

		Grid out(width, height);
		if (recipe.plain)
		{
			// A board's surface stays at the top; only its grain and the gaps between boards go down from there.
			std::vector<Real> magnitude(darkness.values.size());
			for (size_t i = 0; i < magnitude.size(); ++i)
				magnitude[i] = std::fabs(darkness.values[i]);
			const double strongest = (std::max)(Percentile(std::move(magnitude), 99), 1e-3);
			Grid grain(width, height);
			for (size_t i = 0; i < grain.values.size(); ++i)
				grain.values[i] = Real(Clip01(darkness.values[i] / strongest));
			const Grid softGrain = Gaussian(grain, 0.4 * scale);
			for (size_t i = 0; i < out.values.size(); ++i)
				out.values[i] = Real((1 - recipe.grain * softGrain.values[i]) * (1 - recipe.gap * softSeam.values[i]));
		}
		else
		{
			// Stones: rounded shape from the normal map, measured down from their tops, so a stone's top is the surface.
			for (size_t i = 0; i < out.values.size(); ++i)
			{
				const double dropped = softSeam.values[i] * Smoothstep(0.75, 0.35, shape.values[i]);
				out.values[i] = Real(Clip01(shape.values[i] * (1 - recipe.gap * 0.5) + recipe.gap * 0.5) * (1 - recipe.gap * dropped));
			}
		}

		out = Gaussian(out, recipe.soften * scale);
		const auto range = std::minmax_element(out.values.begin(), out.values.end());
		const double low = *range.first, high = *range.second;
		for (Real& v : out.values)
			v = high > low ? Real(Clip01((v - low) / (high - low))) : 1;
		return out;
	}

	Grid BoxDown(const Grid& grid);

	void StoreShading(PomFile& file)
	{
		const int width = int(file.header.width), height = int(file.header.height);
		Grid surface(width, height);
		for (size_t i = 0; i < surface.values.size(); ++i)
			surface.values[i] = file.pixels[i * 4 + 2] / Real(255);

		// How far below its surroundings a point sits, at a narrow and a wide reach: thin seams and broad hollows both.
		const double scale = width / 256.0;
		const Grid near = Gaussian(surface, SHADE_NEAR * scale), wide = Gaussian(surface, SHADE_WIDE * scale);
		Grid cavity(width, height);
		for (size_t i = 0; i < cavity.values.size(); ++i)
			cavity.values[i] = (std::max)(near.values[i] - surface.values[i], Real(0)) + (std::max)(wide.values[i] - surface.values[i], Real(0));
		const double deepest = (std::max)(Percentile(cavity.values, 99.5), 1e-3);

		Grid shade(width, height);
		for (size_t i = 0; i < shade.values.size(); ++i)
			shade.values[i] = Real(1 - SHADE_DARKEST * Clip01(cavity.values[i] / deepest));

		size_t at = 0;
		uint32_t levelWidth = file.header.width, levelHeight = file.header.height;
		for (uint32_t mip = 0; mip < file.header.mips; ++mip)
		{
			for (size_t i = 0; i < shade.values.size(); ++i)
				file.pixels[at + i * 4 + 3] = uint8_t(std::lround(Clip01(shade.values[i]) * 255));
			at += size_t(levelWidth) * levelHeight * 4;
			levelWidth = (std::max)(levelWidth / 2, 1u);
			levelHeight = (std::max)(levelHeight / 2, 1u);
			shade = BoxDown(shade);
		}
	}

	Grid BoxDown(const Grid& grid)
	{
		const int width = (std::max)(grid.width / 2, 1), height = (std::max)(grid.height / 2, 1);
		Grid out(width, height);
		if (grid.width == 1 || grid.height == 1)
		{
			for (int y = 0; y < height; ++y)
			{
				for (int x = 0; x < width; ++x)
					out.At(x, y) = grid.At(x, y);
			}
			return out;
		}
		for (int y = 0; y < height; ++y)
		{
			for (int x = 0; x < width; ++x)
				out.At(x, y) = (grid.At(x * 2, y * 2) + grid.At(x * 2 + 1, y * 2) + grid.At(x * 2, y * 2 + 1) + grid.At(x * 2 + 1, y * 2 + 1)) / 4;
		}
		return out;
	}
}

const char* PresetName(Preset preset)
{
	switch (preset)
	{
	case Preset::Stone: return "Stone / brick";
	case Preset::Boards: return "Boards / panels";
	case Preset::Bark: return "Bark / rough";
	case Preset::Detail: return "Metal / detail";
	default: return "Auto";
	}
}

Recipe RecipeFor(Preset preset)
{
	Recipe recipe;
	switch (preset)
	{
	case Preset::Boards:
		recipe.depth = 0.02f;
		recipe.gap = 1;
		recipe.blur = 0.4f;
		recipe.flatten = 30;
		recipe.radius = 2;
		recipe.plain = true;
		recipe.grain = 0.1f;
		break;
	case Preset::Bark:
		recipe.depth = 0.016f;
		recipe.gap = 0.25f;
		recipe.blur = 0.6f;
		recipe.flatten = 30;
		recipe.radius = 3;
		recipe.soften = 0.9f;
		break;
	case Preset::Auto:
	case Preset::Detail:
		recipe.depth = 0.008f;
		recipe.gap = 0;
		recipe.blur = 0.5f;
		recipe.flatten = 8;
		recipe.tops = 50;
		recipe.bottoms = 0.5f;
		break;
	default:
		break;
	}
	return recipe;
}

bool Build(const std::vector<Pixels>& normal, const Pixels& color, const Recipe& recipe, PomFile& out, std::string& error)
{
	if (normal.empty() || !color.width || !color.height)
	{
		error = "there's no texture to read";
		return false;
	}
	const Pixels& top = normal[0];
	if (!IsPowerOfTwo(top.width) || !IsPowerOfTwo(top.height))
	{
		error = "the normal map isn't a power of two in size";
		return false;
	}

	Grid height = HeightOf(top, color, recipe);

	memcpy(out.header.magic, "POM1", 4);
	out.header.width = top.width;
	out.header.height = top.height;
	out.header.mips = uint32_t(normal.size());
	out.header.depthUv = recipe.depth;
	out.pixels.clear();
	for (const Pixels& level : normal)
	{
		const size_t at = out.pixels.size();
		out.pixels.resize(at + size_t(level.width) * level.height * 4);
		uint8_t* to = out.pixels.data() + at;
		for (size_t i = 0; i < size_t(level.width) * level.height; ++i)
		{
			to[i * 4] = level.rgba[i * 4];
			to[i * 4 + 1] = level.rgba[i * 4 + 1];
			to[i * 4 + 2] = uint8_t(std::lround(Clip01(height.values[i]) * 255));
			to[i * 4 + 3] = 255;
		}
		height = BoxDown(height);
	}
	StoreShading(out);
	return true;
}

void AddShading(PomFile& file)
{
	StoreShading(file);
}

bool HasShading(const PomFile& file)
{
	const size_t texels = size_t(file.header.width) * file.header.height;
	for (size_t i = 0; i < texels; ++i)
	{
		if (file.pixels[i * 4 + 3] != 255)
			return true;
	}
	return false;
}
}
