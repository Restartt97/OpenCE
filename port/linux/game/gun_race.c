/*
GUN_RACE.C

Slayer Pro gun race. Each player is locked to one weapon. Five kills with
it advance to the next weapon in a fixed order. Weapons the map cannot
fire are skipped. The last stage is unarmed: ten melee kills win. Grenades
are refused. The host decides; nothing here changes a network packet.

Set OPENCE_GUN_RACE=1 on the host and start Slayer Pro.
*/

#include "gun_race.h"

#ifndef GUN_RACE_RULES_ONLY

#include <stdlib.h>

#include "cseries/cseries.h"
#include "cseries/errors.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "effects/effect_definitions.h"
#include "items/projectile_definitions.h"
#include "items/weapon_definitions.h"
#include "items/weapons.h"
#include "memory/data.h"
#include "objects/objects.h"
#include "tag_files/tag_files.h"
#include "tag_files/tag_groups.h"
#include "interface/hud_messaging.h"
#include "units/units.h"
#include "halo_port_capacity.h"

/* port/linux/game/network_distributed.c */
boolean network_game_distributed_client(void);
/* cache_files.c */
boolean tag_index_is_group(long tag_index, long group_tag);

/* game_engine.c's weapon list, in that order. Fuel rod is not on it. */
enum
{
	GR_LIST_ASSAULT_RIFLE = 0,
	GR_LIST_FLAMETHROWER = 1,
	GR_LIST_NEEDLER = 3,
	GR_LIST_PISTOL = 4,
	GR_LIST_PLASMA_PISTOL = 5,
	GR_LIST_PLASMA_RIFLE = 6,
	GR_LIST_ROCKET_LAUNCHER = 7,
	GR_LIST_SHOTGUN = 8,
	GR_LIST_SNIPER_RIFLE = 9
};

#define GR_SOURCE_MAX 8
#define GR_PENDING_MAX 8

static struct gr_match gr_state;
static long gr_stage_definition[GR_MAX_STAGES];
static long gr_projectile_definitions[32];
static int gr_projectile_weapons[32];
static int gr_projectile_count;
static int gr_source_depth;
static int gr_source_melee[GR_SOURCE_MAX];
static int gr_source_weapon[GR_SOURCE_MAX];
static int gr_pending_depth;
static long gr_pending_stack[GR_PENDING_MAX];
static long gr_effect_ids[HALO_PORT_MAXIMUM_EFFECTS];
static long gr_effect_projectiles[HALO_PORT_MAXIMUM_EFFECTS];
static long gr_damage_definitions[160];
static int gr_damage_weapons[160];
static int gr_damage_count;
/* A client draws the scoreboard from the host's kill total. It does not
   run the rules. */
static int gr_scoreboard;
static int gr_radar_final[GR_MAX_PLAYERS];
static int gr_final_announced[GR_MAX_PLAYERS];
static int gr_intro_shown[GR_MAX_PLAYERS];
static int gr_score_seen[GR_MAX_PLAYERS];

#endif

static const char *const gr_names[GR_WEAPON_COUNT] =
{
	"Flamethrower",
	"Fuel Rod",
	"Rocket",
	"Sniper",
	"Shotgun",
	"Needler",
	"Assault Rifle",
	"Plasma Rifle",
	"Pistol",
	"Plasma Pistol",
	"Skull"
};

static int gr_slot_ok(int slot)
{
	return slot >= 0 && slot < GR_MAX_PLAYERS;
}

static void gr_zero(void *memory, unsigned long size)
{
	unsigned char *bytes;
	unsigned long index;

	bytes = memory;
	for (index = 0; index < size; index++)
		bytes[index] = 0;
}

void gr_match_init(struct gr_match *match)
{
	if (!match)
		return;
	gr_zero(match, sizeof(*match));
	match->winner_slot = -1;
}

int gr_match_add_stage(struct gr_match *match, int weapon)
{
	if (!match || weapon < 0 || weapon >= GR_WEAPON_COUNT)
		return 0;
	if (match->stage_count >= GR_MAX_STAGES)
		return 0;
	match->stages[match->stage_count] = weapon;
	match->stage_count++;
	return 1;
}

int gr_player_join(struct gr_match *match, int slot)
{
	if (!match || !match->enabled || !gr_slot_ok(slot))
		return 0;
	gr_zero(&match->players[slot], sizeof(match->players[slot]));
	match->players[slot].connected = 1;
	return 1;
}

void gr_player_leave(struct gr_match *match, int slot)
{
	if (!match || !gr_slot_ok(slot))
		return;
	gr_zero(&match->players[slot], sizeof(match->players[slot]));
}

int gr_current_weapon(const struct gr_match *match, int slot)
{
	int stage;

	if (!match || !gr_slot_ok(slot) || !match->players[slot].connected)
		return GR_WEAPON_INVALID;
	stage = match->players[slot].stage;
	if (stage < 0 || stage >= match->stage_count)
		return GR_WEAPON_INVALID;
	return match->stages[stage];
}

int gr_kills_needed(const struct gr_match *match, int slot)
{
	if (gr_current_weapon(match, slot) == GR_UNARMED)
		return GR_FINAL_MELEE_KILLS;
	if (gr_current_weapon(match, slot) == GR_WEAPON_INVALID)
		return 0;
	return GR_KILLS_PER_GUN;
}

int gr_kills_to_win(const struct gr_match *match)
{
	int index;
	int guns;

	if (!match)
		return 0;
	guns = 0;
	for (index = 0; index < match->stage_count; index++)
	{
		if (match->stages[index] != GR_UNARMED)
			guns++;
	}
	return guns * GR_KILLS_PER_GUN + GR_FINAL_MELEE_KILLS;
}

int gr_host_kill(struct gr_match *match, int attacker_slot, int victim_slot,
	int event_weapon, int is_melee)
{
	struct gr_player *attacker;
	int expected;
	int needed;

	if (!match || !match->enabled || match->match_finished ||
		!gr_slot_ok(attacker_slot) || !gr_slot_ok(victim_slot) ||
		attacker_slot == victim_slot)
	{
		return GR_KILL_IGNORED;
	}
	attacker = &match->players[attacker_slot];
	if (!attacker->connected || !match->players[victim_slot].connected ||
		attacker->finished)
	{
		return GR_KILL_IGNORED;
	}

	expected = gr_current_weapon(match, attacker_slot);
	if (expected == GR_UNARMED)
	{
		if (!is_melee || event_weapon != GR_UNARMED)
			return GR_KILL_IGNORED;
	}
	else if (is_melee || event_weapon != expected)
	{
		return GR_KILL_IGNORED;
	}

	attacker->valid_kills++;
	attacker->stage_kills++;
	needed = expected == GR_UNARMED ? GR_FINAL_MELEE_KILLS : GR_KILLS_PER_GUN;
	if (attacker->stage_kills < needed)
		return GR_KILL_COUNTED;

	if (expected == GR_UNARMED)
	{
		attacker->finished = 1;
		match->match_finished = 1;
		match->winner_slot = attacker_slot;
		return GR_MATCH_WON;
	}

	attacker->stage++;
	attacker->stage_kills = 0;
	return GR_STAGE_ADVANCED;
}

int gr_allow_world_weapon_pickup(const struct gr_match *match)
{
	return !match || !match->enabled;
}

int gr_allow_grenade_use(const struct gr_match *match)
{
	return !match || !match->enabled;
}

int gr_unarmed_melee_should_be_lethal(const struct gr_match *match,
	int attacker_slot, int is_melee)
{
	if (!match || !match->enabled || match->match_finished || !is_melee)
		return 0;
	if (!gr_slot_ok(attacker_slot) || !match->players[attacker_slot].connected)
		return 0;
	if (match->players[attacker_slot].finished)
		return 0;
	return gr_current_weapon(match, attacker_slot) == GR_UNARMED;
}

const char *gr_weapon_name(int weapon)
{
	if (weapon < 0 || weapon >= GR_WEAPON_COUNT)
		return "Invalid";
	return gr_names[weapon];
}

#ifndef GUN_RACE_RULES_ONLY

static int gr_player_slot(long player_index)
{
	long slot;

	if (player_index == NONE)
		return -1;
	slot = DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
	if (slot < 0 || slot >= GR_MAX_PLAYERS)
		return -1;
	return (int)slot;
}

static int gr_env_on(void)
{
	const char *setting;

	setting = getenv("OPENCE_GUN_RACE");
	return setting != NULL && setting[0] == '1' && setting[1] == '\0';
}

static int gr_variant_named_gun_race(void)
{
	static wchar_t const expected[] = { 'G', 'u', 'n', ' ', 'R', 'a', 'c', 'e', 0 };
	struct game_variant *variant;
	int index;

	variant = game_engine_get_variant();
	if (!variant)
		return 0;
	for (index = 0; expected[index] != 0; index++)
	{
		if (variant->human_readable_game_description[index] != expected[index])
			return 0;
	}
	return variant->human_readable_game_description[index] == 0;
}

static int gr_variant_is_slayer_pro(void)
{
	struct game_variant *variant;

	variant = game_engine_get_variant();
	if (!variant || variant->game_engine_index != game_engine_slayer)
		return 0;
	if (variant->universal_variant.teams)
		return 0;
	if (!variant->game_engine_variant.slayer.no_death_bonus)
		return 0;
	if (!variant->game_engine_variant.slayer.no_kill_penalty)
		return 0;
	if (variant->game_engine_variant.slayer.kill_in_order)
		return 0;
	return 1;
}

static long gr_pending_projectile(void)
{
	if (gr_pending_depth <= 0)
		return NONE;
	return gr_pending_stack[gr_pending_depth - 1];
}

static int gr_weapon_for_projectile(long projectile_definition_index)
{
	int index;

	if (projectile_definition_index == NONE)
		return GR_WEAPON_INVALID;
	for (index = 0; index < gr_projectile_count; index++)
	{
		if (gr_projectile_definitions[index] == projectile_definition_index)
			return gr_projectile_weapons[index];
	}
	return GR_WEAPON_INVALID;
}

static void gr_remember_projectile(long projectile_definition_index, int weapon)
{
	if (projectile_definition_index == NONE)
		return;
	if (gr_weapon_for_projectile(projectile_definition_index) != GR_WEAPON_INVALID)
		return;
	if (gr_projectile_count >= (int)(sizeof(gr_projectile_definitions) / sizeof(gr_projectile_definitions[0])))
		return;
	gr_projectile_definitions[gr_projectile_count] = projectile_definition_index;
	gr_projectile_weapons[gr_projectile_count] = weapon;
	gr_projectile_count++;
}

static void gr_remember_damage(long damage_index, int weapon)
{
	int index;

	if (damage_index == NONE || weapon < 0 || weapon >= GR_WEAPON_COUNT)
		return;
	for (index = 0; index < gr_damage_count; index++)
	{
		if (gr_damage_definitions[index] == damage_index &&
			gr_damage_weapons[index] == weapon)
			return;
	}
	if (gr_damage_count >= (int)(sizeof(gr_damage_definitions) / sizeof(gr_damage_definitions[0])))
		return;
	gr_damage_definitions[gr_damage_count] = damage_index;
	gr_damage_weapons[gr_damage_count] = weapon;
	gr_damage_count++;
}

static void gr_remember_projectile_damage(long projectile_index, int weapon, int depth);

static void gr_remember_effect_damage(long effect_index, int weapon, int depth)
{
	struct effect_definition *definition;
	int event_index;
	int event_count;

	if (effect_index == NONE || depth > 6)
		return;
	if (!tag_index_is_group(effect_index, EFFECT_DEFINITION_TAG))
		return;
	definition = (struct effect_definition *)tag_get(EFFECT_DEFINITION_TAG, effect_index);
	event_count = definition->events.count;
	if (event_count > MAXIMUM_EFFECT_EVENTS)
		event_count = MAXIMUM_EFFECT_EVENTS;
	for (event_index = 0; event_index < event_count; event_index++)
	{
		struct effect_event_definition *event;
		int part_index;
		int part_count;

		event = TAG_BLOCK_GET_ELEMENT(
			&definition->events,
			event_index,
			struct effect_event_definition);
		part_count = event->parts.count;
		if (part_count > MAXIMUM_EFFECT_PARTS_PER_EVENT)
			part_count = MAXIMUM_EFFECT_PARTS_PER_EVENT;
		for (part_index = 0; part_index < part_count; part_index++)
		{
			struct effect_part_definition *part;
			long reference_index;

			part = TAG_BLOCK_GET_ELEMENT(
				&event->parts,
				part_index,
				struct effect_part_definition);
			reference_index = part->reference.index;
			if (reference_index == NONE)
				continue;
			if (tag_index_is_group(reference_index, 'jpt!'))
				gr_remember_damage(reference_index, weapon);
			else if (tag_index_is_group(reference_index, EFFECT_DEFINITION_TAG))
				gr_remember_effect_damage(reference_index, weapon, depth + 1);
			else if (tag_index_is_group(reference_index, PROJECTILE_DEFINITION_TAG))
				gr_remember_projectile_damage(reference_index, weapon, depth + 1);
		}
	}
}

static void gr_remember_projectile_damage(long projectile_index, int weapon, int depth)
{
	struct projectile_definition *projectile;
	int response_index;
	int response_count;

	if (projectile_index == NONE || depth > 6)
		return;
	if (!tag_index_is_group(projectile_index, PROJECTILE_DEFINITION_TAG))
		return;
	projectile = projectile_definition_get(projectile_index);
	gr_remember_damage(projectile->projectile.impact_damage.index, weapon);
	gr_remember_damage(projectile->projectile.attached_detonation_damage.index, weapon);
	gr_remember_effect_damage(projectile->projectile.effect.index, weapon, depth + 1);
	gr_remember_effect_damage(projectile->projectile.super_detonation.index, weapon, depth + 1);
	gr_remember_effect_damage(projectile->projectile.detonation_started.index, weapon, depth + 1);
	response_count = projectile->projectile.material_responses.count;
	if (response_count > 16)
		response_count = 16;
	for (response_index = 0; response_index < response_count; response_index++)
	{
		struct projectile_material_response_definition *response;

		response = TAG_BLOCK_GET_ELEMENT(
			&projectile->projectile.material_responses,
			response_index,
			struct projectile_material_response_definition);
		gr_remember_effect_damage(response->default_effect.index, weapon, depth + 1);
		gr_remember_effect_damage(response->potential_effect.index, weapon, depth + 1);
		gr_remember_effect_damage(response->detonation_effect.index, weapon, depth + 1);
	}
}

static int gr_weapon_for_damage(long damage_index, int preferred)
{
	int index;
	int found;

	if (damage_index == NONE)
		return GR_WEAPON_INVALID;
	found = GR_WEAPON_INVALID;
	for (index = 0; index < gr_damage_count; index++)
	{
		if (gr_damage_definitions[index] != damage_index)
			continue;
		if (gr_damage_weapons[index] == preferred)
			return preferred;
		if (found == GR_WEAPON_INVALID)
			found = gr_damage_weapons[index];
	}
	return found;
}

static int gr_weapon_can_fire(long definition_index)
{
	struct weapon_definition *definition;
	int trigger_index;
	int trigger_count;

	if (definition_index == NONE)
		return 0;
	definition = weapon_definition_get(definition_index);
	trigger_count = definition->weapon.triggers.count;
	if (trigger_count > 4)
		trigger_count = 4;
	for (trigger_index = 0; trigger_index < trigger_count; trigger_index++)
	{
		struct weapon_trigger_definition *trigger;

		trigger = TAG_BLOCK_GET_ELEMENT(
			&definition->weapon.triggers,
			trigger_index,
			struct weapon_trigger_definition);
		if (trigger->projectile.index != NONE)
			return 1;
		if (trigger->firing_effects.count > 0)
			return 1;
	}
	return 0;
}

static void gr_remember_weapon_projectiles(long definition_index, int weapon)
{
	struct weapon_definition *definition;
	int trigger_index;
	int trigger_count;

	if (definition_index == NONE)
		return;
	definition = weapon_definition_get(definition_index);
	trigger_count = definition->weapon.triggers.count;
	if (trigger_count > 4)
		trigger_count = 4;
	for (trigger_index = 0; trigger_index < trigger_count; trigger_index++)
	{
		struct weapon_trigger_definition *trigger;

		trigger = TAG_BLOCK_GET_ELEMENT(
			&definition->weapon.triggers,
			trigger_index,
			struct weapon_trigger_definition);
		gr_remember_projectile(trigger->projectile.index, weapon);
		gr_remember_projectile_damage(trigger->projectile.index, weapon, 0);
	}
}

static long gr_fuel_rod_definition(void)
{
	static const char *const names[] =
	{
		"weapons\\plasma cannon\\plasma cannon",
		"weapons\\fuel rod gun\\fuel rod gun",
		"weapons\\plasma_cannon\\plasma_cannon"
	};
	int index;

	for (index = 0; index < (int)(sizeof(names) / sizeof(names[0])); index++)
	{
		long definition_index;

		definition_index = tag_loaded(WEAPON_DEFINITION_TAG, names[index]);
		if (definition_index != NONE)
			return definition_index;
	}
	return NONE;
}

static long gr_list_weapon(int list_index)
{
	if (list_index < 0)
		return NONE;
	return list_index_to_weapon_definition_index(list_index);
}

/* The engine replaces the flamethrower and the gravity rifle with a
   rocket. A stage that would be replaced is not a weapon you can hold. */
static int gr_weapon_is_held_as_itself(long definition_index)
{
	long remapped;

	if (!gr_weapon_can_fire(definition_index))
		return 0;
	remapped = game_engine_remap_weapon(definition_index);
	if (remapped == NONE || remapped != definition_index)
		return 0;
	return 1;
}

/* The oddball is a flag: pulling the trigger drops it, and deleting it
   halts the game. Clear those bits so it is just the skull model. */
static void gr_disarm_skull(long definition_index)
{
	struct weapon_definition *definition;

	if (definition_index == NONE)
		return;
	definition = weapon_definition_get(definition_index);
	definition->weapon.flags &= ~(
		FLAG(_weapon_must_be_readied_bit) |
		FLAG(_weapon_detonates_when_dropped_bit) |
		FLAG(_weapon_prevents_melee_attack_bit));
}

/* The oddball. Remap leaves it alone. Empty hands if the map has none. */
static long gr_skull_definition(void)
{
	long definition_index;
	long remapped;

	definition_index = get_ball_definition_index();
	if (definition_index == NONE)
		return NONE;
	remapped = game_engine_remap_weapon(definition_index);
	if (remapped == NONE)
		return NONE;
	gr_disarm_skull(remapped);
	return remapped;
}

/* A flag cannot be deleted while a game is running. */
static void gr_delete_object(long object_index)
{
	if (object_index == NONE)
		return;
	if (object_get(object_index)->object.type == _object_type_weapon &&
		weapon_is_flag(object_index))
		return;
	object_delete(object_index);
}

static void gr_accept_stage(int weapon, long definition_index)
{
	int stage;

	if (weapon == GR_UNARMED)
		definition_index = gr_skull_definition();
	else if (!gr_weapon_is_held_as_itself(definition_index))
	{
		error(_error_silent, "Gun Race Pro: skipped %s (not in this map)",
			gr_weapon_name(weapon));
		return;
	}
	if (!gr_match_add_stage(&gr_state, weapon))
		return;
	stage = gr_state.stage_count - 1;
	gr_stage_definition[stage] = definition_index;
	if (weapon != GR_UNARMED)
		gr_remember_weapon_projectiles(definition_index, weapon);
	error(_error_silent, "Gun Race Pro: stage %d %s",
		stage + 1, gr_weapon_name(weapon));
}

static void gr_build_ladder(void)
{
	/* Same order as the mode. List index -1 is resolved by name. */
	static const int weapons[] =
	{
		GR_FLAMETHROWER,
		GR_FUEL_ROD,
		GR_ROCKET_LAUNCHER,
		GR_SNIPER,
		GR_SHOTGUN,
		GR_NEEDLER,
		GR_ASSAULT_RIFLE,
		GR_PLASMA_RIFLE,
		GR_PISTOL,
		GR_PLASMA_PISTOL
	};
	static const int lists[] =
	{
		GR_LIST_FLAMETHROWER,
		-1,
		GR_LIST_ROCKET_LAUNCHER,
		GR_LIST_SNIPER_RIFLE,
		GR_LIST_SHOTGUN,
		GR_LIST_NEEDLER,
		GR_LIST_ASSAULT_RIFLE,
		GR_LIST_PLASMA_RIFLE,
		GR_LIST_PISTOL,
		GR_LIST_PLASMA_PISTOL
	};
	int index;

	for (index = 0; index < (int)(sizeof(weapons) / sizeof(weapons[0])); index++)
	{
		long definition_index;

		if (weapons[index] == GR_FUEL_ROD)
			definition_index = gr_fuel_rod_definition();
		else
			definition_index = gr_list_weapon(lists[index]);
		gr_accept_stage(weapons[index], definition_index);
	}
	gr_accept_stage(GR_UNARMED, NONE);
}

static void gr_zero_grenades(struct unit_datum *unit)
{
	int index;

	for (index = 0; index < NUMBER_OF_UNIT_GRENADE_TYPES; index++)
		unit->unit.grenade_counts[index] = 0;
}

static void gr_strip_weapons(long unit_index)
{
	struct unit_datum *unit;
	long weapons[MAXIMUM_WEAPONS_PER_UNIT];
	int index;

	unit = unit_get(unit_index);
	for (index = 0; index < MAXIMUM_WEAPONS_PER_UNIT; index++)
	{
		weapons[index] = unit->unit.weapon_object_indices[index];
		unit->unit.weapon_object_indices[index] = NONE;
	}
	unit->unit.current_weapon_index = NONE;
	unit->unit.desired_weapon_index = NONE;
	for (index = 0; index < MAXIMUM_WEAPONS_PER_UNIT; index++)
	{
		if (weapons[index] != NONE)
			gr_delete_object(weapons[index]);
	}
}

static int gr_holding_only(long unit_index, long definition_index)
{
	struct unit_datum *unit;
	int index;
	int found;

	unit = unit_get(unit_index);
	found = 0;
	for (index = 0; index < MAXIMUM_WEAPONS_PER_UNIT; index++)
	{
		long weapon_index;

		weapon_index = unit->unit.weapon_object_indices[index];
		if (weapon_index == NONE)
			continue;
		if (definition_index == NONE ||
			weapon_get(weapon_index)->definition_index != definition_index)
		{
			return 0;
		}
		found++;
	}
	if (definition_index == NONE)
		return found == 0;
	return found == 1;
}

static void gr_fill_magazines(long weapon_index, long definition_index)
{
	struct weapon_datum *weapon;
	struct weapon_definition *definition;
	int magazine_index;
	int magazine_count;

	weapon = weapon_get(weapon_index);
	definition = weapon_definition_get(definition_index);
	magazine_count = definition->weapon.magazines.count;
	if (magazine_count > 2)
		magazine_count = 2;
	for (magazine_index = 0; magazine_index < magazine_count; magazine_index++)
	{
		struct weapon_magazine_definition *magazine;
		short total;
		short loaded;

		magazine = TAG_BLOCK_GET_ELEMENT(
			&definition->weapon.magazines,
			magazine_index,
			struct weapon_magazine_definition);
		total = magazine->rounds_total_maximum;
		if (total <= 0)
			total = magazine->rounds_total_initial;
		loaded = magazine->rounds_loaded_maximum;
		if (loaded <= 0)
			loaded = total;
		if (loaded < 0)
			loaded = 0;
		if (total < 0)
			total = 0;
		weapon->weapon.magazines[magazine_index].rounds_total = total;
		weapon->weapon.magazines[magazine_index].rounds_loaded = loaded;
	}
}

static void gr_give_weapon(long unit_index, long definition_index, int stage)
{
	struct object_placement_data placement;
	struct unit_datum *unit;
	long weapon_index;

	if (definition_index == NONE)
		return;
	object_placement_data_new(&placement, definition_index, unit_index);
	weapon_index = object_new(&placement);
	if (weapon_index == NONE)
	{
		error(_error_silent, "Gun Race Pro: could not create %s",
			tag_get_name(definition_index));
		return;
	}
	if (object_get(weapon_index)->object.type != _object_type_weapon)
	{
		gr_delete_object(weapon_index);
		return;
	}
	gr_fill_magazines(weapon_index, definition_index);
	/* The normal pickup path puts it in the hands and builds the view model. */
	if (!unit_add_weapon_to_inventory(unit_index, weapon_index, _unit_add_weapon_replace))
	{
		gr_delete_object(weapon_index);
		error(_error_silent, "Gun Race Pro: could not equip %s",
			tag_get_name(definition_index));
		return;
	}
	unit = unit_get(unit_index);
	player_control_set_desired_weapon(unit_index, unit->unit.desired_weapon_index);
	if (stage >= 0 && stage < GR_MAX_STAGES)
	{
		long carried;

		carried = unit->unit.weapon_object_indices[0];
		if (unit->unit.desired_weapon_index >= 0 &&
			unit->unit.desired_weapon_index < MAXIMUM_WEAPONS_PER_UNIT)
			carried = unit->unit.weapon_object_indices[unit->unit.desired_weapon_index];
		if (carried != NONE)
			gr_stage_definition[stage] = weapon_get(carried)->definition_index;
	}
}

/* A carried weapon has no parent until the unit puts it in its hands.
   Deleting those was recreating the rocket every tick (the ready sound). */
static int gr_weapon_is_carried(long weapon_index)
{
	struct object_iterator iterator;
	struct object_datum *object;

	object_iterator_new(&iterator, _object_mask_unit, 0);
	object = (struct object_datum *)object_iterator_next(&iterator);
	while (object)
	{
		struct unit_datum *unit;
		int slot;

		unit = unit_get(iterator.index);
		for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
		{
			if (unit->unit.weapon_object_indices[slot] == weapon_index)
				return 1;
		}
		object = (struct object_datum *)object_iterator_next(&iterator);
	}
	return 0;
}

/* Weapons on the ground, including one a player just dropped. */
static void gr_delete_loose_weapons(void)
{
	struct object_iterator iterator;
	struct object_datum *object;
	long pending[256];
	int count;
	int index;

	count = 0;
	object_iterator_new(&iterator, _object_mask_weapon, 0);
	object = (struct object_datum *)object_iterator_next(&iterator);
	while (object)
	{
		if (object->object.parent_object_index == NONE &&
			!gr_weapon_is_carried(iterator.index) &&
			count < 256)
			pending[count++] = iterator.index;
		object = (struct object_datum *)object_iterator_next(&iterator);
	}
	for (index = 0; index < count; index++)
		gr_delete_object(pending[index]);
}

/* Leave the stage weapon in hand. Remember the tag the game actually
   created, which can differ from the one requested. */
static int gr_keep_stage_weapon(long unit_index, int stage, long definition_index)
{
	struct unit_datum *unit;
	long keep_index;
	int keep_slot;
	int slot;

	if (definition_index == NONE)
		return 0;
	unit = unit_get(unit_index);
	keep_index = NONE;
	keep_slot = NONE;
	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		long weapon_index;

		weapon_index = unit->unit.weapon_object_indices[slot];
		if (weapon_index == NONE)
			continue;
		if (weapon_get(weapon_index)->definition_index != definition_index)
			continue;
		keep_index = weapon_index;
		keep_slot = slot;
		break;
	}
	if (keep_index == NONE)
		return 0;
	gr_stage_definition[stage] = definition_index;
	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		long weapon_index;

		if (slot == keep_slot)
			continue;
		weapon_index = unit->unit.weapon_object_indices[slot];
		unit->unit.weapon_object_indices[slot] = NONE;
		if (weapon_index != NONE)
			gr_delete_object(weapon_index);
	}
	if (unit->unit.current_weapon_index != (short)keep_slot)
	{
		unit->unit.desired_weapon_index = (short)keep_slot;
		player_control_set_desired_weapon(unit_index, (short)keep_slot);
	}
	return 1;
}

static void gr_append_wchar(wchar_t *buffer, long buffer_count, long *used, wchar_t character)
{
	if (*used + 1 >= buffer_count)
		return;
	buffer[*used] = character;
	*used = *used + 1;
	buffer[*used] = 0;
}

static void gr_append_ascii(wchar_t *buffer, long buffer_count, long *used, const char *text)
{
	int index;

	if (!text)
		return;
	for (index = 0; text[index] != '\0'; index++)
		gr_append_wchar(buffer, buffer_count, used, (wchar_t)text[index]);
}

static void gr_append_int(wchar_t *buffer, long buffer_count, long *used, int value)
{
	char digits[16];
	int count;
	int index;

	if (value < 0)
		value = 0;
	count = 0;
	do
	{
		digits[count] = (char)('0' + (value % 10));
		value /= 10;
		count++;
	} while (value > 0 && count < (int)sizeof(digits));
	for (index = count - 1; index >= 0; index--)
		gr_append_wchar(buffer, buffer_count, used, (wchar_t)digits[index]);
}

long gun_race_active(void)
{
	return gr_state.enabled;
}

long gun_race_show_scoreboard(void)
{
	return gr_state.enabled || gr_scoreboard;
}

void gun_race_on_new_map(void)
{
	struct game_variant *variant;
	int kills_to_win;

	gr_match_init(&gr_state);
	gr_projectile_count = 0;
	gr_damage_count = 0;
	gr_scoreboard = 0;
	csmemset(gr_radar_final, 0, sizeof(gr_radar_final));
	csmemset(gr_final_announced, 0, sizeof(gr_final_announced));
	csmemset(gr_intro_shown, 0, sizeof(gr_intro_shown));
	csmemset(gr_score_seen, 0, sizeof(gr_score_seen));
	gr_source_depth = 0;
	gr_pending_depth = 0;
	csmemset(gr_stage_definition, 0xFF, sizeof(gr_stage_definition));
	csmemset(gr_effect_ids, 0xFF, sizeof(gr_effect_ids));
	csmemset(gr_effect_projectiles, 0xFF, sizeof(gr_effect_projectiles));

	if (network_game_distributed_client())
	{
		/* Same ladder, so this machine can print the host's score. */
		if (!gr_variant_named_gun_race())
		{
			if (!gr_env_on() || !gr_variant_is_slayer_pro())
				return;
		}
		gr_build_ladder();
		gr_scoreboard = 1;
		return;
	}
	/* The saved gametype named Gun Race, or Slayer Pro with the switch. */
	if (!gr_variant_named_gun_race())
	{
		if (!gr_env_on())
			return;
		if (!gr_variant_is_slayer_pro())
		{
			error(_error_silent,
				"Gun Race Pro: OPENCE_GUN_RACE=1 ignored; host Slayer Pro");
			return;
		}
	}

	gr_build_ladder();
	gr_state.enabled = 1;
	kills_to_win = gr_kills_to_win(&gr_state);
	variant = game_engine_get_variant();
	if (variant)
		variant->universal_variant.score_to_win = kills_to_win;
	/* Map grenades are remapped away. Counts are also cleared every tick.
	   Map weapons are not spawned, and any already placed are deleted. */
	game_engine_suppress_grenade_set();
	{
		struct game_variant_options options;

		options = *game_variant_options_get();
		options.no_map_weapons = TRUE;
		game_set_game_variant_options(&options);
	}
	error(_error_silent, "Gun Race Pro: %d kills to win", kills_to_win);
}

void gun_race_on_player_added(long player_index)
{
	int slot;

	if (!gr_state.enabled)
		return;
	slot = gr_player_slot(player_index);
	if (slot < 0)
		return;
	gr_player_join(&gr_state, slot);
}

static void gr_append_name(wchar_t *buffer, long buffer_count, long *used, const wchar_t *name)
{
	int index;

	if (!name)
		return;
	for (index = 0; name[index] != 0 && index < 11; index++)
		gr_append_wchar(buffer, buffer_count, used, name[index]);
}

static void gr_print_locals(const wchar_t *text)
{
	struct data_iterator iterator;
	struct player_datum *player;

	data_iterator_new(&iterator, player_data);
	player = (struct player_datum *)data_iterator_next(&iterator);
	while (player)
	{
		if (player->local_player_index != NONE)
			hud_print_message(player->local_player_index, text);
		player = (struct player_datum *)data_iterator_next(&iterator);
	}
}

static void gr_show_intro(long player_index)
{
	struct player_datum *player;
	int slot;

	if (!gun_race_show_scoreboard())
		return;
	slot = gr_player_slot(player_index);
	if (slot < 0 || gr_intro_shown[slot])
		return;
	player = player_try_and_get(player_index);
	if (!player || player->local_player_index == NONE)
		return;
	gr_intro_shown[slot] = 1;
	hud_print_message(
		player->local_player_index,
		L"Meet the score to advance. Melee doesn't count.");
}

static void gr_announce_final(long player_index)
{
	struct player_datum *player;
	wchar_t text[63];
	long used;

	player = player_try_and_get(player_index);
	if (!player)
		return;
	used = 0;
	text[0] = 0;
	gr_append_name(text, 63, &used, player->name);
	gr_append_ascii(text, 63, &used, " reached the last weapon. Don't let them win.");
	gr_print_locals(text);
}

void gun_race_on_player_spawn(long player_index)
{
	gr_show_intro(player_index);
	gun_race_enforce_player(player_index);
}

void gun_race_enforce_player(long player_index)
{
	struct player_datum *player;
	struct unit_datum *unit;
	int slot;
	int stage;
	long definition_index;

	if (!gr_state.enabled || network_game_distributed_client())
		return;
	slot = gr_player_slot(player_index);
	if (slot < 0)
		return;
	if (!gr_state.players[slot].connected)
		gr_player_join(&gr_state, slot);
	if (!gr_state.players[slot].connected)
		return;
	player = player_try_and_get(player_index);
	if (!player || player->unit_index == NONE)
		return;
	unit = unit_try_and_get(player->unit_index);
	if (!unit)
		return;

	gr_zero_grenades(unit);
	{
		static long swept_at = -1;
		long now;

		now = game_time_get();
		if (now != swept_at)
		{
			swept_at = now;
			gr_delete_loose_weapons();
		}
	}
	stage = gr_state.players[slot].stage;
	if (stage < 0 || stage >= gr_state.stage_count)
		return;
	definition_index = gr_stage_definition[stage];
	if (gr_holding_only(player->unit_index, definition_index))
		return;
	if (gr_keep_stage_weapon(player->unit_index, stage, definition_index))
		return;
	gr_strip_weapons(player->unit_index);
	if (definition_index != NONE)
	{
		gr_give_weapon(player->unit_index, definition_index, stage);
		/* The skull's tag can refuse to go in the hands. Fists still
		   kill in one hit, and we do not recreate it every tick. */
		if (gr_current_weapon(&gr_state, slot) == GR_UNARMED &&
			!gr_keep_stage_weapon(player->unit_index, stage, gr_stage_definition[stage]))
		{
			gr_stage_definition[stage] = NONE;
			player_control_set_desired_weapon(player->unit_index, NONE);
		}
	}
	else
		player_control_set_desired_weapon(player->unit_index, NONE);
}

long gun_race_forbids_weapon_pickup(void)
{
	return gr_state.enabled && !gr_allow_world_weapon_pickup(&gr_state);
}

long gun_race_forbids_grenades(void)
{
	return gr_state.enabled && !gr_allow_grenade_use(&gr_state);
}

int gun_race_handle_kill(long killing_player_index, long dead_player_index)
{
	int attacker_slot;
	int victim_slot;
	int weapon;
	int melee;
	int result;

	if (!gr_state.enabled)
		return GR_KILL_IGNORED;
	attacker_slot = gr_player_slot(killing_player_index);
	victim_slot = gr_player_slot(dead_player_index);
	melee = 0;
	weapon = GR_WEAPON_INVALID;
	if (gr_source_depth > 0)
	{
		melee = gr_source_melee[gr_source_depth - 1];
		weapon = gr_source_weapon[gr_source_depth - 1];
	}
	if (melee)
		weapon = GR_UNARMED;
	result = gr_host_kill(&gr_state, attacker_slot, victim_slot, weapon, melee);
	if (result == GR_STAGE_ADVANCED || result == GR_MATCH_WON)
	{
		error(_error_silent, "Gun Race Pro: player %d %s %s",
			attacker_slot,
			result == GR_MATCH_WON ? "won with" : "advanced to",
			gr_weapon_name(gr_current_weapon(&gr_state, attacker_slot)));
	}
	if (result == GR_STAGE_ADVANCED &&
		gr_current_weapon(&gr_state, attacker_slot) == GR_UNARMED &&
		!gr_final_announced[attacker_slot])
	{
		gr_final_announced[attacker_slot] = 1;
		gr_radar_final[attacker_slot] = 1;
		gr_announce_final(killing_player_index);
	}
	return result;
}

static void gr_write_progress(wchar_t *buffer, long buffer_count, int weapon, int stage_kills, int needed)
{
	long used;

	used = 0;
	gr_append_ascii(buffer, buffer_count, &used, gr_weapon_name(weapon));
	gr_append_wchar(buffer, buffer_count, &used, L' ');
	gr_append_int(buffer, buffer_count, &used, stage_kills);
	gr_append_wchar(buffer, buffer_count, &used, L'/');
	gr_append_int(buffer, buffer_count, &used, needed);
}

/* The host's kill total, as this machine was told it. */
static void gr_progress_from_total(int total, int *weapon, int *stage_kills, int *needed)
{
	int index;
	int left;

	*weapon = GR_WEAPON_INVALID;
	*stage_kills = 0;
	*needed = 0;
	if (total < 0)
		total = 0;
	left = total;
	for (index = 0; index < gr_state.stage_count; index++)
	{
		int stage_weapon;
		int stage_needed;

		stage_weapon = gr_state.stages[index];
		stage_needed = stage_weapon == GR_UNARMED ? GR_FINAL_MELEE_KILLS : GR_KILLS_PER_GUN;
		if (left < stage_needed || index + 1 == gr_state.stage_count)
		{
			*weapon = stage_weapon;
			*stage_kills = left;
			if (*stage_kills > stage_needed)
				*stage_kills = stage_needed;
			*needed = stage_needed;
			return;
		}
		left -= stage_needed;
	}
}

void gun_race_format_progress(long player_index, long replicated_kills,
	wchar_t *buffer, long buffer_count)
{
	int slot;
	int weapon;
	int stage_kills;
	int needed;

	if (!buffer || buffer_count <= 0)
		return;
	buffer[0] = 0;
	if (!gr_state.enabled)
	{
		if (!gr_scoreboard)
			return;
		gr_progress_from_total((int)replicated_kills, &weapon, &stage_kills, &needed);
		if (weapon == GR_WEAPON_INVALID)
			return;
		gr_write_progress(buffer, buffer_count, weapon, stage_kills, needed);
		return;
	}
	slot = gr_player_slot(player_index);
	if (slot < 0 || !gr_state.players[slot].connected)
		return;
	gr_write_progress(
		buffer,
		buffer_count,
		gr_current_weapon(&gr_state, slot),
		gr_state.players[slot].stage_kills,
		gr_kills_needed(&gr_state, slot));
}

int gun_race_begin_reported_hit(long player_index, long damage_definition_index, int is_melee)
{
	int slot;
	int preferred;

	if (!gr_state.enabled || gr_source_depth >= GR_SOURCE_MAX)
		return 0;
	if (is_melee)
	{
		gun_race_begin_melee();
		return gr_source_depth > 0;
	}
	slot = gr_player_slot(player_index);
	preferred = gr_current_weapon(&gr_state, slot);
	gr_source_melee[gr_source_depth] = 0;
	gr_source_weapon[gr_source_depth] = gr_weapon_for_damage(damage_definition_index, preferred);
	gr_source_depth++;
	return 1;
}

long gun_race_reported_melee_should_kill(long player_index)
{
	int slot;

	slot = gr_player_slot(player_index);
	if (slot < 0)
		return 0;
	return gr_unarmed_melee_should_be_lethal(&gr_state, slot, 1);
}

void gun_race_push_pending_projectile(long projectile_definition_index)
{
	if (!gr_state.enabled)
		return;
	if (gr_pending_depth >= GR_PENDING_MAX)
		return;
	gr_pending_stack[gr_pending_depth] = projectile_definition_index;
	gr_pending_depth++;
}

void gun_race_pop_pending_projectile(void)
{
	if (!gr_state.enabled)
		return;
	if (gr_pending_depth > 0)
		gr_pending_depth--;
}

void gun_race_bind_effect(long effect_index)
{
	int slot;
	long projectile;

	if (!gr_state.enabled || effect_index == NONE)
		return;
	slot = (int)(effect_index & 0xFFFF);
	if (slot < 0 || slot >= HALO_PORT_MAXIMUM_EFFECTS)
		return;
	/* A reused effect slot must not keep the previous projectile. */
	gr_effect_ids[slot] = NONE;
	gr_effect_projectiles[slot] = NONE;
	projectile = gr_pending_projectile();
	if (projectile == NONE)
		return;
	gr_effect_ids[slot] = effect_index;
	gr_effect_projectiles[slot] = projectile;
}

long gun_race_effect_projectile(long effect_index)
{
	int slot;

	if (!gr_state.enabled || effect_index == NONE)
		return NONE;
	slot = (int)(effect_index & 0xFFFF);
	if (slot < 0 || slot >= HALO_PORT_MAXIMUM_EFFECTS)
		return NONE;
	if (gr_effect_ids[slot] != effect_index)
		return NONE;
	return gr_effect_projectiles[slot];
}

void gun_race_begin_projectile(long projectile_definition_index)
{
	if (!gr_state.enabled || gr_source_depth >= GR_SOURCE_MAX)
		return;
	gr_source_melee[gr_source_depth] = 0;
	gr_source_weapon[gr_source_depth] = gr_weapon_for_projectile(projectile_definition_index);
	gr_source_depth++;
}

void gun_race_begin_melee(void)
{
	if (!gr_state.enabled || gr_source_depth >= GR_SOURCE_MAX)
		return;
	gr_source_melee[gr_source_depth] = 1;
	gr_source_weapon[gr_source_depth] = GR_UNARMED;
	gr_source_depth++;
}

void gun_race_end_damage(void)
{
	if (!gr_state.enabled)
		return;
	if (gr_source_depth > 0)
		gr_source_depth--;
}

long gun_race_melee_should_kill(long attacker_unit_index)
{
	struct unit_datum *unit;
	int slot;

	if (!gr_state.enabled || attacker_unit_index == NONE)
		return 0;
	unit = unit_try_and_get(attacker_unit_index);
	if (!unit)
		return 0;
	slot = gr_player_slot(unit->unit.player_index);
	if (slot < 0)
		return 0;
	return gr_unarmed_melee_should_be_lethal(&gr_state, slot, 1);
}

void gun_race_observe_score(long player_index, long total_kills)
{
	int slot;
	int weapon;
	int stage_kills;
	int needed;
	int on_final;

	if (!gun_race_show_scoreboard())
		return;
	slot = gr_player_slot(player_index);
	if (slot < 0)
		return;
	if (gr_state.enabled)
	{
		if (gr_state.players[slot].connected)
			gr_radar_final[slot] = gr_current_weapon(&gr_state, slot) == GR_UNARMED;
		return;
	}
	gr_progress_from_total((int)total_kills, &weapon, &stage_kills, &needed);
	on_final = weapon == GR_UNARMED;
	if (!gr_score_seen[slot])
	{
		gr_score_seen[slot] = 1;
		gr_radar_final[slot] = on_final;
		if (on_final)
			gr_final_announced[slot] = 1;
		return;
	}
	if (on_final && !gr_final_announced[slot])
	{
		gr_final_announced[slot] = 1;
		gr_radar_final[slot] = 1;
		gr_announce_final(player_index);
		return;
	}
	gr_radar_final[slot] = on_final;
}

long gun_race_mark_on_radar(long unit_index)
{
	long player_index;
	int slot;

	if (!gun_race_show_scoreboard() || unit_index == NONE)
		return 0;
	player_index = player_index_from_unit_index(unit_index);
	slot = gr_player_slot(player_index);
	if (slot < 0)
		return 0;
	return gr_radar_final[slot];
}

#endif
