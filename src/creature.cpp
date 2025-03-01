#include "creature.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>

#include "anatomy.h"
#include "avatar.h"
#include "calendar.h"
#include "character.h"
#include "color.h"
#include "cursesdef.h"
#include "damage.h"
#include "debug.h"
#include "effect.h"
#include "enums.h"
#include "event.h"
#include "event_bus.h"
#include "field.h"
#include "game.h"
#include "game_constants.h"
#include "int_id.h"
#include "item.h"
#include "json.h"
#include "lightmap.h"
#include "line.h"
#include "map.h"
#include "map_iterator.h"
#include "mapdata.h"
#include "messages.h"
#include "monster.h"
#include "mtype.h"
#include "npc.h"
#include "output.h"
#include "player.h"
#include "point.h"
#include "projectile.h"
#include "ranged.h"
#include "rng.h"
#include "string_id.h"
#include "string_utils.h"
#include "translations.h"
#include "value_ptr.h"
#include "vehicle.h"
#include "vpart_position.h"

static const ammo_effect_str_id ammo_effect_APPLY_SAP( "APPLY_SAP" );
static const ammo_effect_str_id ammo_effect_BEANBAG( "BEANBAG" );
static const ammo_effect_str_id ammo_effect_BLINDS_EYES( "BLINDS_EYES" );
static const ammo_effect_str_id ammo_effect_BOUNCE( "BOUNCE" );
static const ammo_effect_str_id ammo_effect_IGNITE( "IGNITE" );
static const ammo_effect_str_id ammo_effect_INCENDIARY( "INCENDIARY" );
static const ammo_effect_str_id ammo_effect_LARGE_BEANBAG( "LARGE_BEANBAG" );
static const ammo_effect_str_id ammo_effect_magic( "magic" );
static const ammo_effect_str_id ammo_effect_NO_CRIT( "NO_CRIT" );
static const ammo_effect_str_id ammo_effect_NO_DAMAGE_SCALING( "NO_DAMAGE_SCALING" );
static const ammo_effect_str_id ammo_effect_NOGIB( "NOGIB" );
static const ammo_effect_str_id ammo_effect_PARALYZEPOISON( "PARALYZEPOISON" );
static const ammo_effect_str_id ammo_effect_TANGLE( "TANGLE" );


static const efftype_id effect_blind( "blind" );
static const efftype_id effect_bounced( "bounced" );
static const efftype_id effect_downed( "downed" );
static const efftype_id effect_lying_down( "lying_down" );
static const efftype_id effect_no_sight( "no_sight" );
static const efftype_id effect_npc_suspend( "npc_suspend" );
static const efftype_id effect_onfire( "onfire" );
static const efftype_id effect_paralyzepoison( "paralyzepoison" );
static const efftype_id effect_ridden( "ridden" );
static const efftype_id effect_riding( "riding" );
static const efftype_id effect_sap( "sap" );
static const efftype_id effect_sleep( "sleep" );
static const efftype_id effect_stunned( "stunned" );
static const efftype_id effect_tied( "tied" );
static const efftype_id effect_zapped( "zapped" );

const std::map<std::string, m_size> Creature::size_map = {
    {"TINY", MS_TINY}, {"SMALL", MS_SMALL}, {"MEDIUM", MS_MEDIUM},
    {"LARGE", MS_LARGE}, {"HUGE", MS_HUGE}
};

const std::set<material_id> Creature::cmat_flesh{
    material_id( "flesh" ), material_id( "iflesh" )
};
const std::set<material_id> Creature::cmat_fleshnveg{
    material_id( "flesh" ),  material_id( "iflesh" ), material_id( "veggy" )
};
const std::set<material_id> Creature::cmat_flammable{
    material_id( "paper" ), material_id( "powder" ), material_id( "wood" ),
    material_id( "cotton" ), material_id( "wool" )
};
const std::set<material_id> Creature::cmat_flameres{
    material_id( "stone" ), material_id( "kevlar" ), material_id( "steel" )
};

Creature::Creature()
{
    moves = 0;
    pain = 0;
    killer = nullptr;
    speed_base = 100;
    underwater = false;

    Creature::reset_bonuses();

    fake = false;
}

Creature::~Creature() = default;

std::vector<std::string> Creature::get_grammatical_genders() const
{
    // Returning empty list means we use the language-specified default
    return {};
}

void Creature::reset()
{
    reset_bonuses();
    reset_stats();
}

void Creature::bleed() const
{
    const field_type_id &blood_type = bloodType();
    if( blood_type ) {
        get_map().add_splatter( blood_type, pos() );
    }
}

void Creature::reset_bonuses()
{
    num_blocks = 1;
    num_dodges = 1;
    num_blocks_bonus = 0;
    num_dodges_bonus = 0;

    armor_bash_bonus = 0;
    armor_cut_bonus = 0;
    armor_bullet_bonus = 0;

    speed_bonus = 0;
    speed_mult = 0;
    dodge_bonus = 0;
    block_bonus = 0;
    hit_bonus = 0;
}

void Creature::process_turn()
{
    if( is_dead_state() ) {
        return;
    }
    reset_bonuses();

    process_effects();

    // Call this in case any effects have changed our stats
    reset_stats();

    // add an appropriate number of moves
    if( !has_effect( effect_ridden ) ) {
        moves += get_speed();
    }
}

bool Creature::is_underwater() const
{
    return underwater;
}

void Creature::set_underwater( bool x )
{
    underwater = x;
}

bool Creature::digging() const
{
    return false;
}

bool Creature::is_dangerous_fields( const field &fld ) const
{
    // Else check each field to see if it's dangerous to us
    for( auto &dfield : fld ) {
        if( is_dangerous_field( dfield.second ) ) {
            return true;
        }
    }
    // No fields were found to be dangerous, so the field set isn't dangerous
    return false;
}

bool Creature::is_dangerous_field( const field_entry &entry ) const
{
    // If it's dangerous and we're not immune return true, else return false
    return entry.is_dangerous() && !is_immune_field( entry.get_field_type() );
}

bool Creature::sees( const Creature &critter ) const
{
    // Creatures always see themselves (simplifies drawing).
    if( &critter == this ) {
        return true;
    }

    if( critter.is_hallucination() ) {
        // hallucinations are imaginations of the player character, npcs or monsters don't hallucinate.
        // Invisible hallucinations would be pretty useless (nobody would see them at all), therefor
        // the player will see them always.
        return is_player();
    }

    if( !fov_3d && !debug_mode && posz() != critter.posz() ) {
        return false;
    }

    // This check is ridiculously expensive so defer it to after everything else.
    auto visible = []( const Character * ch ) {
        return ch == nullptr || !ch->is_invisible();
    };

    map &here = get_map();
    const Character *ch = critter.as_character();
    const int wanted_range = rl_dist( pos(), critter.pos() );
    // Can always see adjacent monsters on the same level, unless they're through a vehicle wall.
    // We also bypass lighting for vertically adjacent monsters, but still check for floors.
    if( wanted_range <= 1 && ( posz() == critter.posz() || here.sees( pos(), critter.pos(), 1 ) ) ) {
        if( here.obscured_by_vehicle_rotation( pos(), critter.pos() ) ) {
            return false;
        }
        return visible( ch );
    } else if( ( wanted_range > 1 && critter.digging() ) ||
               ( critter.has_flag( MF_NIGHT_INVISIBILITY ) && here.light_at( critter.pos() ) <= lit_level::LOW ) ||
               ( critter.is_underwater() && !is_underwater() && here.is_divable( critter.pos() ) ) ||
               ( here.has_flag_ter_or_furn( TFLAG_HIDE_PLACE, critter.pos() ) &&
                 !( std::abs( posx() - critter.posx() ) <= 1 && std::abs( posy() - critter.posy() ) <= 1 &&
                    std::abs( posz() - critter.posz() ) <= 1 ) ) ) {
        return false;
    }
    if( ch != nullptr ) {
        if( ch->movement_mode_is( CMM_CROUCH ) ) {
            const int coverage = here.obstacle_coverage( pos(), critter.pos() );
            if( coverage < 30 ) {
                return sees( critter.pos(), critter.is_avatar() ) && visible( ch );
            }
            float size_modifier = 1.0;
            switch( ch->get_size() ) {
                case MS_TINY:
                    size_modifier = 2.0;
                    break;
                case MS_SMALL:
                    size_modifier = 1.4;
                    break;
                case MS_MEDIUM:
                    break;
                case MS_LARGE:
                    size_modifier = 0.6;
                    break;
                case MS_HUGE:
                    size_modifier = 0.15;
                    break;
                default:
                    break;
            }
            const int vision_modifier = 30 - 0.5 * coverage * size_modifier;
            if( vision_modifier > 1 ) {
                return sees( critter.pos(), critter.is_avatar(), vision_modifier ) && visible( ch );
            }
            return false;
        }
    }
    return sees( critter.pos(), critter.is_avatar() ) && visible( ch );
}

bool Creature::sees( const tripoint &t, bool is_avatar, int range_mod ) const
{
    if( !fov_3d && posz() != t.z ) {
        return false;
    }

    map &here = get_map();
    const int range_cur = sight_range( here.ambient_light_at( t ) );
    const int range_day = sight_range( default_daylight_level() );
    const int range_night = sight_range( 0 );
    const int range_max = std::max( range_day, range_night );
    const int range_min = std::min( range_cur, range_max );
    const int wanted_range = rl_dist( pos(), t );
    if( wanted_range <= range_min ||
        ( wanted_range <= range_max &&
          here.ambient_light_at( t ) > g->natural_light_level( t.z ) ) ) {
        int range = 0;
        if( here.ambient_light_at( t ) > g->natural_light_level( t.z ) ) {
            range = MAX_VIEW_DISTANCE;
        } else {
            range = range_min;
        }
        if( has_effect( effect_no_sight ) ) {
            range = 1;
        }
        if( range_mod > 0 ) {
            range = std::min( range, range_mod );
        }
        if( is_avatar ) {
            // Special case monster -> player visibility, forcing it to be symmetric with player vision.
            const float player_visibility_factor = g->u.visibility() / 100.0f;
            int adj_range = std::floor( range * player_visibility_factor );
            return adj_range >= wanted_range &&
                   here.get_cache_ref( pos().z ).seen_cache[pos().x][pos().y] > LIGHT_TRANSPARENCY_SOLID;
        } else {
            return here.sees( pos(), t, range );
        }
    } else {
        return false;
    }
}

// Helper function to check if potential area of effect of a weapon overlaps vehicle
// Maybe TODO: If this is too slow, precalculate a bounding box and clip the tested area to it
static bool overlaps_vehicle( const std::set<tripoint> &veh_area, const tripoint &pos,
                              const int area )
{
    for( const tripoint &tmp : tripoint_range<tripoint>( pos - tripoint( area, area, 0 ),
            pos + tripoint( area - 1, area - 1, 0 ) ) ) {
        if( veh_area.count( tmp ) > 0 ) {
            return true;
        }
    }

    return false;
}

Creature *Creature::auto_find_hostile_target( int range, int &boo_hoo, int area )
{
    Creature *target = nullptr;
    player &u = g->u; // Could easily protect something that isn't the player
    constexpr int hostile_adj = 2; // Priority bonus for hostile targets
    const int iff_dist = ( range + area ) * 3 / 2 + 6; // iff check triggers at this distance
    // iff safety margin (degrees). less accuracy, more paranoia
    units::angle iff_hangle = units::from_degrees( 15 + area );
    float best_target_rating = -1.0f; // bigger is better
    units::angle u_angle = {};         // player angle relative to turret
    boo_hoo = 0;         // how many targets were passed due to IFF. Tragically.
    bool self_area_iff = false; // Need to check if the target is near the vehicle we're a part of
    bool area_iff = false;      // Need to check distance from target to player
    bool angle_iff = true;      // Need to check if player is in a cone between us and target
    int pldist = rl_dist( pos(), g->u.pos() );
    map &here = get_map();
    vehicle *in_veh = is_fake() ? veh_pointer_or_null( here.veh_at( pos() ) ) : nullptr;
    if( pldist < iff_dist && sees( g->u ) ) {
        area_iff = area > 0;
        angle_iff = true;
        // Player inside vehicle won't be hit by shots from the roof,
        // so we can fire "through" them just fine.
        const optional_vpart_position vp = here.veh_at( u.pos() );
        if( in_veh && veh_pointer_or_null( vp ) == in_veh && vp->is_inside() ) {
            angle_iff = false; // No angle IFF, but possibly area IFF
        } else if( pldist < 3 ) {
            // granularity increases with proximity
            iff_hangle = ( pldist == 2 ? 30_degrees : 60_degrees );
        }
        u_angle = coord_to_angle( pos(), u.pos() );
    }

    if( area > 0 && in_veh != nullptr ) {
        self_area_iff = true;
    }

    std::vector<Creature *> targets = g->get_creatures_if( [&]( const Creature & critter ) {
        if( critter.is_monster() ) {
            // friendly to the player, not a target for us
            return static_cast<const monster *>( &critter )->friendly == 0;
        }
        if( critter.is_npc() ) {
            // friendly to the player, not a target for us
            return static_cast<const npc *>( &critter )->get_attitude() == NPCATT_KILL;
        }
        // TODO: what about g->u?
        return false;
    } );
    for( auto &m : targets ) {
        if( !sees( *m ) ) {
            // can't see nor sense it
            if( is_fake() && in_veh ) {
                // If turret in the vehicle then
                // Hack: trying yo avoid turret LOS blocking by frames bug by trying to see target from vehicle boundary
                // Or turret wallhack for turret's car
                // TODO: to visibility checking another way, probably using 3D FOV
                std::vector<tripoint> path_to_target = line_to( pos(), m->pos() );
                path_to_target.insert( path_to_target.begin(), pos() );

                // Getting point on vehicle boundaries and on line between target and turret
                bool continueFlag = true;
                do {
                    const optional_vpart_position vp = here.veh_at( path_to_target.back() );
                    vehicle *const veh = vp ? &vp->vehicle() : nullptr;
                    if( in_veh == veh ) {
                        continueFlag = false;
                    } else {
                        path_to_target.pop_back();
                    }
                } while( continueFlag );

                tripoint oldPos = pos();
                setpos( path_to_target.back() ); //Temporary moving targeting npc on vehicle boundary postion
                bool seesFromVehBound = sees( *m ); // And look from there
                setpos( oldPos );
                if( !seesFromVehBound ) {
                    continue;
                }
            } else {
                continue;
            }
        }
        int dist = rl_dist( pos(), m->pos() ) + 1; // rl_dist can be 0
        if( dist > range + 1 || dist < area ) {
            // Too near or too far
            continue;
        }
        // Prioritize big, armed and hostile stuff
        float mon_rating = m->power_rating();
        float target_rating = mon_rating / dist;
        if( mon_rating + hostile_adj <= 0 ) {
            // We wouldn't attack it even if it was hostile
            continue;
        }

        if( in_veh != nullptr && veh_pointer_or_null( here.veh_at( m->pos() ) ) == in_veh ) {
            // No shooting stuff on vehicle we're a part of
            continue;
        }
        if( area_iff && rl_dist( u.pos(), m->pos() ) <= area ) {
            // Player in AoE
            boo_hoo++;
            continue;
        }
        // Hostility check can be expensive, but we need to inform the player of boo_hoo
        // only when the target is actually "hostile enough"
        bool maybe_boo = false;
        if( angle_iff ) {
            units::angle tangle = coord_to_angle( pos(), m->pos() );
            units::angle diff = units::fabs( u_angle - tangle );
            // Player is in the angle and not too far behind the target
            if( ( diff + iff_hangle > 360_degrees || diff < iff_hangle ) &&
                ( dist * 3 / 2 + 6 > pldist ) ) {
                maybe_boo = true;
            }
        }
        if( !maybe_boo && ( ( mon_rating + hostile_adj ) / dist <= best_target_rating ) ) {
            // "Would we skip the target even if it was hostile?"
            // Helps avoid (possibly expensive) attitude calculation
            continue;
        }
        if( m->attitude_to( u ) == A_HOSTILE ) {
            target_rating = ( mon_rating + hostile_adj ) / dist;
            if( maybe_boo ) {
                boo_hoo++;
                continue;
            }
        }
        if( target_rating <= best_target_rating || target_rating <= 0 ) {
            continue; // Handle this late so that boo_hoo++ can happen
        }
        // Expensive check for proximity to vehicle
        if( self_area_iff && overlaps_vehicle( in_veh->get_points(), m->pos(), area ) ) {
            continue;
        }

        target = m;
        best_target_rating = target_rating;
    }
    return target;
}

/*
 * Damage-related functions
 */

int Creature::size_melee_penalty() const
{
    switch( get_size() ) {
        case MS_TINY:
            return 30;
        case MS_SMALL:
            return 15;
        case MS_MEDIUM:
            return 0;
        case MS_LARGE:
            return -10;
        case MS_HUGE:
            return -20;
        default:
            break;
    }

    debugmsg( "Invalid target size %d", get_size() );
    return 0;
}

int Creature::deal_melee_attack( Creature *source, int hitroll )
{
    int hit_spread = hitroll - dodge_roll() - size_melee_penalty();
    if( has_flag( MF_IMMOBILE ) ) {
        // Under normal circumstances, even a clumsy person would
        // not miss a turret.  It should, however, be possible to
        // miss a smaller target, especially when wielding a
        // clumsy weapon or when severely encumbered.
        hit_spread += 40;
    }

    // If attacker missed call targets on_dodge event
    if( hit_spread <= 0 && source != nullptr && !source->is_hallucination() ) {
        on_dodge( source, source->get_melee() );
    }

    return hit_spread;
}

void Creature::deal_melee_hit( Creature *source, int hit_spread, bool critical_hit,
                               const damage_instance &dam, dealt_damage_instance &dealt_dam )
{
    if( source == nullptr || source->is_hallucination() ) {
        dealt_dam.bp_hit = anatomy_id( "human_anatomy" )->random_body_part()->token;
        return;
    }
    // If carrying a rider, there is a chance the hits may hit rider instead.
    // melee attack will start off as targeted at mount
    if( has_effect( effect_ridden ) ) {
        monster *mons = dynamic_cast<monster *>( this );
        if( mons && mons->mounted_player ) {
            if( !mons->has_flag( MF_MECH_DEFENSIVE ) &&
                one_in( std::max( 2, mons->get_size() - mons->mounted_player->get_size() ) ) ) {
                mons->mounted_player->deal_melee_hit( source, hit_spread, critical_hit, dam, dealt_dam );
                return;
            }
        }
    }
    damage_instance d = dam; // copy, since we will mutate in block_hit
    bodypart_id bp_hit = convert_bp( select_body_part( source, hit_spread ) ).id();
    const body_part bp_token = bp_hit->token;
    block_hit( source, bp_hit, d );

    on_hit( source, bp_hit ); // trigger on-gethit events
    dealt_dam = deal_damage( source, bp_hit, d );
    dealt_dam.bp_hit = bp_token;
}

namespace ranged
{

void print_dmg_msg( Creature &target, Creature *source, const dealt_damage_instance &dealt_dam,
                    hit_tier ht )
{
    std::string message;
    game_message_type sct_color = m_neutral;
    switch( ht ) {
        case hit_tier::grazing:
            message = _( "Grazing hit." );
            sct_color = m_grazing;
        case hit_tier::normal:
            break;
        case hit_tier::critical:
            message = _( "Critical!" );
            sct_color = m_critical;
            break;
    }

    if( source != nullptr && !message.empty() ) {
        source->add_msg_if_player( m_good, message );
    }

    if( dealt_dam.total_damage() == 0 ) {
        //~ 1$ - monster name, 2$ - character's bodypart or monster's skin/armor
        add_msg( _( "The shot reflects off %1$s %2$s!" ), target.disp_name( true ),
                 ( target.is_monster() || dealt_dam.bp_hit == num_bp ) ?
                 target.skin_name() :
                 body_part_name_accusative( dealt_dam.bp_hit ) );
    } else if( target.is_player() ) {
        //monster hits player ranged
        //~ Hit message. 1$s is bodypart name in accusative. 2$d is damage value.
        target.add_msg_if_player( m_bad, _( "You were hit in the %1$s for %2$d damage." ),
                                  body_part_name_accusative( dealt_dam.bp_hit ),
                                  dealt_dam.total_damage() );
    } else if( source != nullptr ) {
        if( source->is_player() ) {
            //player hits monster ranged
            SCT.add( target.pos().xy(),
                     direction_from( point_zero, target.pos().xy() - source->pos().xy() ),
                     get_hp_bar( dealt_dam.total_damage(), target.get_hp_max(), true ).first,
                     m_good, message, sct_color );

            if( target.get_hp() > 0 ) {
                SCT.add( target.pos().xy(),
                         direction_from( point_zero, target.pos().xy() - source->pos().xy() ),
                         get_hp_bar( target.get_hp(), target.get_hp_max(), true ).first, m_good,
                         //~ "hit points", used in scrolling combat text
                         _( "hp" ), m_neutral, "hp" );
            } else {
                SCT.removeCreatureHP();
            }

            //~ %1$s: creature name, %2$d: damage value
            add_msg( m_good, _( "You hit %1$s for %2$d damage." ),
                     target.disp_name(), dealt_dam.total_damage() );
        } else {
            //~ 1$ - shooter, 2$ - target
            add_msg( _( "%1$s shoots %2$s." ),
                     source->disp_name(), target.disp_name() );
        }
    }
}

dealt_damage_instance hit_with_aoe( Creature &target, Creature *source, const damage_instance &di )
{
    const auto all_body_parts = target.get_body();
    float hit_size_sum = std::accumulate( all_body_parts.begin(), all_body_parts.end(), 0.0f,
    []( float acc, const std::pair<bodypart_str_id, bodypart> &pr ) {
        return acc + pr.first->hit_size;
    } );
    dealt_damage_instance dealt_damage;
    for( const std::pair<const bodypart_str_id, bodypart> &pr : all_body_parts ) {
        damage_instance impact = di;
        impact.mult_damage( pr.first->hit_size / hit_size_sum );
        dealt_damage_instance bp_damage = target.deal_damage( source, pr.first.id(), impact );
        for( size_t i = 0; i < dealt_damage.dealt_dams.size(); i++ ) {
            dealt_damage.dealt_dams[i] += bp_damage.dealt_dams[i];
        }
    }

    dealt_damage.bp_hit = bodypart_str_id::NULL_ID()->token;
    if( get_player_character().sees( target ) ) {
        ranged::print_dmg_msg( target, source, dealt_damage );
    }

    if( target.has_effect( effect_ridden ) ) {
        monster *mons = dynamic_cast<monster *>( &target );
        if( mons && mons->mounted_player && !mons->has_flag( MF_MECH_DEFENSIVE ) ) {
            // TODO: Return value
            hit_with_aoe( *mons->mounted_player, source, di );
        }
    }

    return dealt_damage;
}

} // namespace ranged

/**
 * Attempts to harm a creature with a projectile.
 *
 * @param source Pointer to the creature who shot the projectile.
 * @param attack A structure describing the attack and its results.
 * @param print_messages enables message printing by default.
 */
void Creature::deal_projectile_attack( Creature *source, dealt_projectile_attack &attack )
{
    const bool magic = attack.proj.has_effect( ammo_effect_magic );
    const bool targetted_crit_allowed = !attack.proj.has_effect( ammo_effect_NO_CRIT );
    const double missed_by = attack.missed_by;
    if( missed_by >= 1.0 && !magic ) {
        // Total miss
        return;
    }
    // If carrying a rider, there is a chance the hits may hit rider instead.
    if( has_effect( effect_ridden ) ) {
        monster *mons = dynamic_cast<monster *>( this );
        if( mons && mons->mounted_player ) {
            if( !mons->has_flag( MF_MECH_DEFENSIVE ) &&
                one_in( std::max( 2, mons->get_size() - mons->mounted_player->get_size() ) ) ) {
                mons->mounted_player->deal_projectile_attack( source, attack );
                return;
            }
        }
    }
    const projectile &proj = attack.proj;
    dealt_damage_instance &dealt_dam = attack.dealt_dam;

    const bool u_see_this = g->u.sees( *this );

    const int avoid_roll = dodge_roll();
    // Do dice(10, speed) instead of dice(speed, 10) because speed could potentially be > 10000
    const int diff_roll = dice( 10, proj.speed );
    // Partial dodge, capped at [0.0, 1.0], added to missed_by
    const double dodge_rescaled = avoid_roll / static_cast<double>( diff_roll );
    const double goodhit = missed_by + std::max( 0.0, std::min( 1.0, dodge_rescaled ) );

    if( goodhit >= 1.0 && !magic ) {
        attack.missed_by = 1.0; // Arbitrary value
        // "Avoid" rather than "dodge", because it includes removing self from the line of fire
        //  rather than just Matrix-style bullet dodging
        if( source != nullptr && g->u.sees( *source ) ) {
            add_msg_player_or_npc(
                m_warning,
                _( "You avoid %s projectile!" ),
                _( "<npcname> avoids %s projectile." ),
                source->disp_name( true ) );
        } else {
            add_msg_player_or_npc(
                m_warning,
                _( "You avoid an incoming projectile!" ),
                _( "<npcname> avoids an incoming projectile." ) );
        }
        return;
    }

    // Bounce applies whether it does damage or not.
    if( proj.has_effect( ammo_effect_BOUNCE ) ) {
        add_effect( effect_bounced, 1_turns );
    }

    bodypart_id bp_hit;
    double hit_value = missed_by + rng_float( -0.5, 0.5 );
    if( targetted_crit_allowed || magic ) { //default logic for selecting bodypart
        if( goodhit < accuracy_critical && hit_value <= 0.2 ) {
            bp_hit = bodypart_str_id( "head" );
        } else if( hit_value <= 0.4 || magic ) {
            bp_hit = bodypart_str_id( "torso" );
        } else if( one_in( 4 ) ) {
            if( one_in( 2 ) ) {
                bp_hit = bodypart_str_id( "leg_l" );
            } else {
                bp_hit = bodypart_str_id( "leg_r" );
            }
        } else {
            if( one_in( 2 ) ) {
                bp_hit = bodypart_str_id( "arm_l" );
            } else {
                bp_hit = bodypart_str_id( "arm_r" );
            }
        }
    } else { // no crit logic for selecting bodypart
        if( hit_value <= 0.4 && !one_in( 4 ) ) {
            bp_hit = one_in( 3 ) ? bodypart_str_id( "head" ) : bodypart_str_id( "torso" );
        } else if( one_in( 4 ) ) {
            if( one_in( 2 ) ) {
                bp_hit = bodypart_str_id( "leg_l" );
            } else {
                bp_hit = bodypart_str_id( "leg_r" );
            }
        } else {
            if( one_in( 2 ) ) {
                bp_hit = bodypart_str_id( "arm_l" );
            } else {
                bp_hit = bodypart_str_id( "arm_r" );
            }
        }
    }


    double damage_mult = 1.0;

    ranged::hit_tier ht = ranged::hit_tier::normal;
    if( magic ) {
        damage_mult *= rng_float( 0.9, 1.1 );
    } else if( targetted_crit_allowed && goodhit < accuracy_critical ) {
        ht = ranged::hit_tier::critical;
        damage_mult *= 1.5;
    } else if( goodhit < accuracy_standard ) {
        damage_mult *= rng_float( 0.9, 1.1 );
    } else if( goodhit < accuracy_grazing ) {
        ht = ranged::hit_tier::grazing;
        damage_mult *= 0.5;
    }

    attack.missed_by = goodhit;

    // copy it, since we're mutating
    damage_instance impact = proj.impact;
    if( damage_mult > 0.0f && proj.has_effect( ammo_effect_NO_DAMAGE_SCALING ) ) {
        damage_mult = 1.0f;
    }

    impact.mult_damage( damage_mult );

    if( proj.has_effect( ammo_effect_NOGIB ) ) {
        float dmg_ratio = static_cast<float>( impact.total_damage() ) / get_hp_max( bp_hit );
        if( dmg_ratio > 1.25f ) {
            impact.mult_damage( 1.0f / dmg_ratio );
        }
    }

    dealt_dam = deal_damage( source, bp_hit, impact );
    dealt_dam.bp_hit = bp_hit->token;

    // Apply ammo effects to target.
    if( proj.has_effect( ammo_effect_TANGLE ) ) {
        monster *z = dynamic_cast<monster *>( this );
        player *n = dynamic_cast<player *>( this );
        // if its a tameable animal, its a good way to catch them if they are running away, like them ranchers do!
        // we assume immediate success, then certain monster types immediately break free in monster.cpp move_effects()
        if( z ) {
            const item &drop_item = proj.get_drop();
            if( !drop_item.is_null() ) {
                z->add_effect( effect_tied, 1_turns, num_bp );
                z->tied_item = cata::make_value<item>( drop_item );
            } else {
                add_msg( m_debug, "projectile with TANGLE effect, but no drop item specified" );
            }
        } else if( n && !is_immune_effect( effect_downed ) ) {
            // no tied up effect for people yet, just down them and stun them, its close enough to the desired effect.
            // we can assume a person knows how to untangle their legs eventually and not panic like an animal.
            add_effect( effect_downed, 1_turns );
            // stunned to simulate staggering around and stumbling trying to get the entangled thing off of them.
            add_effect( effect_stunned, rng( 3_turns, 8_turns ) );
        }
    }
    if( proj.has_effect( ammo_effect_INCENDIARY ) ) {
        if( made_of( material_id( "veggy" ) ) || made_of_any( cmat_flammable ) ) {
            add_effect( effect_onfire, rng( 2_turns, 6_turns ), bp_hit->token );
        } else if( made_of_any( cmat_flesh ) && one_in( 4 ) ) {
            add_effect( effect_onfire, rng( 1_turns, 4_turns ), bp_hit->token );
        }
    } else if( proj.has_effect( ammo_effect_IGNITE ) ) {
        if( made_of( material_id( "veggy" ) ) || made_of_any( cmat_flammable ) ) {
            add_effect( effect_onfire, 6_turns, bp_hit->token );
        } else if( made_of_any( cmat_flesh ) ) {
            add_effect( effect_onfire, 10_turns, bp_hit->token );
        }
    }

    if( bp_hit == bodypart_str_id( "head" ) && proj.has_effect( ammo_effect_BLINDS_EYES ) ) {
        // TODO: Change this to require bp_eyes
        add_env_effect( effect_blind, bp_eyes, 5, rng( 3_turns, 10_turns ) );
    }

    if( proj.has_effect( ammo_effect_APPLY_SAP ) ) {
        add_effect( effect_sap, 1_turns * dealt_dam.total_damage() );
    }
    if( proj.has_effect( ammo_effect_PARALYZEPOISON ) && dealt_dam.total_damage() > 0 ) {
        add_msg_if_player( m_bad, _( "You feel poison coursing through your body!" ) );
        add_effect( effect_paralyzepoison, 5_minutes );
    }

    int stun_strength = 0;
    if( proj.has_effect( ammo_effect_BEANBAG ) ) {
        stun_strength = 4;
    }
    if( proj.has_effect( ammo_effect_LARGE_BEANBAG ) ) {
        stun_strength = 16;
    }
    if( stun_strength > 0 ) {
        switch( get_size() ) {
            case MS_TINY:
                stun_strength *= 4;
                break;
            case MS_SMALL:
                stun_strength *= 2;
                break;
            case MS_MEDIUM:
            default:
                break;
            case MS_LARGE:
                stun_strength /= 2;
                break;
            case MS_HUGE:
                stun_strength /= 4;
                break;
        }
        add_effect( effect_stunned, 1_turns * rng( stun_strength / 2, stun_strength ) );
    }

    if( u_see_this ) {
        if( damage_mult == 0 ) {
            if( source != nullptr ) {
                add_msg( source->is_player() ? _( "You miss!" ) : _( "The shot misses!" ) );
            }
        } else {
            ranged::print_dmg_msg( *this, source, dealt_dam, ht );
        }
    }

    check_dead_state();
    attack.hit_critter = this;
    attack.missed_by = goodhit;
}

dealt_damage_instance Creature::deal_damage( Creature *source, bodypart_id bp,
        const damage_instance &dam )
{
    if( is_dead_state() ) {
        return dealt_damage_instance();
    }
    int total_damage = 0;
    int total_pain = 0;
    damage_instance d = dam; // copy, since we will mutate in absorb_hit

    dealt_damage_instance dealt_dams;
    absorb_hit( bp, d );

    // Add up all the damage units dealt
    for( const auto &it : d.damage_units ) {
        int cur_damage = 0;
        deal_damage_handle_type( it, bp, cur_damage, total_pain );
        if( cur_damage > 0 ) {
            dealt_dams.dealt_dams[ it.type ] += cur_damage;
            total_damage += cur_damage;
        }
    }

    mod_pain( total_pain );

    apply_damage( source, bp, total_damage );
    return dealt_dams;
}
void Creature::deal_damage_handle_type( const damage_unit &du, bodypart_id bp, int &damage,
                                        int &pain )
{
    // Handles ACIDPROOF, electric immunity etc.
    if( is_immune_damage( du.type ) ) {
        return;
    }

    // Apply damage multiplier from skill, critical hits or grazes after all other modifications.
    const int adjusted_damage = du.amount * du.damage_multiplier;
    if( adjusted_damage <= 0 ) {
        return;
    }

    float div = 4.0f;

    switch( du.type ) {
        case DT_BASH:
            // Bashing damage is less painful
            div = 5.0f;
            break;

        case DT_HEAT:
            // heat damage sets us on fire sometimes
            if( rng( 0, 100 ) < adjusted_damage ) {
                add_effect( effect_onfire, rng( 1_turns, 3_turns ), bp->token );
            }
            break;

        case DT_ELECTRIC:
            // Electrical damage adds a major speed/dex debuff
            add_effect( effect_zapped, 1_turns * std::max( adjusted_damage, 2 ) );
            break;

        case DT_ACID:
            // Acid damage and acid burns are more painful
            div = 3.0f;
            break;

        default:
            break;
    }

    on_damage_of_type( adjusted_damage, du.type, bp );

    damage += adjusted_damage;
    pain += roll_remainder( adjusted_damage / div );
}

void Creature::on_dodge( Creature */*source*/, int /*difficulty*/ )
{

}

/*
 * State check functions
 */

bool Creature::is_warm() const
{
    return true;
}

bool Creature::in_species( const species_id & ) const
{
    return false;
}

bool Creature::is_fake() const
{
    return fake;
}

void Creature::set_fake( const bool fake_value )
{
    fake = fake_value;
}

void Creature::add_effect( const effect &eff, bool force, bool deferred )
{
    add_effect( eff.get_id(), eff.get_duration(), eff.get_bp(), eff.get_intensity(),
                force, deferred );
}


void Creature::add_effect( const efftype_id &eff_id, const time_duration &dur, body_part bp,
                           int intensity, bool force, bool deferred )
{
    add_effect( eff_id, dur, convert_bp( bp ), intensity, force, deferred );
}

void Creature::add_effect( const efftype_id &eff_id, const time_duration &dur,
                           const bodypart_str_id &base_bp,
                           int intensity, bool force, bool deferred )
{
    // Check our innate immunity
    if( !force && is_immune_effect( eff_id ) ) {
        return;
    }
    if( eff_id == efftype_id( "knockdown" ) && ( has_effect( effect_ridden ) ||
            has_effect( effect_riding ) ) ) {
        monster *mons = dynamic_cast<monster *>( this );
        if( mons && mons->mounted_player ) {
            mons->mounted_player->forced_dismount();
        }
    }

    if( !eff_id.is_valid() ) {
        debugmsg( "Invalid effect, ID: %s", eff_id.c_str() );
        return;
    }
    const effect_type &type = eff_id.obj();

    bodypart_str_id bp = base_bp;
    // Mutate to a main (HP'd) body_part if necessary.
    if( type.get_main_parts() ) {
        bp = bp->main_part;
    }

    bool found = false;
    // Check if we already have it
    auto matching_map = effects->find( eff_id );
    if( matching_map != effects->end() ) {
        auto &bodyparts = matching_map->second;
        auto found_effect = bodyparts.find( bp );
        if( found_effect != bodyparts.end() && !found_effect->second.is_removed() ) {
            found = true;
            effect &e = found_effect->second;
            const int prev_int = e.get_intensity();
            // If we do, mod the duration, factoring in the mod value
            e.mod_duration( time_duration::from_turns( static_cast<int>(
                                to_turns<int64_t>( dur ) * e.get_dur_add_perc() / 100 ) ) );
            // Limit to max duration
            if( e.get_max_duration() > 0_turns && e.get_duration() > e.get_max_duration() ) {
                e.set_duration( e.get_max_duration() );
            }
            // int_dur_factor overrides all other intensity settings
            // ...but it's handled in set_duration, so explicitly do nothing here
            if( e.get_int_dur_factor() > 0_turns ) {
                // Set intensity if value is given
            } else if( intensity > 0 ) {
                e.set_intensity( intensity );
                // Else intensity uses the type'd step size if it already exists
            } else if( e.get_int_add_val() != 0 ) {
                e.mod_intensity( e.get_int_add_val() );
            }

            // Bound intensity by [1, max intensity]
            if( e.get_intensity() < 1 ) {
                add_msg( m_debug, "Bad intensity, ID: %s", e.get_id().c_str() );
                e.set_intensity( 1 );
            } else if( e.get_intensity() > e.get_max_intensity() ) {
                e.set_intensity( e.get_max_intensity() );
            }
            if( e.get_intensity() != prev_int ) {
                on_effect_int_change( e.get_id(), e.get_intensity(), e.get_bp() );
            }
        }
    }

    if( !found ) {
        // If we don't already have it then add a new one

        // Then check if the effect is blocked by another
        for( const auto &elem : *effects ) {
            for( auto &_effect_it : elem.second ) {
                if( !_effect_it.second.is_removed() )
                    for( const auto &blocked_effect : _effect_it.second.get_blocks_effects() ) {
                        if( blocked_effect == eff_id ) {
                            // The effect is blocked by another, return
                            return;
                        }
                    }
            }
        }

        // Now we can make the new effect for application
        effect e( &type, dur, bp, intensity, calendar::turn );
        // Bound to max duration
        if( e.get_max_duration() > 0_turns && e.get_duration() > e.get_max_duration() ) {
            e.set_duration( e.get_max_duration() );
        }

        // Force intensity if it is duration based
        if( e.get_int_dur_factor() != 0_turns ) {
            // + 1 here so that the lowest is intensity 1, not 0
            e.set_intensity( e.get_duration() / e.get_int_dur_factor() + 1 );
        }
        // Bound new effect intensity by [1, max intensity]
        if( e.get_intensity() < 1 ) {
            add_msg( m_debug, "Bad intensity, ID: %s", e.get_id().c_str() );
            e.set_intensity( 1 );
        } else if( e.get_intensity() > e.get_max_intensity() ) {
            e.set_intensity( e.get_max_intensity() );
        }
        ( *effects )[eff_id][bp] = e;
        if( Character *ch = as_character() ) {
            g->events().send<event_type::character_gains_effect>( ch->getID(), eff_id );
            if( is_player() && !type.get_apply_message().empty() ) {
                add_msg( type.gain_game_message_type(), _( type.get_apply_message() ) );
            }
        }
        on_effect_int_change( e.get_id(), e.get_intensity(), e.get_bp() );
        // Perform any effect addition effects.
        // only when not deferred
        if( !deferred ) {
            process_one_effect( e, true );
        }
    }
}
bool Creature::add_env_effect( const efftype_id &eff_id, body_part vector, int strength,
                               const time_duration &dur, body_part bp, int intensity, bool force )
{
    if( !force && is_immune_effect( eff_id ) ) {
        return false;
    }

    if( dice( strength, 3 ) > dice( get_env_resist( convert_bp( vector ).id() ), 3 ) ) {
        // Only add the effect if we fail the resist roll
        // Don't check immunity (force == true), because we did check above
        add_effect( eff_id, dur, bp, intensity, true );
        return true;
    } else {
        return false;
    }
}
void Creature::clear_effects()
{
    for( auto &elem : *effects ) {
        for( auto &_effect_it : elem.second ) {
            remove_effect( elem.first, _effect_it.first );
        }
    }
}
bool Creature::remove_effect( const efftype_id &eff_id, body_part bp )
{
    return remove_effect( eff_id, convert_bp( bp ) );
}
bool Creature::remove_effect( const efftype_id &eff_id, const bodypart_str_id &bp )
{
    if( !has_effect( eff_id, bp ) ) {
        //Effect doesn't exist, so do nothing
        return false;
    }
    const effect_type &type = eff_id.obj();

    Character *ch = as_character();
    if( ch != nullptr ) {
        if( is_player() ) {
            if( !type.get_remove_message().empty() ) {
                add_msg( type.lose_game_message_type(), _( type.get_remove_message() ) );
            }
        }
        g->events().send<event_type::character_loses_effect>( ch->getID(), eff_id );
    }

    // num_bp means remove all of a given effect id
    if( !bp ) {
        for( auto &it : ( *effects )[eff_id] ) {
            auto &e = it.second;
            if( !e.is_removed() ) {
                on_effect_int_change( e.get_id(), 0, e.get_bp() );
                e.set_removed();
            }
        }
    } else {
        effect &e = get_effect( eff_id, bp->token );
        on_effect_int_change( e.get_id(), 0, e.get_bp() );
        e.set_removed();
    }
    // Sleep is a special case, since it affects max sight range and other effects
    // Must be below the set_removed above or we'll get an infinite loop
    if( ch != nullptr && eff_id == effect_sleep ) {
        ch->wake_up();
    }
    return true;
}
bool Creature::has_effect( const efftype_id &eff_id, body_part bp ) const
{
    return has_effect( eff_id, convert_bp( bp ) );
}
bool Creature::has_effect( const efftype_id &eff_id, const bodypart_str_id &bp ) const
{
    // num_bp means anything targeted or not
    if( !bp ) {
        auto got = effects->find( eff_id );
        return got != effects->end() && !got->second.begin()->second.is_removed();
    } else {
        auto got_outer = effects->find( eff_id );
        if( got_outer != effects->end() ) {
            auto got_inner = got_outer->second.find( bp );
            if( got_inner != got_outer->second.end() && !got_inner->second.is_removed() ) {
                return true;
            }
        }
        return false;
    }
}

bool Creature::has_effect_with_flag( const std::string &flag, body_part bp ) const
{
    const auto &tmp = convert_bp( bp ).id();
    for( const auto &elem : *effects ) {
        for( const auto &_it : elem.second ) {
            if( tmp == _it.first && !_it.second.is_removed() && _it.second.has_flag( flag ) ) {
                return true;
            }
        }
    }
    return false;
}

effect &Creature::get_effect( const efftype_id &eff_id, body_part bp )
{
    return const_cast<effect &>( const_cast<const Creature *>( this )->get_effect( eff_id, bp ) );
}

const effect &Creature::get_effect( const efftype_id &eff_id, body_part bp ) const
{
    auto got_outer = effects->find( eff_id );
    if( got_outer != effects->end() ) {
        auto got_inner = got_outer->second.find( convert_bp( bp ) );
        if( got_inner != got_outer->second.end() && !got_inner->second.is_removed() ) {
            return got_inner->second;
        }
    }
    return effect::null_effect;
}
std::vector<const effect *> Creature::get_all_effects_of_type( const efftype_id &eff_id ) const
{
    std::vector<const effect *> ret;
    auto got_outer = effects->find( eff_id );
    if( got_outer != effects->end() ) {
        for( const auto &pr : got_outer->second ) {
            ret.push_back( &pr.second );
        }
    }
    return ret;
}
time_duration Creature::get_effect_dur( const efftype_id &eff_id, body_part bp ) const
{
    const effect &eff = get_effect( eff_id, bp );
    if( !eff.is_null() ) {
        return eff.get_duration();
    }

    return 0_turns;
}
int Creature::get_effect_int( const efftype_id &eff_id, body_part bp ) const
{
    const effect &eff = get_effect( eff_id, bp );
    if( !eff.is_null() ) {
        return eff.get_intensity();
    }

    return 0;
}
void Creature::process_effects()
{
    process_effects_internal();

    // id's and body_part's of all effects to be removed. If we ever get player or
    // monster specific removals these will need to be moved down to that level and then
    // passed in to this function.
    std::vector<std::pair<efftype_id, bodypart_str_id>> to_remove;

    // Decay/removal of effects
    for( auto &elem : *effects ) {
        for( auto &_it : elem.second ) {
            if( _it.second.is_removed() ) {
                to_remove.emplace_back( elem.first, _it.first );
                continue;
            }
            // Add any effects that others remove to the removal list
            for( const efftype_id &removed_effect : _it.second.get_removes_effects() ) {
                to_remove.emplace_back( removed_effect, bodypart_str_id::NULL_ID() );
            }
            effect &e = _it.second;
            const int prev_int = e.get_intensity();
            // Run decay effects, marking effects for removal as necessary.
            if( e.decay( calendar::turn, is_player() ) ) {
                to_remove.emplace_back( elem.first, _it.first );
            }

            if( e.get_intensity() != prev_int && e.get_duration() > 0_turns ) {
                on_effect_int_change( e.get_id(), e.get_intensity(), e.get_bp() );
            }
        }
    }

    // Run the on-remove effects
    for( const std::pair<efftype_id, bodypart_str_id> &r : to_remove ) {
        remove_effect( r.first, r.second );
    }
    // Actually remove effects. This should be the last thing done in process_effects().
    for( const std::pair<efftype_id, bodypart_str_id> &r : to_remove ) {
        if( !r.second ) {
            effects->erase( r.first );
        } else {
            ( *effects )[r.first].erase( r.second );
            // If there are no more effects of a given type remove the type map
            if( ( *effects )[r.first].empty() ) {
                effects->erase( r.first );
            }
        }
    }
}

bool Creature::resists_effect( const effect &e ) const
{
    for( auto &i : e.get_resist_effects() ) {
        if( has_effect( i ) ) {
            return true;
        }
    }
    for( auto &i : e.get_resist_traits() ) {
        if( has_trait( i ) ) {
            return true;
        }
    }
    return false;
}

bool Creature::has_trait( const trait_id &/*flag*/ ) const
{
    return false;
}

// Methods for setting/getting misc key/value pairs.
void Creature::set_value( const std::string &key, const std::string &value )
{
    values[ key ] = value;
}

void Creature::remove_value( const std::string &key )
{
    values.erase( key );
}

std::string Creature::get_value( const std::string &key ) const
{
    auto it = values.find( key );
    return ( it == values.end() ) ? "" : it->second;
}

void Creature::mod_pain( int npain )
{
    mod_pain_noresist( npain );
}

void Creature::mod_pain_noresist( int npain )
{
    set_pain( pain + npain );
}

void Creature::set_pain( int npain )
{
    npain = std::max( npain, 0 );
    if( pain != npain ) {
        pain = npain;
        on_stat_change( "pain", pain );
    }
}

int Creature::get_pain() const
{
    return pain;
}

int Creature::get_perceived_pain() const
{
    return get_pain();
}

std::pair<std::string, nc_color> Creature::get_pain_description() const
{
    float scale = get_perceived_pain() / 10.f;
    std::string pain_string;
    nc_color pain_color = c_yellow;
    if( scale > 7 ) {
        pain_string = _( "Severe pain" );
    } else if( scale > 6 ) {
        pain_string = _( "Intense pain" );
    } else if( scale > 5 ) {
        pain_string = _( "Unmanageable pain" );
    } else if( scale > 4 ) {
        pain_string = _( "Distressing pain" );
    } else if( scale > 3 ) {
        pain_string = _( "Distracting pain" );
    } else if( scale > 2 ) {
        pain_string = _( "Moderate pain" );
    } else if( scale > 1 ) {
        pain_string = _( "Mild pain" );
    } else if( scale > 0 ) {
        pain_string = _( "Minimal pain" );
    } else {
        pain_string = _( "No pain" );
        pain_color = c_white;
    }
    return std::make_pair( pain_string, pain_color );
}

int Creature::get_moves() const
{
    return moves;
}
void Creature::mod_moves( int nmoves )
{
    moves += nmoves;
}
void Creature::set_moves( int nmoves )
{
    moves = nmoves;
}

bool Creature::in_sleep_state() const
{
    return has_effect( effect_sleep ) || has_effect( effect_lying_down ) ||
           has_effect( effect_npc_suspend );
}

/*
 * Killer-related things
 */
Creature *Creature::get_killer() const
{
    return killer;
}

void Creature::set_killer( Creature *const killer )
{
    // Only the first killer will be stored, calling set_killer again with a different
    // killer would mean it's called on a dead creature and therefore ignored.
    if( killer != nullptr && !killer->is_fake() && this->killer == nullptr ) {
        this->killer = killer;
    }
}

int Creature::get_num_blocks() const
{
    return num_blocks + num_blocks_bonus;
}
int Creature::get_num_dodges() const
{
    return num_dodges + num_dodges_bonus;
}
int Creature::get_num_blocks_bonus() const
{
    return num_blocks_bonus;
}
int Creature::get_num_dodges_bonus() const
{
    return num_dodges_bonus;
}
int Creature::get_num_dodges_base() const
{
    return num_dodges;
}

// currently this is expected to be overridden to actually have use
int Creature::get_env_resist( bodypart_id ) const
{
    return 0;
}
int Creature::get_armor_bash( bodypart_id ) const
{
    return armor_bash_bonus;
}
int Creature::get_armor_cut( bodypart_id ) const
{
    return armor_cut_bonus;
}
int Creature::get_armor_bullet( bodypart_id ) const
{
    return armor_bullet_bonus;
}
int Creature::get_armor_bash_base( bodypart_id ) const
{
    return armor_bash_bonus;
}
int Creature::get_armor_cut_base( bodypart_id ) const
{
    return armor_cut_bonus;
}
int Creature::get_armor_bullet_base( bodypart_id ) const
{
    return armor_bullet_bonus;
}
int Creature::get_armor_bash_bonus() const
{
    return armor_bash_bonus;
}
int Creature::get_armor_cut_bonus() const
{
    return armor_cut_bonus;
}
int Creature::get_armor_bullet_bonus() const
{
    return armor_bullet_bonus;
}


int Creature::get_speed() const
{
    int speed = round( ( get_speed_base() + get_speed_bonus() ) * ( 1 + get_speed_mult() ) );
    return std::max( static_cast<int>( round( 0.25 * get_speed_base() ) ), speed );
}
float Creature::get_dodge() const
{
    return get_dodge_base() + get_dodge_bonus();
}
float Creature::get_hit() const
{
    return get_hit_base() + get_hit_bonus();
}

anatomy_id Creature::get_anatomy() const
{
    return creature_anatomy;
}

void Creature::set_anatomy( anatomy_id anat )
{
    creature_anatomy = anat;
}

const std::map<bodypart_str_id, bodypart> &Creature::get_body() const
{
    return body;
}

void Creature::set_body()
{
    body.clear();
    for( const bodypart_id &bp : get_anatomy()->get_bodyparts() ) {
        body.emplace( bp.id(), bodypart( bp.id() ) );
    }
}

bodypart *Creature::get_part( const bodypart_id &id )
{
    auto found = body.find( id.id() );
    if( found == body.end() ) {
        debugmsg( "Could not find bodypart %s in %s's body", id.id().c_str(), get_name() );
        return nullptr;
    }
    return &found->second;
}

bodypart Creature::get_part( const bodypart_id &id ) const
{
    auto found = body.find( id.id() );
    if( found == body.end() ) {
        debugmsg( "Could not find bodypart %s in %s's body", id.id().c_str(), get_name() );
        return bodypart();
    }
    return found->second;
}

int Creature::get_part_hp_cur( const bodypart_id &id ) const
{
    return get_part( id ).get_hp_cur();
}

int Creature::get_part_hp_max( const bodypart_id &id ) const
{
    return get_part( id ).get_hp_max();
}

int Creature::get_part_healed_total( const bodypart_id &id ) const
{
    return get_part( id ).get_healed_total();
}

void Creature::set_part_hp_cur( const bodypart_id &id, int set )
{
    get_part( id )->set_hp_cur( set );
}

void Creature::set_part_hp_max( const bodypart_id &id, int set )
{
    get_part( id )->set_hp_max( set );
}

void Creature::set_part_healed_total( const bodypart_id &id, int set )
{
    get_part( id )->set_healed_total( set );
}

void Creature::mod_part_hp_cur( const bodypart_id &id, int mod )
{
    get_part( id )->mod_hp_cur( mod );
}

void Creature::mod_part_hp_max( const bodypart_id &id, int mod )
{
    get_part( id )->mod_hp_max( mod );
}

void Creature::mod_part_healed_total( const bodypart_id &id, int mod )
{
    get_part( id )->mod_healed_total( mod );
}

void Creature::set_all_parts_hp_cur( const int set )
{
    for( std::pair<const bodypart_str_id, bodypart> &elem : body ) {
        elem.second.set_hp_cur( set );
    }
}

void Creature::set_all_parts_hp_to_max()
{
    for( std::pair<const bodypart_str_id, bodypart> &elem : body ) {
        elem.second.set_hp_to_max();
    }
}


bodypart_id Creature::get_random_body_part( bool main ) const
{
    // TODO: Refuse broken limbs, adjust for mutations
    const bodypart_id &part = get_anatomy()->random_body_part();
    return main ? part->main_part.id() : part;
}

std::vector<bodypart_id> Creature::get_all_body_parts( bool only_main ) const
{
    std::vector<bodypart_id> all_bps;
    for( const std::pair<const bodypart_str_id, bodypart> &elem : body ) {
        if( only_main && elem.first->main_part != elem.first ) {
            continue;
        }
        all_bps.push_back( elem.first );
    }

    return  all_bps;
}

int Creature::get_hp( const bodypart_id &bp ) const
{
    if( bp != bodypart_id( "num_bp" ) ) {
        return get_part_hp_cur( bp );
    }
    int hp_total = 0;
    for( const std::pair<const bodypart_str_id, bodypart> &elem : get_body() ) {
        hp_total += elem.second.get_hp_cur();
    }
    return hp_total;
}

int Creature::get_hp() const
{
    return get_hp( bodypart_id( "num_bp" ) );
}

int Creature::get_hp_max( const bodypart_id &bp ) const
{
    if( bp != bodypart_id( "num_bp" ) ) {
        return get_part_hp_max( bp );
    }
    int hp_total = 0;
    for( const std::pair<const bodypart_str_id, bodypart> &elem : get_body() ) {
        hp_total += elem.second.get_hp_max();
    }
    return hp_total;
}

int Creature::get_hp_max() const
{
    return get_hp_max( bodypart_id( "num_bp" ) );
}

int Creature::get_speed_base() const
{
    return speed_base;
}
int Creature::get_speed_bonus() const
{
    return speed_bonus;
}
float Creature::get_speed_mult() const
{
    return speed_mult;
}
float Creature::get_dodge_bonus() const
{
    return dodge_bonus;
}
int Creature::get_block_bonus() const
{
    return block_bonus; //base is 0
}
float Creature::get_hit_bonus() const
{
    return hit_bonus; //base is 0
}

void Creature::mod_stat( const std::string &stat, float modifier )
{
    if( stat == "speed" ) {
        mod_speed_bonus( modifier );
    } else if( stat == "dodge" ) {
        mod_dodge_bonus( modifier );
    } else if( stat == "block" ) {
        mod_block_bonus( modifier );
    } else if( stat == "hit" ) {
        mod_hit_bonus( modifier );
    } else if( stat == "pain" ) {
        mod_pain( modifier );
    } else if( stat == "moves" ) {
        mod_moves( modifier );
    } else {
        add_msg( "Tried to modify a nonexistent stat %s.", stat.c_str() );
    }
}

void Creature::set_num_blocks_bonus( int nblocks )
{
    num_blocks_bonus = nblocks;
}
void Creature::mod_num_dodges_bonus( int ndodges )
{
    num_dodges_bonus += ndodges;
}

void Creature::set_armor_bash_bonus( int nbasharm )
{
    armor_bash_bonus = nbasharm;
}
void Creature::set_armor_cut_bonus( int ncutarm )
{
    armor_cut_bonus = ncutarm;
}
void Creature::set_armor_bullet_bonus( int nbulletarm )
{
    armor_bullet_bonus = nbulletarm;
}

void Creature::set_speed_base( int nspeed )
{
    speed_base = nspeed;
}
void Creature::set_speed_bonus( int nspeed )
{
    speed_bonus = nspeed;
}
void Creature::set_speed_mult( float nspeed )
{
    speed_mult = nspeed;
}
void Creature::set_dodge_bonus( float ndodge )
{
    dodge_bonus = ndodge;
}

void Creature::set_block_bonus( int nblock )
{
    block_bonus = nblock;
}
void Creature::set_hit_bonus( float nhit )
{
    hit_bonus = nhit;
}
void Creature::mod_speed_bonus( int nspeed )
{
    speed_bonus += nspeed;
}
void Creature::mod_speed_mult( float nspeed )
{
    speed_mult += nspeed;
}
void Creature::mod_dodge_bonus( float ndodge )
{
    dodge_bonus += ndodge;
}
void Creature::mod_block_bonus( int nblock )
{
    block_bonus += nblock;
}
void Creature::mod_hit_bonus( float nhit )
{
    hit_bonus += nhit;
}

units::mass Creature::weight_capacity() const
{
    units::mass base_carry = 13_kilogram;
    switch( get_size() ) {
        case MS_TINY:
            base_carry /= 4;
            break;
        case MS_SMALL:
            base_carry /= 2;
            break;
        case MS_MEDIUM:
        default:
            break;
        case MS_LARGE:
            base_carry *= 2;
            break;
        case MS_HUGE:
            base_carry *= 4;
            break;
    }

    return base_carry;
}

/*
 * Drawing-related functions
 */
void Creature::draw( const catacurses::window &w, point origin, bool inverted ) const
{
    draw( w, tripoint( origin, posz() ), inverted );
}

void Creature::draw( const catacurses::window &w, const tripoint &origin, bool inverted ) const
{
    if( is_draw_tiles_mode() ) {
        return;
    }

    point draw( -origin.xy() + point( getmaxx( w ) / 2 + posx(), getmaxy( w ) / 2 + posy() ) );
    if( inverted ) {
        mvwputch_inv( w, draw, basic_symbol_color(), symbol() );
    } else if( is_symbol_highlighted() ) {
        mvwputch_hi( w, draw, basic_symbol_color(), symbol() );
    } else {
        mvwputch( w, draw, symbol_color(), symbol() );
    }
}

bool Creature::is_symbol_highlighted() const
{
    return false;
}

body_part Creature::select_body_part( Creature *source, int hit_roll ) const
{
    int szdif = source->get_size() - get_size();

    add_msg( m_debug, "hit roll = %d", hit_roll );
    add_msg( m_debug, "source size = %d", source->get_size() );
    add_msg( m_debug, "target size = %d", get_size() );
    add_msg( m_debug, "difference = %d", szdif );

    return human_anatomy->select_body_part( szdif, hit_roll )->token;
}

void Creature::check_dead_state()
{
    if( is_dead_state() ) {
        die( nullptr );
    }
}

std::string Creature::attitude_raw_string( Attitude att )
{
    switch( att ) {
        case Creature::A_HOSTILE:
            return "hostile";
        case Creature::A_NEUTRAL:
            return "neutral";
        case Creature::A_FRIENDLY:
            return "friendly";
        default:
            return "other";
    }
}

const std::pair<translation, nc_color> &Creature::get_attitude_ui_data( Attitude att )
{
    using pair_t = std::pair<translation, nc_color>;
    static const std::array<pair_t, 5> strings {
        {
            pair_t {to_translation( "Hostile" ), c_red},
            pair_t {to_translation( "Neutral" ), h_white},
            pair_t {to_translation( "Friendly" ), c_green},
            pair_t {to_translation( "Any" ), c_yellow},
            pair_t {to_translation( "BUG: Behavior unnamed.  (Creature::get_attitude_ui_data)" ), h_red}
        }
    };

    if( static_cast<int>( att ) < 0 || static_cast<int>( att ) >= static_cast<int>( strings.size() ) ) {
        return strings.back();
    }

    return strings[att];
}

std::string Creature::replace_with_npc_name( std::string input ) const
{
    return replace_all( std::move( input ), "<npcname>", disp_name() );
}

void Creature::knock_back_from( const tripoint &p )
{
    if( p == pos() ) {
        return; // No effect
    }
    if( is_hallucination() ) {
        die( nullptr );
        return;
    }
    tripoint to = pos();
    if( p.x < posx() ) {
        to.x++;
    }
    if( p.x > posx() ) {
        to.x--;
    }
    if( p.y < posy() ) {
        to.y++;
    }
    if( p.y > posy() ) {
        to.y--;
    }

    knock_back_to( to );
}

void Creature::add_msg_if_player( const translation &msg ) const
{
    return add_msg_if_player( msg.translated() );
}

void Creature::add_msg_if_player( const game_message_params &params, const translation &msg ) const
{
    return add_msg_if_player( params, msg.translated() );
}

void Creature::add_msg_if_npc( const translation &msg ) const
{
    return add_msg_if_npc( msg.translated() );
}

void Creature::add_msg_if_npc( const game_message_params &params, const translation &msg ) const
{
    return add_msg_if_npc( params, msg.translated() );
}

void Creature::add_msg_player_or_npc( const translation &pc, const translation &npc ) const
{
    return add_msg_player_or_npc( pc.translated(), npc.translated() );
}

void Creature::add_msg_player_or_npc( const game_message_params &params, const translation &pc,
                                      const translation &npc ) const
{
    return add_msg_player_or_npc( params, pc.translated(), npc.translated() );
}

void Creature::add_msg_player_or_say( const translation &pc, const translation &npc ) const
{
    return add_msg_player_or_say( pc.translated(), npc.translated() );
}

void Creature::add_msg_player_or_say( const game_message_params &params, const translation &pc,
                                      const translation &npc ) const
{
    return add_msg_player_or_say( params, pc.translated(), npc.translated() );
}

static std::vector<int> default_dispersion_for_ecogh = { {
        1731, 859, 573, 421, 341, 286, 245, 214, 191, 175,
        151, 143, 129, 118, 114, 107, 101, 94, 90, 78,
        78, 78, 74, 71, 68, 66, 62, 61, 59, 57,
        46, 46, 46, 46, 46, 46, 45, 45, 44, 42,
        41, 41, 39, 39, 38, 37, 36, 35, 34, 34,
        33, 33, 32, 30, 30, 30, 30, 29, 28
    }
};
std::vector<int> Creature::dispersion_for_even_chance_of_good_hit = default_dispersion_for_ecogh;

void Creature::load_hit_range( const JsonObject &jo )
{
    if( jo.has_array( "even_good" ) ) {
        jo.read( "even_good", dispersion_for_even_chance_of_good_hit );
    }
}

void Creature::reset_hit_range()
{
    dispersion_for_even_chance_of_good_hit = default_dispersion_for_ecogh;
}

void Creature::describe_infrared( std::vector<std::string> &buf ) const
{
    std::string size_str;
    switch( get_size() ) {
        case m_size::MS_TINY:
            size_str = pgettext( "infrared size", "tiny" );
            break;
        case m_size::MS_SMALL:
            size_str = pgettext( "infrared size", "small" );
            break;
        case m_size::MS_MEDIUM:
            size_str = pgettext( "infrared size", "medium" );
            break;
        case m_size::MS_LARGE:
            size_str = pgettext( "infrared size", "large" );
            break;
        case m_size::MS_HUGE:
            size_str = pgettext( "infrared size", "huge" );
            break;
        default:
            debugmsg( "Creature has invalid size class." );
            size_str = "invalid";
            break;
    }
    buf.push_back( _( "You see a figure radiating heat." ) );
    buf.push_back( string_format( _( "It is %s in size." ), size_str ) );
}

void Creature::describe_specials( std::vector<std::string> &buf ) const
{
    buf.push_back( _( "You sense a creature here." ) );
}

effects_map Creature::get_all_effects() const
{
    effects_map effects_without_removed;
    for( auto &outer : *effects ) {
        for( auto &inner : outer.second ) {
            if( !inner.second.is_removed() ) {
                effects_without_removed[outer.first][inner.first] = inner.second;
            }
        }
    }
    return effects_without_removed;
}
