/*
 * Copyright (C) 2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  patch.c
 * @brief Runtime patches applied to the game binary.
 */

#include <kubridge.h>
#include <so_util/so_util.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vitasdk.h>

#ifdef __cplusplus
extern "C"
{
#endif
	extern so_module so_mod;
#ifdef __cplusplus
};
#endif

#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX (0x0C20D050)

#include "utils/logger.h"
#include "utils/dialog.h"
#include "reimpl/sys.h"
#include <stdbool.h>

// Executable scratch space for our trampolines. kuser_patch() maps the page and
// uses 0xFA0/0xFC0; everything below 0xF00 is ours.
#define CAVE_BASE 0x9A000200

// Trampolines for swp_patches(), one per site, above the ones in CAVE_BASE.
#define SWP_CAVE_BASE 0x9A000400
#define SWP_CAVE_END  0x9A000F00

static void swp_patches(void);
static void savegame_patches(void);
static void nullzone_patches(void);
static void checkpoint_patches(void);
static void hud_patches(void);

void __kuser_memory_barrier(void) {
	__sync_synchronize();
}

void kuser_patch(void) {
	SceKernelAllocMemBlockKernelOpt opt;
	memset(&opt, 0, sizeof(SceKernelAllocMemBlockKernelOpt));
	opt.size = sizeof(SceKernelAllocMemBlockKernelOpt);
	opt.attr = 0x1;
	opt.field_C = (SceUInt32)0x9A000000;
	if (kuKernelAllocMemBlock("atomic", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX, 0x1000, &opt) < 0)
		fatal_error("Error could not allocate atomic block.");
	kuKernelMemProtect((void *)0x9A000000, (SceSize)0x1000, KU_KERNEL_PROT_EXEC | KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE);

	hook_addr(0x9A000FA0, (uintptr_t)__kuser_memory_barrier);
	hook_addr(0x9A000FC0, (uintptr_t)__atomic_cmpxchg);

	uint32_t patched_addr;
	for (uint32_t addr = so_mod.text_base; addr < so_mod.text_base + so_mod.text_size; addr += 4) {
		uint32_t *a = (uint32_t *)addr;
		if (*a == 0xFFFF0FC0) {
			l_debug("Patching 0x%x -> __kuser_cmpxchg", a);
			patched_addr = 0x9A000FC0;
			kuKernelCpuUnrestrictedMemcpy((void *)(addr), &patched_addr, sizeof(uint32_t));
		}
		else if (*a == 0xFFFF0FA0) {
			l_debug("Patching 0x%x -> __kuser_memory_barrier", a);
			patched_addr = 0x9A000FA0;
			kuKernelCpuUnrestrictedMemcpy((void *)(addr), &patched_addr, sizeof(uint32_t));
		}
	}
}

void so_patch(void) {
	kuser_patch();
	swp_patches();
	savegame_patches();
	nullzone_patches();
	checkpoint_patches();
	hud_patches();
}

/* ------------------------------------------------------------------------- *
 * Patch helpers
 * ------------------------------------------------------------------------- */

// ARM branch encoding: offset is (target - (pc + 8)) >> 2.
static uint32_t arm_branch(uintptr_t from, uintptr_t to, uint32_t cond) {
	int32_t off = ((int32_t)(to - (from + 8))) >> 2;
	return cond | ((uint32_t)off & 0x00FFFFFF);
}

/*
 * Every patch below is a byte offset into a function of one specific build of
 * the game. On any other build those offsets land on unrelated instructions and
 * we would silently corrupt the binary, which is impossible to diagnose from a
 * crash. So each site states the instruction it expects to overwrite, and we
 * check it first: a mismatch means the data does not match this port, and it is
 * far better to say so than to scribble over the code and crash later.
 */
static void expect_insn(const char *what, uintptr_t at, uint32_t expected) {
	uint32_t found = *(volatile uint32_t *)at;
	if (found != expected) {
		fatal_error("Unsupported game version.\n\n"
		            "%s: expected %08X at %p, found %08X.\n\n"
		            "Your game files are from a different build of 9mm HD than\n"
		            "this port was made for.",
		            what, expected, (void *)at, found);
	}
}

#define B_AL 0xEA000000u
#define B_EQ 0x0A000000u

/*
 * Most of the crashes below share one shape: the engine loads through a pointer
 * that is briefly null while a level streams in. Each site already has a branch
 * for the "nothing to iterate" case, so we test the register and jump there.
 *
 * The load can't be widened in place, so it moves into a trampoline:
 *
 *      cmp   reg, #0
 *      beq   bail_out
 *      <original load>
 *      b     next_instruction
 *
 * Offsets are relative to the function start so they survive relocation.
 */
static void null_guard(const char *name, const char *symbol, uint32_t at_off,
                       uint32_t bail_off, uint32_t cmp_insn, uint32_t load_insn,
                       uintptr_t cave) {
	uintptr_t fn = (uintptr_t)so_symbol(&so_mod, symbol);
	if (!fn) {
		l_error("null_guard: %s not found", name);
		return;
	}
	uintptr_t at = fn + at_off;
	expect_insn(name, at, load_insn);

	uint32_t tramp[4] = {
		cmp_insn,
		arm_branch(cave + 4, fn + bail_off, B_EQ),
		load_insn,
		arm_branch(cave + 12, at + 4, B_AL),
	};
	kuKernelCpuUnrestrictedMemcpy((void *)cave, tramp, sizeof(tramp));

	uint32_t br = arm_branch(at, cave, B_AL);
	kuKernelCpuUnrestrictedMemcpy((void *)at, &br, 4);
	l_debug("patched %s @ %p", name, (void *)at);
}

/* ------------------------------------------------------------------------- *
 * SWP
 *
 * glitch::io::CMemoryReadFile guards the refcount of its shared buffer with a
 * spinlock built on SWP (ctor, dtor and clone). SWP is deprecated on ARMv7 and
 * the Vita doesn't enable it, so the first memory file that shares its buffer
 * dies with an undefined instruction exception.
 *
 * Each one is sent to a trampoline doing the same swap with LDREX/STREX:
 *
 *      push  {a, b, c}
 *      mrs   b, APSR
 *      dmb   ish
 *   1: ldrex c, [Rn]
 *      strex a, Rt2, [Rn]
 *      cmp   a, #0
 *      bne   1b
 *      dmb   ish
 *      msr   APSR_nzcvq, b
 *      mov   Rt, c
 *      pop   {a, b, c}
 *      b     next_instruction
 *
 * Going through c keeps swp Rt, Rt, [Rn] correct. Every SWP in the game is a
 * spinlock test followed by a cmp on Rt; requiring that keeps us from rewriting
 * a literal pool word that happens to decode as one.
 * ------------------------------------------------------------------------- */
static void swp_patches(void) {
	uintptr_t cave = SWP_CAVE_BASE;
	int count = 0;

	for (uintptr_t addr = so_mod.text_base; addr + 4 < so_mod.text_base + so_mod.text_size; addr += 4) {
		uint32_t insn = *(uint32_t *)addr;
		if ((insn & 0x0FB00FF0) != 0x01000090)
			continue;

		uint32_t cond = insn & 0xF0000000;
		uint32_t byte = insn & 0x00400000;
		uint32_t rn  = (insn >> 16) & 0xF;
		uint32_t rt  = (insn >> 12) & 0xF;
		uint32_t rt2 = insn & 0xF;

		uint32_t next = *(uint32_t *)(addr + 4);
		// cmp Rt, #imm or cmp Rt, Rm
		if (cond == 0xF0000000 || (next & 0x0DFF0000) != (0x01500000 | (rt << 16)))
			continue;
		if (rn >= 13 || rt >= 13 || rt2 >= 13) {
			l_error("swp_patches: unhandled SWP %08X at %p", insn, (void *)addr);
			continue;
		}
		if (cave + 12 * 4 > SWP_CAVE_END)
			fatal_error("Too many SWP instructions to patch (%d).", count);

		uint32_t scratch[3], n = 0;
		for (uint32_t r = 0; n < 3; r++)
			if (r != rn && r != rt && r != rt2)
				scratch[n++] = r;
		uint32_t a = scratch[0], b = scratch[1], c = scratch[2];
		uint32_t regs = (1 << a) | (1 << b) | (1 << c);

		uint32_t tramp[12] = {
			0xE92D0000 | regs,                                       // push {a, b, c}
			0xE10F0000 | (b << 12),                                  // mrs b, APSR
			0xF57FF05B,                                              // dmb ish
			(byte ? 0xE1D00F9F : 0xE1900F9F) | (rn << 16) | (c << 12), // ldrex(b) c, [Rn]
			(byte ? 0xE1C00F90 : 0xE1800F90) | (rn << 16) | (a << 12) | rt2, // strex(b) a, Rt2, [Rn]
			0xE3500000 | (a << 16),                                  // cmp a, #0
			0,                                                       // bne ldrex
			0xF57FF05B,                                              // dmb ish
			0xE128F000 | b,                                          // msr APSR_nzcvq, b
			0xE1A00000 | (rt << 12) | c,                             // mov Rt, c
			0xE8BD0000 | regs,                                       // pop {a, b, c}
			0,                                                       // b next
		};
		tramp[6]  = arm_branch(cave + 6 * 4, cave + 3 * 4, 0x1A000000);
		tramp[11] = arm_branch(cave + 11 * 4, addr + 4, B_AL);
		kuKernelCpuUnrestrictedMemcpy((void *)cave, tramp, sizeof(tramp));
		kuKernelFlushCaches((void *)cave, sizeof(tramp));

		// Same condition as the SWP, so a skipped one stays skipped.
		uint32_t br = arm_branch(addr, cave, cond | 0x0A000000);
		kuKernelCpuUnrestrictedMemcpy((void *)addr, &br, 4);

		cave += sizeof(tramp);
		count++;
	}

	l_debug("swp_patches: replaced %d SWP instructions", count);
}

/* ------------------------------------------------------------------------- *
 * Savegame deserialisation
 *
 * CGameObject::SaveLoad walks the components stored in a save block and matches
 * them against the ones the object actually has. Two ways it walks off the rails
 * when the two don't line up, both of which Android gets away with:
 *
 *   - a saved component that no longer exists leaves the search index at -1,
 *     and the next iteration reads components[-1];
 *   - the forward lookup reads components[0] without checking that the vector
 *     is non-empty.
 *
 * CZone::SaveLoad has the same forward-read bug on its object vector.
 * ------------------------------------------------------------------------- */
static void savegame_patches(void) {
	uintptr_t sl = (uintptr_t)so_symbol(&so_mod, "_ZN11CGameObject8SaveLoadEP13CMemoryStream");
	if (sl) {
		// After SkipBlock, reset the index to 0 instead of looping with -1.
		uintptr_t at = sl + 0x1c8;
		uintptr_t cave = CAVE_BASE;
		expect_insn("CGameObject::SaveLoad (loop)", at, 0xEAFFFFC2);  // b <loop top>
		uint32_t tramp[3] = {
			0xE3A06000,          // mov r6, #0
			0xE51FF004,          // ldr pc, [pc, #-4]
			(uint32_t)(sl + 0xd8),
		};
		kuKernelCpuUnrestrictedMemcpy((void *)cave, tramp, sizeof(tramp));
		uint32_t br = arm_branch(at, cave, B_AL);
		kuKernelCpuUnrestrictedMemcpy((void *)at, &br, 4);

		// Empty component vector: fall through to the backward scan, which
		// already handles a count of zero by skipping the block.
		null_guard("CGameObject::SaveLoad (empty vector)",
		           "_ZN11CGameObject8SaveLoadEP13CMemoryStream",
		           0x100, 0x120, 0xE3530000 /* cmp r3, #0 */,
		           0xE7933106 /* ldr r3, [r3, r6, lsl #2] */, CAVE_BASE + 0x40);
	}

	null_guard("CZone::SaveLoad (empty vector)",
	           "_ZN5CZone8SaveLoadEP13CMemoryStream",
	           0xa8, 0xb8, 0xE3500000 /* cmp r0, #0 */,
	           0xE790A003 /* ldr sl, [r0, r3] */, CAVE_BASE + 0x20);

	// CZone::SetBatchVisibility logs that its batch manager is null and then
	// dereferences it anyway. Turn the "is it null" branch into an exit.
	uintptr_t bv = (uintptr_t)so_symbol(&so_mod,
		"_ZN5CZone18SetBatchVisibilityERKSt6vectorIPvSaIS1_EEb");
	if (bv) {
		expect_insn("CZone::SetBatchVisibility", bv + 0x18, 0x0A000014);  // beq <log>
		uint32_t br = B_EQ | 0x13;   // beq <epilogue>
		kuKernelCpuUnrestrictedMemcpy((void *)(bv + 0x18), &br, 4);
	}
}

/* ------------------------------------------------------------------------- *
 * Null zones during level streaming
 *
 * Levels are built one zone at a time while rendering and gameplay are already
 * running, so for a few frames the player has no zone and portals can point at
 * zones that don't exist yet. The engine notices in some of these spots -- it
 * logs the null pointer -- but then carries on and reads through it anyway.
 *
 * SetVisible is the important one: it kills the level build a few zones in.
 * ------------------------------------------------------------------------- */
static void nullzone_patches(void) {
	// Portal leading to a zone that hasn't been created yet: skip the portal.
	null_guard("CZone::SetVisible",
	           "_ZN5CZone10SetVisibleEPKN6glitch5scene12SViewFrustumEPK11CZonePortalRSt6vectorISt4pairIPKS_S9_IS7_S7_EESaISD_EE",
	           0x1f0, 0x540, 0xE3540000 /* cmp r4, #0 */,
	           0xE5947104 /* ldr r7, [r4, #0x104] */, CAVE_BASE + 0xe0);

	// Two identical sites where the null zone is read as a log argument.
	null_guard("CZonesManager::UpdateVisibility (a)",
	           "_ZN13CZonesManager16UpdateVisibilityEPKN6glitch5scene12SViewFrustumE",
	           0xa4, 0xac, 0xE3530000, 0xE5933044, CAVE_BASE + 0xc0);
	null_guard("CZonesManager::UpdateVisibility (b)",
	           "_ZN13CZonesManager16UpdateVisibilityEPKN6glitch5scene12SViewFrustumE",
	           0xf0, 0xfc, 0xE3530000, 0xE5933044, CAVE_BASE + 0x100);

	// Both walk the object list of the player's zone before he has one.
	null_guard("PlayerComponent::MoveTo",
	           "_ZN15PlayerComponent6MoveToERKN6glitch4core8vector3dIfEE",
	           0x270, 0x458, 0xE3560000 /* cmp r6, #0 */,
	           0xE5965054 /* ldr r5, [r6, #0x54] */, CAVE_BASE + 0x60);
	null_guard("PlayerComponent::CheckJumpZones",
	           "_ZN15PlayerComponent14CheckJumpZonesEv",
	           0x30, 0xc4, 0xE3570000 /* cmp r7, #0 */,
	           0xE5972058 /* ldr r2, [r7, #0x58] */, CAVE_BASE + 0xa0);

	// Called with a null `this`; bail out before the prologue touches anything.
	uintptr_t cz = (uintptr_t)so_symbol(&so_mod,
		"_ZN5CZone16CheckChangedZoneERKN6glitch4core8vector3dIfEES5_PK11CZonePortal");
	if (cz) {
		uintptr_t cave = CAVE_BASE + 0x80;
		expect_insn("CZone::CheckChangedZone", cz, 0xE92D4FF0);  // push {r4-r9, sl, fp, lr}
		uint32_t tramp[4] = {
			0xE3500000,          // cmp r0, #0
			0x012FFF1E,          // bxeq lr
			0xE92D4FF0,          // push {r4-r9, sl, fp, lr}
			arm_branch(cave + 12, cz + 4, B_AL),
		};
		kuKernelCpuUnrestrictedMemcpy((void *)cave, tramp, sizeof(tramp));
		uint32_t br = arm_branch(cz, cave, B_AL);
		kuKernelCpuUnrestrictedMemcpy((void *)cz, &br, 4);
	}
}

/* ------------------------------------------------------------------------- *
 * Checkpoints
 *
 * CLevel::Init restores a checkpoint if one exists, but right after a level
 * change the file on disk still describes the *previous* level. Its zone and
 * object ids mean nothing in the new level, so the restore half-fails and the
 * player ends up without a zone: invisible, falling through the world.
 *
 * The engine is perfectly happy starting a level with no checkpoint -- that is
 * what a new game does -- so we tell it there isn't one. A checkpoint stores its
 * zone count 45 bytes in (4 byte header, then the fixed-size fields CLevel::Load
 * reads before it), which is enough to spot a stale file.
 * ------------------------------------------------------------------------- */
static void *zones_manager = NULL;
static so_hook zm_ctor_hook, exists_hook, save_cp_hook;

extern const char *io_current_level(void);   // reimpl/io.c

// Written next to the save, naming the level its checkpoint came from.
#define CP_TAG_FILE DATA_PATH "checkpoint.level"

static void *zones_manager_ctor(void *self) {
	zones_manager = self;
	return (void *)SO_CONTINUE(uintptr_t, zm_ctor_hook, self);
}

static int loaded_zone_count(void) {
	if (!zones_manager) return -1;
	char *m = (char *)zones_manager;
	int begin = *(int *)(m + 0x24), end = *(int *)(m + 0x28);
	return begin ? (end - begin) / 4 : 0;
}

static int checkpoint_zone_count(void) {
	SceUID f = sceIoOpen(DATA_PATH "save.dat", SCE_O_RDONLY, 0777);
	if (f < 0) return -1;
	unsigned char head[64];
	int n = sceIoRead(f, head, sizeof(head));
	sceIoClose(f);
	if (n < 47) return -1;
	return (head[45] << 8) | head[46];
}

// Tag the checkpoint with the level it was taken in, since the save itself holds
// nothing that identifies one.
static void save_checkpoint(void *app) {
	SO_CONTINUE(int, save_cp_hook, app);
	const char *level = io_current_level();
	if (!level) return;
	SceUID f = sceIoOpen(CP_TAG_FILE, SCE_O_CREAT | SCE_O_WRONLY | SCE_O_TRUNC, 0777);
	if (f >= 0) {
		sceIoWrite(f, level, strlen(level));
		sceIoClose(f);
	}
}

static int checkpoint_is_for_this_level(void) {
	const char *level = io_current_level();
	if (!level) return 1;              // nothing to compare against, stay out of the way

	char tag[64];
	SceUID f = sceIoOpen(CP_TAG_FILE, SCE_O_RDONLY, 0777);
	if (f < 0) return 0;               // untagged save, so not one of ours
	int n = sceIoRead(f, tag, sizeof(tag) - 1);
	sceIoClose(f);
	if (n <= 0) return 0;
	tag[n] = 0;
	return strcmp(tag, level) == 0;
}

static int exists_checkpoint_save(void *app) {
	int found = SO_CONTINUE(int, exists_hook, app);
	if (!found) return 0;

	// A checkpoint from another level is worse than none at all: its zone and
	// object ids mean nothing here, the restore half-applies, and the player ends
	// up attached to no zone, falling through the world.
	if (!checkpoint_is_for_this_level()) {
		l_debug("ignoring checkpoint, it was taken in another level");
		return 0;
	}

	// Zone counts have to line up too. They don't while the level is still
	// streaming in, but the engine retries and by then they do.
	int here = loaded_zone_count(), saved = checkpoint_zone_count();
	if (saved >= 0 && here > 0 && saved != here) return 0;

	return found;
}

static void checkpoint_patches(void) {
	uintptr_t ctor = (uintptr_t)so_symbol(&so_mod, "_ZN13CZonesManagerC1Ev");
	if (ctor)
		zm_ctor_hook = hook_addr(ctor, (uintptr_t)&zones_manager_ctor);

	uintptr_t save_cp = (uintptr_t)so_symbol(&so_mod, "_ZN11Application14SaveCheckPointEv");
	if (save_cp)
		save_cp_hook = hook_addr(save_cp, (uintptr_t)&save_checkpoint);

	uintptr_t exists = (uintptr_t)so_symbol(&so_mod, "_ZN11Application20ExistsCheckPointSaveEv");
	if (exists)
		exists_hook = hook_addr(exists, (uintptr_t)&exists_checkpoint_save);
	else
		l_error("Application::ExistsCheckPointSave not found");
}

/* ------------------------------------------------------------------------- *
 * On-screen controls
 *
 * Movement, fire, slow-motion and sprint are on the sticks and buttons, so the
 * touch widgets are just clutter. Hiding them with SetVisible() would also kill
 * their hit test, and fire/slow-mo are still driven by faking a touch on them,
 * so we set alpha to 0 instead -- that leaves the touch area alive.
 *
 * The joystick and the slow-mo gauge redraw themselves every frame, so they get
 * flattened from their Update() rather than once at startup.
 * ------------------------------------------------------------------------- */
static so_hook set_button_visible_hook, slomo_update_hook, joystick_update_hook;
static void (*flash_set_alpha)(void *, const char *, float) = NULL;
static void (*char_set_alpha)(void *, float) = NULL;

static void set_button_visible(void *self, int id, int visible) {
	SO_CONTINUE(int, set_button_visible_hook, self, id, visible);
	// id 1 is the fire button; it goes up once when the HUD appears, which is a
	// safe moment to touch the movie (doing it mid-transition crashes).
	if (visible && flash_set_alpha && id == 1) {
		flash_set_alpha(self, "btn_primary", 0.0f);
		flash_set_alpha(self, "btn_sprint", 0.0f);
	}
}

static void hide_flash_parts(void *obj, const int *offsets, int count) {
	if (!char_set_alpha) return;
	for (int i = 0; i < count; i++)
		char_set_alpha((char *)obj + offsets[i], 0.0f);
}

static void slomo_update(void *self) {
	SO_CONTINUE(int, slomo_update_hook, self);
	static const int parts[] = { 0x28, 0x50, 0x78, 0xf0 };
	hide_flash_parts(self, parts, 4);
}

static void joystick_update(void *self) {
	SO_CONTINUE(int, joystick_update_hook, self);
	static const int parts[] = { 0x18, 0x40, 0x6c, 0x94 };
	hide_flash_parts(self, parts, 4);
}

static void hud_patches(void) {
	flash_set_alpha = (void (*)(void *, const char *, float))so_symbol(&so_mod,
		"_ZN12FlashManager8SetAlphaEPKcf");
	char_set_alpha = (void (*)(void *, float))so_symbol(&so_mod,
		"_ZN14FlashCharacter8SetAlphaEf");

	uintptr_t sbv = (uintptr_t)so_symbol(&so_mod, "_ZN12FlashManager16SetButtonVisibleEib");
	if (sbv)
		set_button_visible_hook = hook_addr(sbv, (uintptr_t)&set_button_visible);

	uintptr_t slomo = (uintptr_t)so_symbol(&so_mod, "_ZN11SlomoButton6UpdateEv");
	if (slomo && char_set_alpha)
		slomo_update_hook = hook_addr(slomo, (uintptr_t)&slomo_update);

	uintptr_t joy = (uintptr_t)so_symbol(&so_mod, "_ZN13FlashJoystick6UpdateEv");
	if (joy && char_set_alpha)
		joystick_update_hook = hook_addr(joy, (uintptr_t)&joystick_update);
}
