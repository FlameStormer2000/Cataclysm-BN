#pragma once
#ifndef CATA_SRC_OVERMAP_H
#define CATA_SRC_OVERMAP_H

#include <algorithm>
#include <array>
#include <climits>
#include <cstdlib>
#include <functional>
#include <iosfwd>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "coordinates.h"
#include "enums.h"
#include "enum_conversions.h"
#include "game_constants.h"
#include "memory_fast.h"
#include "mongroup.h"
#include "omdata.h"
#include "overmap_types.h" // IWYU pragma: keep
#include "pimpl.h"
#include "point.h"
#include "string_id.h"
#include "type_id.h"

class JsonIn;
class JsonObject;
class JsonOut;
class basecamp;
class character_id;
class map_extra;
class monster;
class npc;
class overmap_connection;
class overmap_special;
class overmap_special_batch;
struct om_special_sectors;
struct regional_settings;
template <typename E> struct enum_traits;

namespace pf
{
template<typename Point>
struct directed_path;
} // namespace pf

struct city {
    // location of the city (in overmap terrain coordinates)
    point_om_omt pos;
    int size;
    std::string name;
    city( const point_om_omt &P = point_om_omt(), int S = -1 );

    operator bool() const {
        return size >= 0;
    }

    int get_distance_from( const tripoint_om_omt &p ) const;
};

enum class lab_type : int {
    standard = 0,
    ice,
    central,
    invalid
};

namespace io
{

template<>
std::string enum_to_string<lab_type>( lab_type data );

} // namespace io

template<>
struct enum_traits<lab_type> {
    static constexpr auto last = lab_type::invalid;
};

struct lab {
    lab_type type;
    std::set<tripoint_om_omt> tiles;
    std::set<tripoint_om_omt> finales;
};

struct om_note {
    std::string text;
    point_om_omt p;
    bool dangerous = false;
    int danger_radius = 0;
};

struct om_map_extra {
    string_id<map_extra> id;
    point_om_omt p;
};

struct om_vehicle {
    point_om_omt p; // overmap coordinates of tracked vehicle
    std::string name;
};

enum class radio_type : int {
    MESSAGE_BROADCAST,
    WEATHER_RADIO
};

extern std::map<enum radio_type, std::string> radio_type_names;

static constexpr int RADIO_MIN_STRENGTH = 80;
static constexpr int RADIO_MAX_STRENGTH = 200;

struct radio_tower {
    // local (to the containing overmap) submap coordinates
    point_om_sm pos;
    int strength;
    radio_type type;
    std::string message;
    int frequency;
    radio_tower( const point_om_sm &p, int S = -1, const std::string &M = "",
                 radio_type T = radio_type::MESSAGE_BROADCAST );
};

struct map_layer {
    oter_id terrain[OMAPX][OMAPY];
    bool visible[OMAPX][OMAPY];
    bool explored[OMAPX][OMAPY];
    std::vector<om_note> notes;
    std::vector<om_map_extra> extras;
};

static const std::map<std::string, oter_flags> oter_flags_map = {
    { "KNOWN_DOWN", known_down },
    { "KNOWN_UP", known_up },
    { "RIVER", river_tile },
    { "SIDEWALK", has_sidewalk },
    { "NO_ROTATE", no_rotate },
    { "LINEAR", line_drawing },
    { "SUBWAY", subway_connection },
    { "LAKE", lake },
    { "LAKE_SHORE", lake_shore },
    { "GENERIC_LOOT", generic_loot },
    { "RISK_HIGH", risk_high },
    { "RISK_LOW", risk_low },
    { "SOURCE_AMMO", source_ammo },
    { "SOURCE_ANIMALS", source_animals },
    { "SOURCE_BOOKS", source_books },
    { "SOURCE_CHEMISTRY", source_chemistry },
    { "SOURCE_CLOTHING", source_clothing },
    { "SOURCE_CONSTRUCTION", source_construction },
    { "SOURCE_COOKING", source_cooking },
    { "SOURCE_DRINK", source_drink },
    { "SOURCE_ELECTRONICS", source_electronics },
    { "SOURCE_FABRICATION", source_fabrication },
    { "SOURCE_FARMING", source_farming },
    { "SOURCE_FOOD", source_food },
    { "SOURCE_FORAGE", source_forage },
    { "SOURCE_FUEL", source_fuel },
    { "SOURCE_GUN", source_gun },
    { "SOURCE_LUXURY", source_luxury },
    { "SOURCE_MEDICINE", source_medicine },
    { "SOURCE_PEOPLE", source_people },
    { "SOURCE_SAFETY", source_safety },
    { "SOURCE_TAILORING", source_tailoring },
    { "SOURCE_VEHICLES", source_vehicles },
    { "SOURCE_WEAPON", source_weapon }
};

class overmap
{
    public:
        overmap( const overmap & );
        overmap( overmap && );
        overmap( const point_abs_om &p );
        ~overmap();

        overmap &operator=( const overmap & );

        /**
         * Create content in the overmap.
         **/
        void populate( overmap_special_batch &enabled_specials );
        void populate();

        const point_abs_om &pos() const {
            return loc;
        }

        void save() const;

        /**
         * @return The (local) overmap terrain coordinates of a randomly
         * chosen place on the overmap with the specific overmap terrain.
         * Returns @ref invalid_tripoint if no suitable place has been found.
         */
        tripoint_om_omt find_random_omt( const std::pair<std::string, ot_match_type> &target ) const;
        tripoint_om_omt find_random_omt( const std::string &omt_base_type,
                                         ot_match_type match_type = ot_match_type::type ) const {
            return find_random_omt( std::make_pair( omt_base_type, match_type ) );
        };
        /**
         * Return a vector containing the absolute coordinates of
         * every matching terrain on the current z level of the current overmap.
         * @returns A vector of terrain coordinates (absolute overmap terrain
         * coordinates), or empty vector if no matching terrain is found.
         */
        std::vector<point_abs_omt> find_terrain( const std::string &term, int zlevel );

        void ter_set( const tripoint_om_omt &p, const oter_id &id );
        const oter_id &ter( const tripoint_om_omt &p ) const;
        bool &seen( const tripoint_om_omt &p );
        bool seen( const tripoint_om_omt &p ) const;
        bool &explored( const tripoint_om_omt &p );
        bool is_explored( const tripoint_om_omt &p ) const;

        bool has_note( const tripoint_om_omt &p ) const;
        std::optional<int> has_note_with_danger_radius( const tripoint_om_omt &p ) const;
        bool is_marked_dangerous( const tripoint_om_omt &p ) const;
        const std::vector<om_note> &all_notes( int z ) const;
        const std::string &note( const tripoint_om_omt &p ) const;
        void add_note( const tripoint_om_omt &p, std::string message );
        void delete_note( const tripoint_om_omt &p );
        void mark_note_dangerous( const tripoint_om_omt &p, int radius, bool is_dangerous );

        bool has_extra( const tripoint_om_omt &p ) const;
        const string_id<map_extra> &extra( const tripoint_om_omt &p ) const;
        void add_extra( const tripoint_om_omt &p, const string_id<map_extra> &id );
        void delete_extra( const tripoint_om_omt &p );

        /**
         * Getter for overmap scents.
         * @returns a reference to a scent_trace from the requested location.
         */
        const scent_trace &scent_at( const tripoint_abs_omt &loc ) const;
        /**
         * Setter for overmap scents, stores the provided scent at the provided location.
         */
        void set_scent( const tripoint_abs_omt &loc, const scent_trace &new_scent );

        /**
         * @returns Whether @param p is within desired bounds of the overmap
         * @param clearance Minimal distance from the edges of the overmap
         */
        static bool inbounds( const tripoint_om_omt &p, int clearance = 0 );
        static bool inbounds( const point_om_omt &p, int clearance = 0 ) {
            return inbounds( tripoint_om_omt( p, 0 ), clearance );
        }
        /**
         * Dummy value, used to indicate that a point returned by a function is invalid.
         */
        static constexpr tripoint_abs_omt invalid_tripoint{ tripoint_min };
        /**
         * Return a vector containing the absolute coordinates of
         * every matching note on the current z level of the current overmap.
         * @returns A vector of note coordinates (absolute overmap terrain
         * coordinates), or empty vector if no matching notes are found.
         */
        std::vector<point_abs_omt> find_notes( int z, const std::string &text );
        /**
         * Return a vector containing the absolute coordinates of
         * every matching map extra on the current z level of the current overmap.
         * @returns A vector of map extra coordinates (absolute overmap terrain
         * coordinates), or empty vector if no matching map extras are found.
         */
        std::vector<point_abs_omt> find_extras( int z, const std::string &text );

        /**
         * Returns whether or not the location has been generated (e.g. mapgen has run).
         * @param loc Location to check.
         * @returns True if param @loc has been generated.
         */
        bool is_omt_generated( const tripoint_om_omt &loc ) const;

        /** Returns the (0, 0) corner of the overmap in the global coordinates. */
        point_abs_omt global_base_point() const;

        // TODO: Should depend on coordinates
        const regional_settings &get_settings() const {
            return *settings;
        }

        void clear_mon_groups();
        void clear_overmap_special_placements();
        void clear_cities();
        void clear_labs();
        void clear_connections_out();
        void place_special_forced( const overmap_special_id &special_id, const tripoint_om_omt &p,
                                   om_direction::type dir );
    private:
        std::multimap<tripoint_om_sm, mongroup> zg;
    public:
        /** Unit test enablers to check if a given mongroup is present. */
        bool mongroup_check( const mongroup &candidate ) const;
        bool monster_check( const std::pair<tripoint_om_sm, monster> &candidate ) const;

    private:
        /** Mapping of overmap coordinate to bits representing NESW+up+down connectivity. */
        std::map<tripoint_om_omt, std::bitset<six_cardinal_directions.size()>> electric_grid_connections;
    public:

        void set_electric_grid_connections( const tripoint_om_omt &p,
                                            const std::bitset<six_cardinal_directions.size()> &connections );

        // TODO: make private
        std::vector<radio_tower> radios;
        std::map<int, om_vehicle> vehicles;
        std::vector<basecamp> camps;
        std::vector<city> cities;
        std::vector<lab> labs;
        std::map<overmap_connection_id, std::vector<tripoint_om_omt>> connections_out;
        std::optional<basecamp *> find_camp( const point_abs_omt &p );
        /// Adds the npc to the contained list of npcs ( @ref npcs ).
        void insert_npc( shared_ptr_fast<npc> who );
        /// Removes the npc and returns it ( or returns nullptr if not found ).
        shared_ptr_fast<npc> erase_npc( const character_id &id );

        void for_each_npc( const std::function<void( npc & )> &callback );
        void for_each_npc( const std::function<void( const npc & )> &callback ) const;

        shared_ptr_fast<npc> find_npc( const character_id &id ) const;

        const std::vector<shared_ptr_fast<npc>> &get_npcs() const {
            return npcs;
        }
        std::vector<shared_ptr_fast<npc>> get_npcs( const std::function<bool( const npc & )>
                                       &predicate )
                                       const;

    private:
        friend class overmapbuffer;

        std::vector<shared_ptr_fast<npc>> npcs;

        bool nullbool = false;
        point_abs_om loc;

        std::array<map_layer, OVERMAP_LAYERS> layer;
        std::unordered_map<tripoint_abs_omt, scent_trace> scents;

        // Records the locations where a given overmap special was placed, which
        // can be used after placement to lookup whether a given location was created
        // as part of a special.
        // TODO: Should have individual instances grouped by placement (ie. 2 adjacent houses aren't one house)
        std::unordered_map<tripoint_om_omt, overmap_special_id> overmap_special_placements;

        pimpl<regional_settings> settings;

        oter_id get_default_terrain( int z ) const;

        // Initialize
        void init_layers();
        // open existing overmap, or generate a new one
        void open( overmap_special_batch &enabled_specials );
    public:

        /**
         * When monsters despawn during map-shifting they will be added here.
         * map::spawn_monsters will load them and place them into the reality bubble
         * (adding it to the creature tracker and putting it onto the map).
         * This stores each submap worth of monsters in a different bucket of the multimap.
         */
        pimpl<std::unordered_multimap<tripoint_om_sm, monster>> monster_map;

        // parse data in an opened overmap file
        void unserialize( std::istream &fin, const std::string &file_path );
        // Parse per-player overmap view data.
        void unserialize_view( std::istream &fin, const std::string &file_path );
        // Save data in an opened overmap file
        void serialize( std::ostream &fout ) const;
        // Save per-player overmap view data.
        void serialize_view( std::ostream &fout ) const;
    private:
        void generate( const overmap *north, const overmap *east,
                       const overmap *south, const overmap *west,
                       overmap_special_batch &enabled_specials );
        bool generate_sub( int z );
        bool generate_over( int z );

        const city &get_nearest_city( const tripoint_om_omt &p ) const;

        void signal_hordes( const tripoint_rel_sm &p, int sig_power );
        void process_mongroups();
        void move_hordes();

        static bool is_obsolete_terrain( const std::string &ter );
        void convert_terrain( const std::unordered_map<tripoint_om_omt, std::string> &needs_conversion );

        // Overall terrain
        void place_river( point_om_omt pa, point_om_omt pb );
        void place_forests();
        void place_lakes();
        void place_rivers( const overmap *north, const overmap *east, const overmap *south,
                           const overmap *west );
        void place_swamps();
        void place_forest_trails();
        void place_forest_trailheads();

        void place_roads( const overmap *north, const overmap *east, const overmap *south,
                          const overmap *west );

        void populate_connections_out_from_neighbors( const overmap *north, const overmap *east,
                const overmap *south, const overmap *west );

        // City Building
        overmap_special_id pick_random_building_to_place( int town_dist ) const;

        void place_cities();
        void place_building( const tripoint_om_omt &p, om_direction::type dir, const city &town );

        void build_city_street( const overmap_connection &connection, const point_om_omt &p, int cs,
                                om_direction::type dir, const city &town, int block_width = 2 );
        bool build_lab( const tripoint_om_omt &p, lab &l, int size,
                        std::vector<point_om_omt> &lab_train_points,
                        const std::string &prefix, int train_odds );
        void build_anthill( const tripoint_om_omt &p, int s );
        void build_tunnel( const tripoint_om_omt &p, int s, om_direction::type dir );
        bool build_slimepit( const tripoint_om_omt &origin, int s );
        void build_mine( const tripoint_om_omt &origin, int s );

        // Connection laying
        pf::directed_path<point_om_omt> lay_out_connection(
            const overmap_connection &connection, const point_om_omt &source,
            const point_om_omt &dest, int z, bool must_be_unexplored ) const;
        pf::directed_path<point_om_omt> lay_out_street(
            const overmap_connection &connection, const point_om_omt &source,
            om_direction::type dir, size_t len ) const;

        void build_connection(
            const overmap_connection &connection, const pf::directed_path<point_om_omt> &path, int z,
            const om_direction::type &initial_dir = om_direction::type::invalid );
        void build_connection( const point_om_omt &source, const point_om_omt &dest, int z,
                               const overmap_connection &connection, bool must_be_unexplored,
                               const om_direction::type &initial_dir = om_direction::type::invalid );
        void connect_closest_points( const std::vector<point_om_omt> &points, int z,
                                     const overmap_connection &connection );
        // Polishing
        bool check_ot( const std::string &otype, ot_match_type match_type,
                       const tripoint_om_omt &p ) const;
        bool check_overmap_special_type( const overmap_special_id &id,
                                         const tripoint_om_omt &location ) const;
        void chip_rock( const tripoint_om_omt &p );

        void polish_river();
        void good_river( const tripoint_om_omt &p );

        om_direction::type random_special_rotation( const overmap_special &special,
                const tripoint_om_omt &p, bool must_be_unexplored ) const;

        bool can_place_special( const overmap_special &special, const tripoint_om_omt &p,
                                om_direction::type dir, bool must_be_unexplored ) const;

        void place_special(
            const overmap_special &special, const tripoint_om_omt &p, om_direction::type dir,
            const city &cit, bool must_be_unexplored, bool force );
        /**
         * Iterate over the overmap and place the quota of specials.
         * If the stated minimums are not reached, it will spawn a new nearby overmap
         * and continue placing specials there.
         * @param enabled_specials specifies what specials to place, and tracks how many have been placed.
         **/
        void place_specials( overmap_special_batch &enabled_specials );
        /**
         * Walk over the overmap and attempt to place specials.
         * @param enabled_specials vector of objects that track specials being placed.
         * @param sectors sectors in which to attempt placement.
         * @param place_optional restricts attempting to place specials that have met their minimum count in the first pass.
         */
        void place_specials_pass( overmap_special_batch &enabled_specials,
                                  om_special_sectors &sectors, bool place_optional, bool must_be_unexplored );

        /**
         * Attempts to place specials within a sector.
         * @param enabled_specials vector of objects that track specials being placed.
         * @param sector sector identifies the location where specials are being placed.
         * @param place_optional restricts attempting to place specials that have met their minimum count in the first pass.
         */
        bool place_special_attempt(
            overmap_special_batch &enabled_specials, const point_om_omt &sector, int sector_width,
            bool place_optional, bool must_be_unexplored );

        void place_mongroups();
        void place_radios();

        void add_mon_group( const mongroup &group );

        void load_monster_groups( JsonIn &jsin );
        void load_legacy_monstergroups( JsonIn &jsin );
        void save_monster_groups( JsonOut &jo ) const;
    public:
        static void load_obsolete_terrains( const JsonObject &jo );
        static void reset_obsolete_terrains();
};

bool is_river( const oter_id &ter );
bool is_river_or_lake( const oter_id &ter );

/**
* Determine if the provided name is a match with the provided overmap terrain
* based on the specified match type.
* @param name is the name we're looking for.
* @param oter is the overmap terrain id we're comparing our name with.
* @param match_type is the matching rule to use when comparing the two values.
*/
bool is_ot_match( const std::string &name, const oter_id &oter,
                  ot_match_type match_type );

/**
* Returns the string of oter without any directional suffix
*/
std::string oter_no_dir( const oter_id &oter );

/**
 * Returns oter rotation direction value.
 */
om_direction::type oter_get_rotation_dir( const oter_id &oter );

/**
* Returns number of clockwise rotations 0, 1, 2, 3 respectively
* if the suffix is _north, _east, _south, _west.
* Returns 0 if there's no suffix.
*/
int oter_get_rotations( const oter_id &oter );

/**
* Returns the directional suffix or "" if there isn't one.
*
* Returned reference is kept alive during the whole program execution.
*/
const std::string &oter_get_rotation_string( const oter_id &oter );

/**
 * Determine whether provided tile belongs to overmap connection.
 */
bool belongs_to_connection( const overmap_connection_id &id, const oter_id &oter );
#endif // CATA_SRC_OVERMAP_H
