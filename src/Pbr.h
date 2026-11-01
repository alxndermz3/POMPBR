#pragma once

// Puts Brdf's patch on every lit shader of every material. Unlike POM, which is one surface's business and works on copies,
// this is the game's lighting, so the game's own shader records are changed in place: a shader's object, code and size.
// That's three writes per shader, undone just as easily, and whatever reads a material's code afterwards gets the patched
// one, which is how POM's copies end up with it. So they have to be made after this, and remade around a change.
//
// Runs on the game's thread, from Engine's per-view check.
namespace Pom::Pbr
{
	// Patches what's not patched yet and lets go of what's gone. Shaders in zones that stayed loaded are kept.
	void Refresh();
	// Puts the game's own shaders back. What was made stays around, to go back on at once.
	void Off();
	bool IsOn();
	// The game replaced shader objects, which a vid_restart does.
	bool Lost();
}
