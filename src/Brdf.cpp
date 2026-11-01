#include "Brdf.h"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

#include "DxbcFormat.h"
#include "pbr/site_0.h"
#include "pbr/site_1.h"
#include "pbr/site_2.h"
#include "pbr/site_3.h"
#include "pbr/site_4.h"
#include "pbr/site_5.h"
#include "pbr/site_6.h"
#include "pbr/site_7.h"

namespace Pom::Brdf
{
namespace
{
	using namespace Dxbc;

	// As in Pbr.hlsl.
	enum Site
	{
		SITE_SURFACE,
		SITE_POWER,
		SITE_LOBE_IN,
		SITE_LOBE_OUT,
		SITE_VISIBILITY,
		SITE_REFLECT,
		SITE_MIX,
		SITE_OMNI,
		SITE_COUNT,
	};

	struct Compiled
	{
		const BYTE* code;
		size_t size;
	};

	const Compiled COMPILED[SITE_COUNT] = {
		{g_Site_0, sizeof(g_Site_0)},
		{g_Site_1, sizeof(g_Site_1)},
		{g_Site_2, sizeof(g_Site_2)},
		{g_Site_3, sizeof(g_Site_3)},
		{g_Site_4, sizeof(g_Site_4)},
		{g_Site_5, sizeof(g_Site_5)},
		{g_Site_6, sizeof(g_Site_6)},
		{g_Site_7, sizeof(g_Site_7)},
	};

	struct Snippet
	{
		std::vector<uint32_t> words;
		std::vector<Instruction> code;
		uint32_t temps = 0;
		bool ok = false;
	};

	const std::vector<Snippet>& Snippets()
	{
		static const std::vector<Snippet> snippets = []
		{
			std::vector<Snippet> all(SITE_COUNT);
			for (int i = 0; i < SITE_COUNT; ++i)
			{
				std::vector<Chunk> chunks;
				Snippet& snippet = all[i];
				snippet.ok = ReadChunks(COMPILED[i].code, COMPILED[i].size, chunks) && ReadProgram(chunks, snippet.words) && ReadInstructions(snippet.words, snippet.code);
				for (const Instruction& ins : snippet.code)
				{
					if (ins.opcode == OP_DCL_TEMPS)
						snippet.temps = snippet.words[ins.at + 1];
				}
				// Spliced into the middle of a shader, a return would end the whole of it.
				for (size_t n = 0; snippet.ok && n + 1 < snippet.code.size(); ++n)
					snippet.ok = snippet.code[n].opcode != OP_RET;
			}
			return all;
		}();
		return snippets;
	}

	constexpr uint32_t SCENE_BUFFER = 0;
	constexpr uint32_t OBJECT_BUFFER = 3;
	constexpr uint32_t LIGHT_FLAGS = 11;
	constexpr uint32_t OMNI_FLAG = 1;
	constexpr uint32_t SCENE_VECTORS_READ = 67;   // What the snippets read of b0.
	constexpr uint32_t CONSTANT_VECTORS = 2;
	constexpr uint32_t PROBE_RESOURCE = 15;
	constexpr uint32_t OPERAND_MODIFIER = 1;
	constexpr int MOST_HOPS = 8;

	constexpr float GLOSS_TO_POWER = 13;
	constexpr float VIEW_FALLOFF = -9.28f;
	constexpr float VISIBILITY_END = 1.01f;
	constexpr float DIELECTRIC = 0.04f;

	struct Slot
	{
		uint32_t reg = 0;
		uint32_t component = 0;
	};

	bool Near(float a, float b)
	{
		return std::abs(a - b) < 1e-4f;
	}

	uint32_t ComponentRead(const Operand& source, uint32_t destination)
	{
		if (source.mode == MODE_SELECT1)
			return source.selection & 3;
		if (source.mode == MODE_SWIZZLE)
			return (source.selection >> (destination * 2)) & 3;
		return destination;
	}

	// The components a mask writes, in order. How many there are.
	uint32_t ComponentsOf(uint32_t mask, uint32_t out[4])
	{
		uint32_t count = 0;
		for (uint32_t c = 0; c < 4; ++c)
		{
			if (mask & (1u << c))
				out[count++] = c;
		}
		return count;
	}

	bool HasModifier(const std::vector<uint32_t>& words, const Operand& o)
	{
		return (o.token >> 31) && (words[o.at + 1] & 0x3F) == OPERAND_MODIFIER && ((words[o.at + 1] >> 6) & 0xFF) != 0;
	}

	bool ReadLiteral(const std::vector<uint32_t>& words, const Operand& o, uint32_t destination, float& value)
	{
		if (o.type != OPERAND_IMMEDIATE32)
			return false;
		const size_t at = o.indexAt + ((o.token & 3) == 2 ? destination : 0);
		memcpy(&value, &words[at], 4);
		return true;
	}

	// One game shader, with what's needed to follow its registers.
	struct Program
	{
		const std::vector<uint32_t>& words;
		const std::vector<Instruction>& code;
		int first;
		std::vector<std::vector<Operand>> operands;
		std::vector<int> depth;  // How many ifs an instruction is in.

		bool Read()
		{
			operands.resize(code.size());
			depth.assign(code.size(), 0);
			int level = 0;
			for (int i = first; i < int(code.size()); ++i)
			{
				if (code[i].opcode == OP_ENDIF)
					--level;
				depth[i] = level;
				if (code[i].opcode == OP_IF)
					++level;
				if (code[i].opcode != OP_CUSTOMDATA && !OperandsOf(words, code[i], operands[i]))
					return false;
			}
			return true;
		}

		bool Scalar(int i, Slot& out) const
		{
			if (operands[i].empty())
				return false;
			const Operand& d = operands[i][0];
			uint32_t components[4];
			if (d.type != OPERAND_TEMP || d.mode != MODE_MASK || ComponentsOf(d.selection, components) != 1)
				return false;
			out = {d.index0, components[0]};
			return true;
		}

		bool Writes(int i, Slot slot) const
		{
			if (operands[i].empty())
				return false;
			const Operand& d = operands[i][0];
			return d.type == OPERAND_TEMP && d.mode == MODE_MASK && d.index0 == slot.reg && (d.selection >> slot.component) & 1;
		}

		bool ReadsSlot(const Operand& source, uint32_t destination, Slot slot) const
		{
			return source.type == OPERAND_TEMP && source.index0 == slot.reg && ComponentRead(source, destination) == slot.component;
		}

		int LastWriter(int before, Slot slot) const
		{
			for (int i = before - 1; i >= first; --i)
			{
				if (Writes(i, slot))
					return i;
			}
			return -1;
		}

		bool Literal(int i, float value) const
		{
			Slot dest;
			if (!Scalar(i, dest))
				return false;
			for (size_t n = 1; n < operands[i].size(); ++n)
			{
				float found = 0;
				if (ReadLiteral(words, operands[i][n], dest.component, found) && Near(found, value))
					return true;
			}
			return false;
		}

		// The next instruction from `from` of this kind that reads `slot`, while nothing overwrites it. -1 if there's none.
		int NextReader(int from, Slot slot, uint32_t opcode) const
		{
			for (int i = from; i < int(code.size()); ++i)
			{
				Slot dest;
				if (code[i].opcode == opcode && Scalar(i, dest))
				{
					for (size_t n = 1; n < operands[i].size(); ++n)
					{
						if (ReadsSlot(operands[i][n], dest.component, slot))
							return i;
					}
				}
				if (Writes(i, slot))
					return -1;
			}
			return -1;
		}
	};

	enum Change : uint8_t
	{
		KEEP,
		LOBE_IN,
		LOBE_OUT,
		VISIBILITY,
		HOLD_VISIBILITY,  // It comes before the power that Smith needs, so it's finished at the next lobe.
		MIX,
	};

	struct Plan
	{
		std::vector<Change> changes;
		int view = -1;                // The dp3_sat of the view angle.
		size_t normalOperand = 0;     // Which of its operands is the normal. The other is the view.
		int normalDone = -1;          // What finishes the normal: from here on it's what lighting will use.
		int position = -1;            // The input with the camera-to-pixel vector.
		int surfaceNormal = -1;       // The input with the normal from before the normal map, if it can be told.
		std::set<int> powers;         // Each exp of 13 * gloss.
		int mirror = -1;              // The mad with 1 - F0, from a spec map.
		size_t mirrorOperand = 0;
		int probe = -1;               // The sample_l.
		int decode = -1;              // The mul that finishes it.
		std::map<int, size_t> factors;   // Multiplies on the probe's way to the color, and which operand isn't the probe.
		size_t mixProbe = 0, mixTint = 0;
		int omni = -1;                // What adds the omnis' diffuse to the rest.
		std::set<int> finishVisibility;  // Lobes that apply what a HOLD_VISIBILITY held.
		unsigned lobes = 0, visibilities = 0;
	};

	// Follows a vector back through what normalizes and combines it, to the interpolator it started from. Shader families
	// don't agree on which TEXCOORD holds what: weapon camo sends its position in TEXCOORD1, where most send the normal.
	int InputBehind(const Program& p, int before, uint32_t reg, uint32_t component)
	{
		for (int hop = 0; hop < MOST_HOPS; ++hop)
		{
			const int writer = p.LastWriter(before, {reg, component});
			if (writer < 0)
				return -1;
			const std::vector<Operand>& operands = p.operands[writer];
			const Operand* source = nullptr;
			if (p.code[writer].opcode == OP_MAD && operands.size() == 4)
				source = &operands[3];  // N + x * T + y * B: what's being added to.
			else if (p.code[writer].opcode == OP_MOV && operands.size() == 2)
				source = &operands[1];
			else if (p.code[writer].opcode == OP_MUL && operands.size() == 3)
			{
				// Scaled by its inverse length: the vector is the side that isn't one number.
				for (size_t n = 1; n < 3; ++n)
				{
					const Operand& o = operands[n];
					const uint32_t c = o.selection;
					if (o.mode == MODE_SWIZZLE && ((c & 3) != ((c >> 2) & 3) || (c & 3) != ((c >> 4) & 3)))
						source = &o;
				}
			}
			if (!source)
				return -1;
			if (source->type == OPERAND_INPUT)
				return int(source->index0);
			if (source->type != OPERAND_TEMP)
				return -1;
			reg = source->index0;
			component = ComponentRead(*source, component);
			before = writer;
		}
		return -1;
	}

	void FindView(const Program& p, Plan& plan)
	{
		for (int i = p.first; i < int(p.code.size()); ++i)
		{
			Slot dest;
			if (p.code[i].opcode != OP_MUL || !p.Literal(i, VIEW_FALLOFF) || !p.Scalar(i, dest))
				continue;
			for (size_t n = 1; n < p.operands[i].size(); ++n)
			{
				const Operand& source = p.operands[i][n];
				if (source.type != OPERAND_TEMP)
					continue;
				const int writer = p.LastWriter(i, {source.index0, ComponentRead(source, dest.component)});
				if (writer < 0 || p.code[writer].opcode != OP_DP3 || p.operands[writer].size() != 3 || p.depth[writer])
					continue;
				const Operand& a = p.operands[writer][1];
				const Operand& b = p.operands[writer][2];
				if (a.type != OPERAND_TEMP || b.type != OPERAND_TEMP)
					continue;
				// N against -V, whichever way round.
				const bool negativeA = HasModifier(p.words, a), negativeB = HasModifier(p.words, b);
				if (negativeA == negativeB)
					continue;
				plan.view = writer;
				plan.normalOperand = negativeA ? 2 : 1;
				const Operand& normal = p.operands[writer][plan.normalOperand];
				for (uint32_t c = 0; c < 3; ++c)
					plan.normalDone = (std::max)(plan.normalDone, p.LastWriter(writer, {normal.index0, ComponentRead(normal, c)}));
				if (plan.normalDone >= 0 && p.depth[plan.normalDone])
					plan.normalDone = -1;
				const Operand& view = p.operands[writer][negativeA ? 1 : 2];
				plan.position = InputBehind(p, writer, view.index0, ComponentRead(view, 0));
				plan.surfaceNormal = InputBehind(p, writer, normal.index0, ComponentRead(normal, 0));
				return;
			}
		}
	}

	void FindPowers(const Program& p, Plan& plan)
	{
		for (int i = p.first; i < int(p.code.size()); ++i)
		{
			Slot scaled;
			if (p.code[i].opcode != OP_MUL || !p.Literal(i, GLOSS_TO_POWER) || !p.Scalar(i, scaled))
				continue;
			const int power = p.NextReader(i + 1, scaled, OP_EXP);
			if (power >= 0)
				plan.powers.insert(power);
		}
	}

	void FindLobes(const Program& p, Plan& plan)
	{
		for (int i = p.first; i < int(p.code.size()); ++i)
		{
			Slot logarithm, scaled;
			if (p.code[i].opcode != OP_LOG || !p.Scalar(i, logarithm))
				continue;
			const int multiply = p.NextReader(i + 1, logarithm, OP_MUL);
			if (multiply < 0 || !p.Scalar(multiply, scaled))
				continue;
			// The other side of the multiply has to be a power: logs of anything else are other effects.
			bool ofPower = false;
			for (size_t n = 1; n < p.operands[multiply].size(); ++n)
			{
				const Operand& source = p.operands[multiply][n];
				if (source.type != OPERAND_TEMP)
					continue;
				const int writer = p.LastWriter(multiply, {source.index0, ComponentRead(source, scaled.component)});
				ofPower |= plan.powers.count(writer) != 0;
			}
			const int exponent = ofPower ? p.NextReader(multiply + 1, scaled, OP_EXP) : -1;
			if (exponent < 0)
				continue;
			plan.changes[i] = LOBE_IN;
			plan.changes[exponent] = LOBE_OUT;
			++plan.lobes;
		}
	}

	// div V, NoL, Z, where Z comes from either
	//     add Z, a * LoH * LoH - a, l(1.01)         or        add W, -a, l(1.01)   mad Z, LoH * LoH, a, W
	void FindVisibility(const Program& p, Plan& plan)
	{
		for (int i = p.first; i < int(p.code.size()); ++i)
		{
			Slot dest;
			if (p.code[i].opcode != OP_DIV || !p.Scalar(i, dest) || p.operands[i].size() != 3)
				continue;
			const Operand& divisor = p.operands[i][2];
			if (p.operands[i][1].type != OPERAND_TEMP || divisor.type != OPERAND_TEMP)
				continue;
			const int writer = p.LastWriter(i, {divisor.index0, ComponentRead(divisor, dest.component)});
			if (writer < 0)
				continue;
			bool found = p.code[writer].opcode == OP_ADD && p.Literal(writer, VISIBILITY_END);
			Slot writerDest;
			if (!found && p.code[writer].opcode == OP_MAD && p.Scalar(writer, writerDest) && p.operands[writer].size() == 4 && p.operands[writer][3].type == OPERAND_TEMP)
			{
				const Operand& rest = p.operands[writer][3];
				const int restWriter = p.LastWriter(writer, {rest.index0, ComponentRead(rest, writerDest.component)});
				found = restWriter >= 0 && p.code[restWriter].opcode == OP_ADD && p.Literal(restWriter, VISIBILITY_END);
			}
			if (!found)
				continue;
			// The game multiplies V and D, so V can as well be applied to D later on.
			if (*plan.powers.begin() < i)
				plan.changes[i] = VISIBILITY;
			else
			{
				const auto lobe = std::find(plan.changes.begin() + i, plan.changes.end(), LOBE_OUT);
				if (lobe == plan.changes.end())
					continue;
				plan.changes[i] = HOLD_VISIBILITY;
				plan.finishVisibility.insert(int(lobe - plan.changes.begin()));
			}
			++plan.visibilities;
		}
	}

	// mad D, -S, S, l(1, 1, 1, 0): 1 - F0, with S the spec map. The game squares it, so F0 is S * S.
	void FindMirror(const Program& p, Plan& plan)
	{
		for (int i = p.first; i < int(p.code.size()) && plan.mirror < 0; ++i)
		{
			if (p.code[i].opcode != OP_MAD || p.operands[i].size() != 4)
				continue;
			const Operand& a = p.operands[i][1];
			const Operand& b = p.operands[i][2];
			float one = 0;
			if (a.type == OPERAND_TEMP && b.type == OPERAND_TEMP && a.index0 == b.index0 && (a.selection == b.selection) && HasModifier(p.words, a) && !HasModifier(p.words, b) &&
				ReadLiteral(p.words, p.operands[i][3], 0, one) && Near(one, 1))
			{
				plan.mirror = i;
				plan.mirrorOperand = 2;
			}
		}
	}

	struct Color
	{
		uint32_t reg = 0;
		uint32_t components[3] = {};
	};

	bool ColorWritten(const Program& p, int i, Color& out)
	{
		if (p.operands[i].empty())
			return false;
		const Operand& d = p.operands[i][0];
		uint32_t components[4];
		if (d.type != OPERAND_TEMP || d.mode != MODE_MASK || ComponentsOf(d.selection, components) != 3)
			return false;
		out.reg = d.index0;
		memcpy(out.components, components, sizeof(out.components));
		return true;
	}

	// Does this operand of an instruction writing `dest` read `color`, in the same order?
	bool ReadsColor(const Operand& source, const Color& dest, const Color& color)
	{
		if (source.type != OPERAND_TEMP || source.index0 != color.reg)
			return false;
		for (int c = 0; c < 3; ++c)
		{
			if (ComponentRead(source, dest.components[c]) != color.components[c])
				return false;
		}
		return true;
	}

	void FindProbe(const Program& p, Plan& plan)
	{
		for (int i = p.first; i < int(p.code.size()) && plan.probe < 0; ++i)
		{
			if (p.code[i].opcode == OP_SAMPLE_L && p.operands[i].size() == 5 && p.operands[i][2].type == OPERAND_RESOURCE && p.operands[i][2].index0 == PROBE_RESOURCE &&
				p.operands[i][0].type == OPERAND_TEMP)
				plan.probe = i;
		}
		if (plan.probe < 0)
			return;

		// add W, s.w, l(1e-6)   div W, l(1), W   mul R.xyz, W, s.xyz
		const uint32_t sampled = p.operands[plan.probe][0].index0;
		Color color;
		for (int i = plan.probe + 1; i < int(p.code.size()) && i <= plan.probe + 6 && plan.decode < 0; ++i)
		{
			if (p.code[i].opcode != OP_MUL || !ColorWritten(p, i, color))
				continue;
			for (size_t n = 1; n < p.operands[i].size(); ++n)
			{
				if (p.operands[i][n].type == OPERAND_TEMP && p.operands[i][n].index0 == sampled && p.operands[i][n].mode == MODE_SWIZZLE &&
					ComponentRead(p.operands[i][n], color.components[0]) == 0 && ComponentRead(p.operands[i][n], color.components[1]) == 1)
					plan.decode = i;
			}
		}
		if (plan.decode < 0)
		{
			plan.probe = -1;
			return;
		}

		// Now on to the color, through whatever multiplies it. The mad at the end is where its tint comes in.
		std::map<int, size_t> factors;
		for (int i = plan.decode + 1; i < int(p.code.size()); ++i)
		{
			Color dest;
			const bool writesColor = ColorWritten(p, i, dest);
			int reading = 0;
			if (writesColor)
			{
				for (size_t n = 1; n < p.operands[i].size() && n < 3; ++n)
				{
					if (ReadsColor(p.operands[i][n], dest, color))
						reading = int(n);
				}
			}
			if (reading && p.code[i].opcode == OP_MUL)
			{
				factors[i] = reading == 1 ? 2 : 1;
				color = dest;
				continue;
			}
			if (reading && p.code[i].opcode == OP_MAD)
			{
				plan.factors = std::move(factors);
				plan.changes[i] = MIX;
				plan.mixProbe = size_t(reading);
				plan.mixTint = reading == 1 ? 2 : 1;
				return;
			}
			// Anything else that overwrites it, and it isn't a reflection any more that this could follow.
			bool overwritten = false;
			for (int c = 0; c < 3; ++c)
				overwritten |= p.Writes(i, {color.reg, color.components[c]});
			if (overwritten)
				return;
		}
	}

	int EndOfBlock(const std::vector<Instruction>& code, int open)
	{
		int depth = 0;
		for (int i = open; i < int(code.size()); ++i)
		{
			if (code[i].opcode == OP_IF)
				++depth;
			else if (code[i].opcode == OP_ENDIF && --depth == 0)
				return i;
		}
		return -1;
	}

	// if_nz cb3[11].y  <omnis>  endif  movc rA.xyz, cb3[11].yyyy, rB.xyzx, l(0,0,0,0)  mad rC.xyz, albedo, rA.xyz, rest
	void FindOmni(const Program& p, Plan& plan)
	{
		for (int i = p.first; i < int(p.code.size()); ++i)
		{
			if (p.code[i].opcode != OP_IF || p.operands[i].empty())
				continue;
			const Operand& flag = p.operands[i][0];
			if (flag.type != OPERAND_CONSTANT_BUFFER || flag.dimension != 2 || flag.index0 != OBJECT_BUFFER || flag.index1 != LIGHT_FLAGS || (flag.selection & 3) != OMNI_FLAG)
				continue;
			const int close = EndOfBlock(p.code, i);
			if (close < 0 || close + 2 >= int(p.code.size()) || p.code[close + 1].opcode != OP_MOVC || p.code[close + 2].opcode != OP_MAD)
				return;
			Color diffuse, total;
			if (!ColorWritten(p, close + 1, diffuse) || !ColorWritten(p, close + 2, total))
				return;
			for (size_t n = 1; n < 3; ++n)
			{
				if (ReadsColor(p.operands[close + 2][n], total, diffuse))
					plan.omni = close + 2;
			}
			return;
		}
	}

	// Registers of the patch's own, above the game's. rSurface and on keep what they hold from where it's found to where
	// it's used, so nothing has to survive in the game's.
	struct Registers
	{
		uint32_t surface, normal, mirror, direction, mip, screen, held, in0, in1, out0, out1, work;
	};

	class Writer
	{
	public:
		Writer(const std::vector<uint32_t>& words, std::vector<uint32_t>& out) : Words(words), Out(out) {}

		// Copies an operand, reading the given component in each position.
		void Source(const Operand& o, const uint32_t components[4])
		{
			const size_t start = Out.size();
			Out.insert(Out.end(), Words.begin() + o.at, Words.begin() + o.at + o.size);
			if (o.type == OPERAND_IMMEDIATE32)
			{
				if ((o.token & 3) == 2)
				{
					const size_t values = start + (o.indexAt - o.at);
					for (int c = 0; c < 4; ++c)
						Out[values + c] = Words[o.indexAt + components[c]];
				}
				return;
			}
			const uint32_t swizzle = components[0] | (components[1] << 2) | (components[2] << 4) | (components[3] << 6);
			Out[start] = (o.token & ~((3u << 2) | (0xFFu << 4))) | (MODE_SWIZZLE << 2) | (swizzle << 4);
		}

		// What a scalar instruction read through `source`, as the x of a register.
		void MoveScalar(uint32_t reg, const Operand& source, uint32_t destination)
		{
			const size_t start = Begin(OP_MOV);
			Temp(MODE_MASK, 1, reg);
			const uint32_t c = ComponentRead(source, destination);
			const uint32_t components[4] = {c, c, c, c};
			Source(source, components);
			End(start);
		}

		// The three components an instruction reads through `source`, for a destination writing `written`, into rgb.
		void ColorSource(const Operand& source, const uint32_t written[3])
		{
			uint32_t components[4];
			for (int c = 0; c < 3; ++c)
				components[c] = ComponentRead(source, written[c]);
			components[3] = components[2];
			Source(source, components);
		}

		void MoveWhole(uint32_t reg, uint32_t mask, const Operand& source)
		{
			const size_t start = Begin(OP_MOV);
			Temp(MODE_MASK, mask, reg);
			Out.insert(Out.end(), Words.begin() + source.at, Words.begin() + source.at + source.size);
			End(start);
		}

		void MoveRegister(uint32_t to, uint32_t from)
		{
			const size_t start = Begin(OP_MOV);
			Temp(MODE_MASK, MASK_XYZW, to);
			Temp(MODE_SWIZZLE, SWIZZLE_XYZW, from);
			End(start);
		}

		void MoveTemp(uint32_t to, uint32_t mask, uint32_t from, uint32_t swizzle)
		{
			const size_t start = Begin(OP_MOV);
			Temp(MODE_MASK, mask, to);
			Temp(MODE_SWIZZLE, swizzle, from);
			End(start);
		}

		void MoveLiteral(uint32_t reg, float value)
		{
			uint32_t bits;
			memcpy(&bits, &value, 4);
			const size_t start = Begin(OP_MOV);
			Temp(MODE_MASK, MASK_XYZW, reg);
			Out.push_back(2 | (OPERAND_IMMEDIATE32 << 12));
			Out.insert(Out.end(), {bits, bits, bits, bits});
			End(start);
		}

		// mov <a game destination>, rFrom.x, keeping the instruction's saturate.
		void Store(const Operand& destination, uint32_t from, uint32_t token)
		{
			const size_t start = Begin(OP_MOV | (token & SATURATE));
			Out.insert(Out.end(), Words.begin() + destination.at, Words.begin() + destination.at + destination.size);
			Temp(MODE_SWIZZLE, 0, from);
			End(start);
		}

		// Swizzle of rgb that puts each of them in the component a mask writes.
		static uint32_t IntoMask(const uint32_t written[3])
		{
			uint32_t swizzle = 0;
			for (uint32_t c = 0; c < 3; ++c)
				swizzle |= c << (written[c] * 2);
			return swizzle;
		}

		void StoreColor(const Operand& destination, uint32_t from, uint32_t token, const uint32_t written[3])
		{
			const size_t start = Begin(OP_MOV | (token & SATURATE));
			Out.insert(Out.end(), Words.begin() + destination.at, Words.begin() + destination.at + destination.size);
			Temp(MODE_SWIZZLE, IntoMask(written), from);
			End(start);
		}

		// add <destination>, rFrom, <operand>
		void AddColor(const Operand& destination, uint32_t from, const Operand& other, uint32_t token, const uint32_t written[3])
		{
			const size_t start = Begin(OP_ADD | (token & SATURATE));
			Out.insert(Out.end(), Words.begin() + destination.at, Words.begin() + destination.at + destination.size);
			Temp(MODE_SWIZZLE, IntoMask(written), from);
			Out.insert(Out.end(), Words.begin() + other.at, Words.begin() + other.at + other.size);
			End(start);
		}

		// add <destination>, rFrom, <destination>, for a destination writing rgb in `written`.
		void AddToItself(const Operand& destination, uint32_t from, const uint32_t written[3])
		{
			const size_t start = Begin(OP_ADD);
			Out.insert(Out.end(), Words.begin() + destination.at, Words.begin() + destination.at + destination.size);
			Temp(MODE_SWIZZLE, IntoMask(written), from);
			Temp(MODE_SWIZZLE, SWIZZLE_XYZW, destination.index0);
			End(start);
		}

		// mul <destination>, <its own component>, rBy.x
		void ScaleScalar(const Operand& destination, uint32_t component, uint32_t by)
		{
			const size_t start = Begin(OP_MUL);
			Out.insert(Out.end(), Words.begin() + destination.at, Words.begin() + destination.at + destination.size);
			Temp(MODE_SWIZZLE, component * 0x55, destination.index0);
			Temp(MODE_SWIZZLE, 0, by);
			End(start);
		}

		// mul rDest.xyz, a, b, both in rgb order.
		void MultiplyColors(uint32_t reg, const Operand& a, const Operand& b, const uint32_t written[3])
		{
			const size_t start = Begin(OP_MUL);
			Temp(MODE_MASK, MASK_XYZ, reg);
			ColorSource(a, written);
			ColorSource(b, written);
			End(start);
		}

		// mul rReg.xyz, rReg.xyz, factor
		void ScaleColor(uint32_t reg, const Operand& factor, const uint32_t written[3])
		{
			const size_t start = Begin(OP_MUL);
			Temp(MODE_MASK, MASK_XYZ, reg);
			Temp(MODE_SWIZZLE, SWIZZLE_XYZW, reg);
			ColorSource(factor, written);
			End(start);
		}

		void Copy(const Instruction& ins)
		{
			Out.insert(Out.end(), Words.begin() + ins.at, Words.begin() + ins.at + ins.length);
		}

		bool Splice(Site site, const std::vector<std::pair<uint32_t, uint32_t>>& inputs, const std::vector<uint32_t>& outputs, uint32_t work)
		{
			const Snippet& snippet = Snippets()[site];
			std::vector<Operand> operands;
			for (const Instruction& ins : snippet.code)
			{
				if (IsDeclaration(ins.opcode) || ins.opcode == OP_RET)
					continue;
				if (!OperandsOf(snippet.words, ins, operands))
					return false;
				const size_t start = Out.size();
				Out.insert(Out.end(), snippet.words.begin() + ins.at, snippet.words.begin() + ins.operandsAt);
				for (const Operand& o : operands)
				{
					const auto retarget = [&](uint32_t type, uint32_t index)
					{
						Out.push_back((o.token & ~(0xFFu << 12)) | (type << 12));
						Out.insert(Out.end(), snippet.words.begin() + o.at + 1, snippet.words.begin() + o.indexAt);
						Out.push_back(index);
					};
					if (o.type == OPERAND_TEMP)
						retarget(OPERAND_TEMP, work + o.index0);
					else if (o.type == OPERAND_INPUT)
					{
						if (o.index0 >= inputs.size())
							return false;
						retarget(inputs[o.index0].first, inputs[o.index0].second);
					}
					else if (o.type == OPERAND_OUTPUT)
					{
						if (o.index0 >= outputs.size())
							return false;
						retarget(OPERAND_TEMP, outputs[o.index0]);
					}
					else
						Out.insert(Out.end(), snippet.words.begin() + o.at, snippet.words.begin() + o.at + o.size);
				}
				Out[start] = WithLength(Out[start], Out.size() - start);
			}
			return true;
		}

	private:
		const std::vector<uint32_t>& Words;
		std::vector<uint32_t>& Out;

		size_t Begin(uint32_t token)
		{
			Out.push_back(token);
			return Out.size() - 1;
		}

		void End(size_t start)
		{
			Out[start] = WithLength(Out[start], Out.size() - start);
		}

		void Temp(uint32_t mode, uint32_t selection, uint32_t reg)
		{
			Out.push_back(TempToken(mode, selection));
			Out.push_back(reg);
		}
	};
}

Result Patch(const uint8_t* shader, size_t size)
{
	Result result;
	std::vector<Chunk> chunks;
	std::vector<uint32_t> words;
	std::vector<Instruction> code;
	if (!ReadChunks(shader, size, chunks))
	{
		result.error = "not a shader container";
		return result;
	}
	if (!ReadProgram(chunks, words) || !ReadInstructions(words, code))
	{
		result.error = "unreadable program";
		return result;
	}
	if (((words[0] >> 16) & 0xFFFF) != 0 || ((words[0] >> 4) & 15) != 4)
	{
		result.error = "not a shader model 4 pixel shader";
		return result;
	}
	for (const Snippet& snippet : Snippets())
	{
		if (!snippet.ok)
		{
			result.error = "unreadable snippet";
			return result;
		}
	}

	int firstCode = -1, temps = -1;
	for (int i = 0; i < int(code.size()); ++i)
	{
		if (code[i].opcode == OP_DCL_TEMPS)
			temps = i;
		if (!IsDeclaration(code[i].opcode))
		{
			firstCode = i;
			break;
		}
	}
	if (temps < 0 || firstCode < 0)
	{
		result.ok = true;  // No temps, no lighting.
		return result;
	}

	Program program{words, code, firstCode};
	if (!program.Read())
	{
		result.error = "unreadable instruction";
		return result;
	}

	Plan plan;
	plan.changes.assign(code.size(), KEEP);
	FindView(program, plan);
	FindPowers(program, plan);
	FindLobes(program, plan);
	FindMirror(program, plan);
	FindProbe(program, plan);
	FindOmni(program, plan);

	// A power has to come after the finished normal it needs. Five shaders in the game don't: they do without what depends
	// on it, and keep the rest. The omnis also need it known outside any light's block.
	bool surfaceFirst = plan.normalDone >= 0 && plan.position >= 0 && !plan.powers.empty();
	bool powersOutside = true;
	for (const int power : plan.powers)
	{
		surfaceFirst = surfaceFirst && power > plan.normalDone;
		powersOutside = powersOutside && !program.depth[power];
	}
	if (surfaceFirst)
		FindVisibility(program, plan);
	if (!surfaceFirst || !powersOutside || (plan.mirror >= 0 && (plan.mirror < plan.normalDone || program.depth[plan.mirror])))
		plan.omni = -1;
	if (plan.position < 0)
	{
		plan.probe = plan.decode = -1;
		plan.factors.clear();
		std::replace(plan.changes.begin(), plan.changes.end(), MIX, KEEP);
	}

	result.lobes = plan.lobes;
	result.visibilities = plan.visibilities;
	result.widened = surfaceFirst;
	result.probe = plan.probe >= 0;
	result.screen = std::find(plan.changes.begin(), plan.changes.end(), MIX) != plan.changes.end();
	result.omni = plan.omni >= 0;
	result.ok = true;
	if (!plan.lobes && !result.probe && !result.omni)
		return result;

	WidenConstants(words, code, SCENE_BUFFER, SCENE_VECTORS_READ);

	const uint32_t gameTemps = words[code[temps].at + 1];
	uint32_t snippetTemps = 0;
	for (const Snippet& snippet : Snippets())
		snippetTemps = (std::max)(snippetTemps, snippet.temps);
	Registers r{};
	r.surface = gameTemps;
	r.normal = gameTemps + 1;
	r.mirror = gameTemps + 2;
	r.direction = gameTemps + 3;
	r.mip = gameTemps + 4;
	r.screen = gameTemps + 5;
	r.held = gameTemps + 6;
	r.in0 = gameTemps + 7;
	r.in1 = gameTemps + 8;
	r.out0 = gameTemps + 9;
	r.out1 = gameTemps + 10;
	r.work = gameTemps + 11;

	std::vector<uint32_t> out;
	out.reserve(words.size() * 2 + 2048);
	out.insert(out.end(), words.begin(), words.begin() + 2);
	Writer writer(words, out);

	// The declarations, with what's new in front of dcl_temps.
	for (int i = 0; i < firstCode; ++i)
	{
		if (code[i].opcode == OP_DCL_TEMPS)
		{
			out.push_back(OP_DCL_CONSTANT_BUFFER | (4u << 24));
			out.push_back(2 | (MODE_SWIZZLE << 2) | (SWIZZLE_XYZW << 4) | (OPERAND_CONSTANT_BUFFER << 12) | (2u << 20));
			out.push_back(CONSTANT_SLOT);
			out.push_back(CONSTANT_VECTORS);
			if (result.screen || result.probe)
			{
				const Snippet& reflect = Snippets()[SITE_REFLECT];
				for (const Instruction& ins : reflect.code)
				{
					if (ins.opcode == OP_DCL_RESOURCE)
						out.insert(out.end(), reflect.words.begin() + ins.at, reflect.words.begin() + ins.at + ins.length);
				}
			}
			out.push_back(OP_DCL_TEMPS | (2u << 24));
			out.push_back(r.work + snippetTemps);
			continue;
		}
		writer.Copy(code[i]);
	}

	const auto fail = [&](const char* what)
	{
		result.ok = false;
		result.changed = false;
		result.error = what;
		return result;
	};

	for (int i = firstCode; i < int(code.size()); ++i)
	{
		const Instruction& ins = code[i];
		const std::vector<Operand>& operands = program.operands[i];
		if (ins.opcode == OP_CUSTOMDATA)
		{
			writer.Copy(ins);
			continue;
		}
		uint32_t written[4] = {};
		if (!operands.empty() && operands[0].type == OPERAND_TEMP && operands[0].mode == MODE_MASK)
			ComponentsOf(operands[0].selection, written);

		// Before the instruction, since it may overwrite what they read.
		if (i == plan.mirror && plan.omni >= 0)
			writer.MultiplyColors(r.mirror, operands[plan.mirrorOperand], operands[plan.mirrorOperand], written);
		if (i == plan.probe)
		{
			writer.MoveWhole(r.direction, MASK_XYZ, operands[1]);
			writer.MoveScalar(r.mip, operands[4], 0);
		}
		const auto factor = plan.factors.find(i);
		if (factor != plan.factors.end())
			writer.ScaleColor(r.screen, operands[factor->second], written);

		switch (plan.changes[i])
		{
		case LOBE_IN:
			writer.MoveScalar(r.in0, operands[1], written[0]);
			if (!writer.Splice(SITE_LOBE_IN, {{OPERAND_TEMP, r.in0}}, {r.out0}, r.work))
				return fail("snippet didn't fit");
			writer.Store(operands[0], r.out0, words[ins.at]);
			break;
		case LOBE_OUT:
			writer.MoveScalar(r.in0, operands[1], written[0]);
			if (!writer.Splice(SITE_LOBE_OUT, {{OPERAND_TEMP, r.in0}}, {r.out0}, r.work))
				return fail("snippet didn't fit");
			writer.Store(operands[0], r.out0, words[ins.at]);
			if (plan.finishVisibility.count(i))
			{
				if (!writer.Splice(SITE_VISIBILITY, {{OPERAND_TEMP, r.held}, {OPERAND_TEMP, r.surface}}, {r.out0}, r.work))
					return fail("snippet didn't fit");
				writer.ScaleScalar(operands[0], written[0], r.out0);
			}
			break;
		case HOLD_VISIBILITY:
			writer.MoveScalar(r.held, operands[1], written[0]);
			writer.MoveLiteral(r.out0, 1);
			writer.Store(operands[0], r.out0, 0);
			break;
		case VISIBILITY:
			writer.MoveScalar(r.in0, operands[1], written[0]);
			if (!writer.Splice(SITE_VISIBILITY, {{OPERAND_TEMP, r.in0}, {OPERAND_TEMP, r.surface}}, {r.out0}, r.work))
				return fail("snippet didn't fit");
			writer.Store(operands[0], r.out0, words[ins.at]);
			break;
		case MIX:
			writer.MultiplyColors(r.in0, operands[plan.mixProbe], operands[plan.mixTint], written);
			if (!writer.Splice(SITE_MIX, {{OPERAND_TEMP, r.in0}, {OPERAND_TEMP, r.screen}}, {r.out0}, r.work))
				return fail("snippet didn't fit");
			writer.AddColor(operands[0], r.out0, operands[3], words[ins.at], written);
			break;
		default:
			writer.Copy(ins);
		}

		// After it.
		if (i == plan.normalDone && surfaceFirst)
		{
			writer.MoveWhole(r.normal, MASK_XYZ, program.operands[plan.view][plan.normalOperand]);
			writer.MoveLiteral(r.mirror, DIELECTRIC);
			if (!writer.Splice(SITE_SURFACE, {{OPERAND_TEMP, r.normal}, {OPERAND_INPUT, uint32_t(plan.position)}}, {r.out0}, r.work))
				return fail("snippet didn't fit");
			writer.MoveRegister(r.surface, r.out0);
		}
		if (plan.powers.count(i) && surfaceFirst)
		{
			writer.MoveTemp(r.in0, 1, operands[0].index0, written[0] * 0x55);
			if (!writer.Splice(SITE_POWER, {{OPERAND_TEMP, r.in0}, {OPERAND_TEMP, r.surface}}, {r.out0, r.out1}, r.work))
				return fail("snippet didn't fit");
			writer.Store(operands[0], r.out0, 0);
			writer.MoveRegister(r.surface, r.out1);
		}
		if (i == plan.decode)
		{
			writer.MoveTemp(r.in0, MASK_XYZ, operands[0].index0, written[0] | (written[1] << 2) | (written[2] << 4) | (written[2] << 6));
			// With the normal unknown, the bent one: nothing fades, which is what the game does.
			const std::pair<uint32_t, uint32_t> flatNormal = plan.surfaceNormal >= 0 ? std::make_pair(uint32_t(OPERAND_INPUT), uint32_t(plan.surfaceNormal)) :
				std::make_pair(uint32_t(OPERAND_TEMP), r.direction);
			if (!writer.Splice(SITE_REFLECT, {{OPERAND_TEMP, r.in0}, {OPERAND_TEMP, r.direction}, {OPERAND_TEMP, r.mip}, {OPERAND_INPUT, uint32_t(plan.position)}, flatNormal},
					{r.out0, r.out1}, r.work))
				return fail("snippet didn't fit");
			writer.StoreColor(operands[0], r.out0, 0, written);
			writer.MoveRegister(r.screen, r.out1);
		}
		if (i == plan.omni)
		{
			if (!writer.Splice(SITE_OMNI, {{OPERAND_TEMP, r.normal}, {OPERAND_INPUT, uint32_t(plan.position)}, {OPERAND_TEMP, r.surface}, {OPERAND_TEMP, r.mirror}}, {r.out0}, r.work))
				return fail("snippet didn't fit");
			writer.AddToItself(operands[0], r.out0, written);
		}
	}

	out[1] = uint32_t(out.size());
	result.shader = BuildContainer(chunks, out);
	result.changed = true;
	return result;
}
}
