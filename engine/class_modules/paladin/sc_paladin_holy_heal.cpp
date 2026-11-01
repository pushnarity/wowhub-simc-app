// wowhub: Holy Paladin healing for healing sims (role=heal with healing targets).
// Upstream SimC refuses Holy Paladin; everything here is ours and is only created in healing sims, so the
// weekly engine update rebases cleanly. Mechanics were checked against Warcraft Logs events (Midnight,
// 12.1.5): beacon transfers take the raw heal (overhealing included), Beacon of Virtue + Beacon of the
// Savior together transfer about half of the raw direct healing in top logs.
#include "sc_paladin.hpp"
#include "simulationcraft.hpp"

namespace paladin
{
struct hpal_state_t
{
  paladin_t* p;
  // talents
  player_talent_t reclamation, light_of_the_martyr, bestow_light, glorious_dawn, overflowing_light, imbued_infusions,
      inflorescence, divine_revelations, moment_of_compassion, commanding_light, beacon_of_virtue, pillars_of_light,
      glistening_radiance, savior, savior_2, savior_3, selfless_healer, truth_prevails, afterimage, rising_sunlight,
      seek_deliverance, light_of_dawn, light_s_conviction, infusion_of_light, extrication;
  std::map<player_t*, buff_t*> bov;  // Beacon of Virtue on each ally
  player_t* savior_target = nullptr;
  double bov_pct = 0, savior_pct = 0;
  double proximity = 0.85;  // Lightbringer: average share of the full mastery bonus (distance to the target)
  int afterimage_spent = 0;
  buff_t* lotm      = nullptr;
  buff_t* bestow    = nullptr;
  buff_t* afterimage_buff = nullptr;
  action_t* beacon_heal       = nullptr;
  action_t* savior_heal       = nullptr;
  action_t* savior_hot        = nullptr;
  action_t* savior_shield     = nullptr;
  action_t* glisten           = nullptr;
  action_t* pillar            = nullptr;
  action_t* overflowing       = nullptr;
  action_t* sun_sear          = nullptr;
  action_t* dawnlight         = nullptr;
  action_t* dawnlight_radiate = nullptr;
  action_t* suns_avatar       = nullptr;
  action_t* hs_echo           = nullptr;  // Second Sunrise / Divine Toll Holy Shocks
  action_t* lod_echo          = nullptr;
  action_t* ef_echo           = nullptr;  // Afterimage
  action_t* truth_heal        = nullptr;
  action_t* truth_spill       = nullptr;
  action_t* selfless_heal     = nullptr;

  explicit hpal_state_t( paladin_t* pl ) : p( pl ) {}

  bool on_beacon( player_t* t ) const
  {
    auto it = bov.find( t );
    return ( it != bov.end() && it->second->check() ) || t == savior_target;
  }

  buff_t* bov_buff( player_t* t )
  {
    auto& b = bov[ t ];
    if ( !b )
      b = make_buff( actor_pair_t( t, p ), "beacon_of_virtue", p->find_spell( 200025 ) )->set_cooldown( 0_ms );
    return b;
  }

  // injured allies, most injured first
  std::vector<player_t*> injured( size_t n, player_t* skip = nullptr ) const
  {
    std::vector<player_t*> l;
    for ( auto* t : p->sim->healing_no_pet_list )
      if ( !t->is_sleeping() && t != skip && t->resources.pct( RESOURCE_HEALTH ) < 1.0 &&
           std::find( l.begin(), l.end(), t ) == l.end() )
        l.push_back( t );
    std::sort( l.begin(), l.end(), []( player_t* a, player_t* b ) {
      return a->resources.pct( RESOURCE_HEALTH ) < b->resources.pct( RESOURCE_HEALTH );
    } );
    if ( l.size() > n )
      l.resize( n );
    return l;
  }

  player_t* random_ally( player_t* skip = nullptr ) const
  {
    std::vector<player_t*> l;
    for ( auto* t : p->sim->healing_no_pet_list )
      if ( !t->is_sleeping() && t != skip )
        l.push_back( t );
    return l.empty() ? nullptr : l[ p->rng().range( l.size() ) ];
  }
};

namespace hpal
{
// A heal that only carries an amount (beacon transfers, Overflowing Light, Piety-like spills): no multipliers.
struct flat_heal_t : public heal_t
{
  flat_heal_t( paladin_t* p, util::string_view n, unsigned id ) : heal_t( n, p, p->find_spell( id ) )
  {
    background = proc = true;
    may_crit = callbacks = false;
    harmful     = false;
    auto_target = false;
    target      = p;
  }
  void init() override
  {
    heal_t::init();
    snapshot_flags = update_flags = 0;
  }
};

struct flat_absorb_t : public absorb_t
{
  double cap_pct;  // share of the target's maximum health the shield may grow to (0 = no cap, stacks add)
  flat_absorb_t( paladin_t* p, util::string_view n, unsigned id, double cap = 0 )
    : absorb_t( n, p, p->find_spell( id ) ), cap_pct( cap )
  {
    background = proc = true;
    may_crit = callbacks = false;
    harmful = false;
    target  = p;
  }
  void init() override
  {
    absorb_t::init();
    snapshot_flags = update_flags = 0;
  }
  void assess_damage( result_amount_type t, action_state_t* s ) override
  {
    if ( target_specific[ s->target ] == nullptr )
      target_specific[ s->target ] = create_buff( s );
    auto* b = target_specific[ s->target ];
    double v = b->check_value() + s->result_amount;
    if ( cap_pct > 0 )
      v = std::min( v, s->target->max_health() * cap_pct );
    s->result_amount = std::max( 0.0, v - b->check_value() );
    b->trigger( 1, v );
    stats->add_result( 0.0, s->result_amount, result_amount_type::ABSORB, s->result, s->block_result, s->target );
    (void)t;
  }
};

// Every Holy heal: Lightbringer mastery, Rising Sunlight, Beacon of the Savior's bonus, and beacon transfers.
struct hpal_heal_t : public paladin_heal_t
{
  bool beacons;
  double effectiveness;

  hpal_heal_t( util::string_view n, paladin_t* p, const spell_data_t* s )
    : paladin_heal_t( n, p, s ), beacons( true ), effectiveness( 1.0 )
  {
  }

  hpal_state_t& st() const
  {
    return *p()->hpal;
  }

  double heal_mult( const action_state_t* s ) const
  {
    double m = effectiveness;
    m *= 1.0 + p()->cache.mastery_value() * st().proximity;
    if ( st().savior_3.ok() && s->target == st().savior_target )
      m *= 1.0 + st().savior_3->effectN( 2 ).percent();
    if ( st().rising_sunlight.ok() )
    {
      double sum = 0;
      int n      = 0;
      for ( auto& [ t, b ] : st().bov )
        if ( b->check() )
        {
          sum += t->resources.pct( RESOURCE_HEALTH );
          n++;
        }
      if ( st().savior_target )
      {
        sum += st().savior_target->resources.pct( RESOURCE_HEALTH );
        n++;
      }
      if ( n )
        m *= 1.0 + st().rising_sunlight->effectN( 2 ).percent() * sum / n;
    }
    return m;
  }

  double composite_da_multiplier( const action_state_t* s ) const override
  {
    return paladin_heal_t::composite_da_multiplier( s ) * heal_mult( s );
  }
  double composite_ta_multiplier( const action_state_t* s ) const override
  {
    return paladin_heal_t::composite_ta_multiplier( s ) * heal_mult( s );
  }

  void transfer( action_state_t* s )
  {
    if ( !beacons || s->result_total <= 0 )
      return;
    double raw = s->result_total;
    for ( auto& [ t, b ] : st().bov )
      if ( b->check() && t != s->target && !t->is_sleeping() )
        st().beacon_heal->execute_on_target( t, raw * st().bov_pct );
    if ( st().savior_target && st().savior_target != s->target && !st().savior_target->is_sleeping() )
      st().savior_heal->execute_on_target( st().savior_target, raw * st().savior_pct );
  }

  void impact( action_state_t* s ) override
  {
    heal_t::impact( s );  // not paladin_heal_t: its Beacon of Light needs a fixed beacon target
    transfer( s );
  }

  // HoT ticks do not reach the beacons (the log's transfer count only fits direct heals)
};

// Holy Shock heal ===========================================================
struct holy_shock_heal_t : public hpal_heal_t
{
  holy_shock_heal_t( paladin_t* p, util::string_view n, double eff ) : hpal_heal_t( n, p, p->find_spell( 25914 ) )
  {
    background = true;
    effectiveness = eff;
    trigger_gcd   = 0_ms;
    // Divine Glimpse and Luminosity come with the passive data; the spec's +10% does not
    base_crit += p->find_spell( 272906 )->effectN( 1 ).percent();
  }

  double composite_da_multiplier( const action_state_t* s ) const override
  {
    double m = hpal_heal_t::composite_da_multiplier( s );
    if ( st().reclamation.ok() )
      m *= 1.0 + st().reclamation->effectN( 2 ).percent() * ( 1.0 - s->target->resources.pct( RESOURCE_HEALTH ) );
    if ( st().lotm && st().lotm->check() )
      m *= 1.0 + st().lotm->data().effectN( 1 ).percent();
    if ( st().bestow && st().bestow->check() )
      m *= 1.0 + st().bestow->check() * st().bestow->data().effectN( 1 ).percent();
    return m;
  }

  void impact( action_state_t* s ) override
  {
    hpal_heal_t::impact( s );

    double over = s->result_total - s->result_amount;
    if ( st().overflowing && over > 0 )
    {
      // the shield may not exceed 10% of the paladin's maximum health
      double cap  = player->max_health() * st().overflowing_light->effectN( 2 ).percent();
      buff_t* old = buff_t::find( s->target, "overflowing_light", player );
      double room = cap - ( old ? old->check_value() : 0.0 );
      if ( room > 0 )
        st().overflowing->execute_on_target( s->target, std::min( room, over * st().overflowing_light->effectN( 1 ).percent() ) );
    }

    if ( s->result == RESULT_CRIT && st().sun_sear )
      st().sun_sear->execute_on_target( s->target );

    // Infusion of Light rolls on every Holy Shock heal, Divine Toll's and the echo included (27 procs in a log
    // with 80 casts and 219 Holy Shock heals)
    if ( st().infusion_of_light.ok() && rng().roll( st().infusion_of_light->effectN( 1 ).percent() ) )
      p()->buffs.infusion_of_light->trigger();
  }
};

struct holy_shock_t : public paladin_spell_t
{
  holy_shock_heal_t* heal;
  holy_shock_t( paladin_t* p, util::string_view options_str )
    : paladin_spell_t( "holy_shock", p, p->find_spell( 20473 ) )
  {
    parse_options( options_str );
    harmful = false;
    heal    = new holy_shock_heal_t( p, "holy_shock_heal", 1.0 );
    add_child( heal );
    cooldown = p->cooldowns.holy_shock;  // Light's Conviction's extra charge comes with the passive data
  }

  void execute() override
  {
    auto& s     = *p()->hpal;
    auto inj    = s.injured( 1 );
    player_t* t = inj.empty() ? s.random_ally() : inj.front();
    paladin_spell_t::execute();
    if ( !t )
      return;
    heal->execute_on_target( t );

    if ( s.glorious_dawn.ok() && rng().roll( s.glorious_dawn->effectN( 1 ).percent() ) )
      cooldown->reset( false, 1 );
    // Second Sunrise: in logs every Holy Shock cast echoes ~0.3 s later on the same ally (not 15%)
    if ( s.hs_echo )
      make_event( *sim, 300_ms, [ &s, t ] { s.hs_echo->execute_on_target( t ); } );
  }
};

// Eternal Flame (Word of Glory) ==============================================
struct eternal_flame_t : public holy_power_consumer_t<hpal_heal_t>
{
  double self_bonus;
  eternal_flame_t( paladin_t* p, util::string_view n, util::string_view options_str, double eff = 1.0 )
    : holy_power_consumer_t( n, p, p->find_spell( 156322 ) ),
      self_bonus( p->find_spell( 156322 )->effectN( 3 ).percent() )
  {
    parse_options( options_str );
    is_wog        = true;
    effectiveness = eff;
    if ( eff < 1.0 )
    {
      background = proc = true;
      auto_target       = false;
    }
  }

  double composite_da_multiplier( const action_state_t* s ) const override
  {
    double m = holy_power_consumer_t::composite_da_multiplier( s );
    if ( s->target == player )
      m *= 1.0 + self_bonus;
    if ( p()->buffs.divine_purpose->check() )
      m *= 1.0 + p()->spells.divine_purpose_buff->effectN( 2 ).percent();
    return m;
  }

  double composite_target_crit_chance( player_t* t ) const override
  {
    double c = holy_power_consumer_t::composite_target_crit_chance( t );
    if ( p()->hpal->extrication.ok() )
      c += p()->hpal->extrication->effectN( 1 ).percent() * ( 1.0 - t->resources.pct( RESOURCE_HEALTH ) );
    return c;
  }

  void impact( action_state_t* s ) override
  {
    hpal_heal_t::impact( s );  // the consumer's own impact would put the damage Dawnlight on the ally
    auto& st = *p()->hpal;
    if ( !background && p()->buffs.herald_of_the_sun.dawnlight->up() && st.dawnlight )
    {
      st.dawnlight->execute_on_target( s->target );
      p()->buffs.herald_of_the_sun.dawnlight->decrement();
    }
  }

  void execute() override
  {
    holy_power_consumer_t::execute();
    auto& st = *p()->hpal;
    (void)st;
  }
};

// Light of Dawn ==============================================================
struct light_of_dawn_heal_t : public holy_power_consumer_t<hpal_heal_t>
{
  light_of_dawn_heal_t( paladin_t* p, util::string_view n, util::string_view options_str, double eff = 1.0 )
    : holy_power_consumer_t( n, p, p->find_spell( 85222 ) )
  {
    parse_options( options_str );
    aoe           = as<int>( data().effectN( 2 ).base_value() );
    effectiveness = eff;
    if ( eff < 1.0 )
    {
      background = proc = true;
      auto_target       = false;
    }
  }

  double composite_da_multiplier( const action_state_t* s ) const override
  {
    double m = holy_power_consumer_t::composite_da_multiplier( s );
    if ( p()->buffs.divine_purpose->check() )
      m *= 1.0 + p()->spells.divine_purpose_buff->effectN( 2 ).percent();
    return m;
  }

  double composite_target_crit_chance( player_t* t ) const override
  {
    double c = holy_power_consumer_t::composite_target_crit_chance( t );
    if ( p()->hpal->extrication.ok() )
      c += p()->hpal->extrication->effectN( 1 ).percent() * ( 1.0 - t->resources.pct( RESOURCE_HEALTH ) );
    return c;
  }

  void impact( action_state_t* s ) override
  {
    hpal_heal_t::impact( s );
    auto& st = *p()->hpal;
    if ( s->result == RESULT_CRIT && st.sun_sear )
      st.sun_sear->execute_on_target( s->target );
    if ( !background && s->chain_target == 0 && p()->buffs.herald_of_the_sun.dawnlight->up() && st.dawnlight )
    {
      st.dawnlight->execute_on_target( s->target );
      p()->buffs.herald_of_the_sun.dawnlight->decrement();
    }
  }

  void execute() override
  {
    holy_power_consumer_t::execute();
    if ( !background && p()->talents.herald_of_the_sun.second_sunrise->ok() &&
         rng().roll( p()->talents.herald_of_the_sun.second_sunrise->effectN( 1 ).percent() ) )
      p()->hpal->lod_echo->execute_on_target( execute_state->target );
  }
};

// Flash of Light =============================================================
struct flash_of_light_t : public hpal_heal_t
{
  flash_of_light_t( paladin_t* p, util::string_view options_str )
    : hpal_heal_t( "flash_of_light", p, p->find_class_spell( "Flash of Light" ) )
  {
    parse_options( options_str );
  }

  double composite_da_multiplier( const action_state_t* s ) const override
  {
    double m = hpal_heal_t::composite_da_multiplier( s );
    auto& st = *p()->hpal;
    if ( p()->buffs.infusion_of_light->check() )
    {
      // the buff's value already carries the tier 2-piece's +100% (log: 300% more healing, not 400%)
      m *= 1.0 + p()->buffs.infusion_of_light->data().effectN( 1 ).percent();
      if ( st.divine_revelations.ok() )
        m *= 1.0 + st.divine_revelations->effectN( 2 ).percent();
    }
    if ( st.moment_of_compassion.ok() && st.on_beacon( s->target ) )
      m *= 1.0 + st.moment_of_compassion->effectN( 1 ).percent();
    if ( st.selfless_healer.ok() && s->target != player )
      m *= 1.0 + st.selfless_healer->effectN( 1 ).percent();
    return m;
  }

  timespan_t execute_time() const override
  {
    if ( p()->buffs.infusion_of_light->check() )
      return 0_ms;
    return hpal_heal_t::execute_time();
  }

  void impact( action_state_t* s ) override
  {
    hpal_heal_t::impact( s );
    auto& st = *p()->hpal;
    if ( st.selfless_healer.ok() && s->target != player )
      st.selfless_heal->execute_on_target( player, s->result_total * st.selfless_healer->effectN( 2 ).percent() );
  }

  void execute() override
  {
    bool infused = p()->buffs.infusion_of_light->check();
    hpal_heal_t::execute();
    if ( infused )
    {
      p()->buffs.infusion_of_light->decrement();
      if ( p()->hpal->imbued_infusions.ok() )
        p()->cooldowns.holy_shock->adjust(
            timespan_t::from_millis( p()->hpal->imbued_infusions->effectN( 1 ).base_value() ) );
    }
  }
};

// Beacon of Virtue (+ Pillars of Light) ======================================
struct beacon_of_virtue_t : public paladin_spell_t
{
  beacon_of_virtue_t( paladin_t* p, util::string_view options_str )
    : paladin_spell_t( "beacon_of_virtue", p, p->find_spell( 200025 ) )
  {
    parse_options( options_str );
    harmful = false;
  }

  bool ready() override
  {
    return p()->hpal->beacon_of_virtue.ok() && paladin_spell_t::ready();
  }

  void execute() override
  {
    paladin_spell_t::execute();
    auto& st = *p()->hpal;
    size_t n = 1 + as<size_t>( data().effectN( 2 ).base_value() );
    auto l   = st.injured( n );
    for ( int tries = 0; l.size() < n && tries < 40; tries++ )  // cast on cooldown: top up with any allies
      if ( auto* t = st.random_ally(); t && std::find( l.begin(), l.end(), t ) == l.end() )
        l.push_back( t );
    for ( auto* t : l )
    {
      st.bov_buff( t )->trigger();
      if ( st.pillar )
        st.pillar->execute_on_target( t );
    }
  }
};

// Divine Toll: Holy Shock on 5 allies at reduced effectiveness, then Dawnlight and Aurora's Divine Purpose
struct divine_toll_heal_t : public paladin_spell_t
{
  holy_shock_heal_t* shock;
  divine_toll_heal_t( paladin_t* p, util::string_view options_str )
    : paladin_spell_t( "divine_toll", p, p->talents.divine_toll )
  {
    parse_options( options_str );
    harmful = false;
    shock   = new holy_shock_heal_t( p, "holy_shock_divine_toll", p->find_spell( 375609 )->effectN( 1 ).percent() );
    add_child( shock );
  }

  void execute() override
  {
    paladin_spell_t::execute();
    auto& st = *p()->hpal;
    auto l   = st.injured( as<size_t>( data().effectN( 1 ).base_value() ) );
    for ( auto* t : l )
    {
      shock->execute_on_target( t );
      p()->resource_gain( RESOURCE_HOLY_POWER, 1, p()->gains.hp_divine_toll );
    }
    if ( p()->talents.herald_of_the_sun.dawnlight->ok() )
      p()->buffs.herald_of_the_sun.dawnlight->trigger(
          as<int>( p()->talents.herald_of_the_sun.dawnlight->effectN( 1 ).base_value() ) );
    if ( p()->talents.herald_of_the_sun.aurora->ok() )
      p()->buffs.divine_purpose->trigger();
  }
};

// Dawnlight on an ally: a HoT that radiates part of each tick to nearby allies, and Sun's Avatar beams
struct dawnlight_heal_t : public hpal_heal_t
{
  dawnlight_heal_t( paladin_t* p ) : hpal_heal_t( "dawnlight_heal", p, p->find_spell( 431381 ) )
  {
    background = proc = true;
    auto_target       = false;
    dot_duration      = timespan_t::from_millis( p->talents.herald_of_the_sun.dawnlight->effectN( 2 ).base_value() );
    dot_behavior      = dot_behavior_e::DOT_EXTEND;
  }

  void tick( dot_t* d ) override
  {
    bool full = d->target->resources.pct( RESOURCE_HEALTH ) >= 1.0;
    hpal_heal_t::tick( d );
    auto& st = *p()->hpal;
    double radiate = d->state->result_total * p()->spells.herald_of_the_sun.dawnlight_aoe_metadata->effectN( 1 ).percent();
    for ( auto* t : st.injured( as<size_t>( p()->spells.herald_of_the_sun.dawnlight_aoe_metadata->effectN( 2 ).base_value() ),
                                d->target ) )
      st.dawnlight_radiate->execute_on_target( t, radiate );
    if ( full && p()->talents.herald_of_the_sun.endless_gleam->ok() )
      d->adjust_duration( timespan_t::from_millis( p()->talents.herald_of_the_sun.endless_gleam->effectN( 1 ).base_value() ) );
    // Sun's Avatar: allies crossing the beams between the paladin and each Dawnlight (about 6 a tick in logs)
    if ( st.suns_avatar )
      for ( int i = 0; i < 6; i++ )
        if ( auto* t = st.random_ally() )
          st.suns_avatar->execute_on_target( t );
  }
};

struct simple_heal_t : public hpal_heal_t
{
  simple_heal_t( paladin_t* p, util::string_view n, unsigned id, bool beacon = true ) : hpal_heal_t( n, p, p->find_spell( id ) )
  {
    background = proc = true;
    auto_target       = false;
    beacons           = beacon;
  }
};

struct sun_sear_t : public hpal_heal_t
{
  sun_sear_t( paladin_t* p ) : hpal_heal_t( "sun_sear", p, p->find_spell( 431415 ) )
  {
    background = proc = true;
    beacons           = false;
    auto_target       = false;
    dot_behavior      = dot_behavior_e::DOT_EXTEND;
  }
};

// Judgment (upstream damage) plus Truth Prevails' self-heal, half of whose overhealing spills onto 2 allies
struct judgment_truth_t : public paladin_spell_t
{
  action_t* dmg;
  judgment_truth_t( paladin_t* p, util::string_view options_str )
    : paladin_spell_t( "judgment_truth_prevails", p, p->find_spell( 275773 ) ),
      dmg( p->create_action_holy( "judgment", "" ) )
  {
    parse_options( options_str );
    harmful = false;
    may_miss = may_crit = false;
    base_dd_min = base_dd_max = 0;
    spell_power_mod.direct    = 0;
    dmg->background           = true;
    dmg->cooldown             = p->get_cooldown( "judgment_damage" );
    add_child( dmg );
  }
  void execute() override
  {
    paladin_spell_t::execute();
    dmg->execute_on_target( dmg->target );
    if ( p()->sets->has_set_bonus( PALADIN_HOLY, MID2, B4 ) &&
         rng().roll( p()->sets->set( PALADIN_HOLY, MID2, B4 )->effectN( 1 ).percent() ) )
      p()->buffs.infusion_of_light->trigger();
    if ( p()->hpal->truth_prevails.ok() )
      p()->hpal->truth_heal->execute_on_target( player );
  }
};

struct truth_prevails_t : public hpal_heal_t
{
  truth_prevails_t( paladin_t* p ) : hpal_heal_t( "truth_prevails", p, p->find_spell( 461546 ) )
  {
    background = proc = true;
    auto_target       = false;
    beacons           = false;
    target            = p;
  }
  void impact( action_state_t* s ) override
  {
    hpal_heal_t::impact( s );
    auto& st    = *p()->hpal;
    double over = s->result_total - s->result_amount;
    if ( over > 0 )
    {
      auto l = st.injured( as<size_t>( st.truth_prevails->effectN( 3 ).base_value() ), player );
      for ( auto* t : l )
        st.truth_spill->execute_on_target( t, over * st.truth_prevails->effectN( 2 ).percent() / l.size() );
    }
  }
};
}  // namespace hpal

// ==========================================================================
// Hooks
// ==========================================================================
bool paladin_t::holy_heal_mode() const
{
  return specialization() == PALADIN_HOLY && primary_role() == ROLE_HEAL && sim->heal_target;
}

void paladin_t::init_holy_heal()
{
  hpal   = new hpal_state_t( this );
  auto& s = *hpal;
  auto ST = [ this ]( std::string_view n ) { return find_talent_spell( talent_tree::SPECIALIZATION, n ); };
  auto CT = [ this ]( std::string_view n ) { return find_talent_spell( talent_tree::CLASS, n ); };
  s.reclamation          = ST( "Reclamation" );
  s.light_of_the_martyr  = ST( "Light of the Martyr" );
  s.bestow_light         = ST( "Bestow Light" );
  s.glorious_dawn        = ST( "Glorious Dawn" );
  s.overflowing_light    = ST( "Overflowing Light" );
  s.imbued_infusions     = ST( "Imbued Infusions" );
  s.inflorescence        = ST( "Inflorescence of the Sunwell" );
  s.divine_revelations   = ST( "Divine Revelations" );
  s.moment_of_compassion = ST( "Moment of Compassion" );
  s.commanding_light     = ST( "Commanding Light" );
  s.beacon_of_virtue     = ST( "Beacon of Virtue" );
  s.pillars_of_light     = ST( "Pillars of Light" );
  s.glistening_radiance  = ST( "Glistening Radiance" );
  s.rising_sunlight      = ST( "Rising Sunlight" );
  s.seek_deliverance     = ST( "Seek Deliverance" );
  s.truth_prevails       = ST( "Truth Prevails" );
  s.light_s_conviction   = ST( "Light's Conviction" );
  s.infusion_of_light    = ST( "Infusion of Light" );
  s.extrication          = ST( "Extrication" );
  s.light_of_dawn        = ST( "Light of Dawn" );
  s.selfless_healer      = CT( "Selfless Healer" );
  s.afterimage           = CT( "Afterimage" );
  // the three Beacon of the Savior nodes share a name: pick them by spell id
  for ( unsigned id : { 1244878u, 1245368u, 1245367u } )
  {
    auto t = find_talent_spell( talent_tree::SPECIALIZATION, id, PALADIN_HOLY );
    if ( id == 1244878 ) s.savior = t;
    if ( id == 1245368 ) s.savior_2 = t;
    if ( id == 1245367 ) s.savior_3 = t;
  }

  // Beacon of Virtue and Beacon of the Savior each transfer 10% of the raw heal: the log's transfer size
  // (25.5k raw a hit) fits that, not the second Savior node's or Commanding Light's extra share
  s.bov_pct    = find_spell( 53651 )->effectN( 1 ).percent();
  s.savior_pct = s.savior.ok() ? s.savior->effectN( 2 ).percent() : 0.0;
}

double paladin_t::savior_shield_amount() const
{
  return find_spell( 1245369 )->effectN( 1 ).sp_coeff() * cache.spell_power( SCHOOL_HOLY ) *
         composite_spell_power_multiplier() * ( 1.0 + cache.heal_versatility() );
}

void paladin_t::create_holy_heal_actions()
{
  auto& s       = *hpal;
  s.beacon_heal = new hpal::flat_heal_t( this, "beacon_of_light", 53652 );
  s.savior_heal = new hpal::flat_heal_t( this, "beacon_of_the_savior_transfer", 53652 );
  if ( s.savior.ok() )
  {
    s.savior_hot = new hpal::simple_heal_t( this, "beacon_of_the_savior", 1244893, false );
    s.savior_hot->spell_power_mod.direct = find_spell( 1244893 )->effectN( 6 ).sp_coeff();  // its 3 s tick
    s.savior_shield = new hpal::flat_absorb_t( this, "beacon_of_the_savior_shield", 1245369 );
  }
  if ( s.glistening_radiance.ok() )
    s.glisten = new hpal::flat_absorb_t( this, "glistening_radiance", 1241866,
                                         s.glistening_radiance->effectN( 2 ).percent() );
  if ( s.pillars_of_light.ok() )
    s.pillar = new hpal::simple_heal_t( this, "pillar_of_lights", 1232617 );
  if ( s.overflowing_light.ok() )
    s.overflowing = new hpal::flat_absorb_t( this, "overflowing_light", 461499 );
  if ( talents.herald_of_the_sun.sun_sear->ok() )
    s.sun_sear = new hpal::sun_sear_t( this );
  if ( talents.herald_of_the_sun.dawnlight->ok() )
  {
    s.dawnlight         = new hpal::dawnlight_heal_t( this );
    s.dawnlight_radiate = new hpal::flat_heal_t( this, "dawnlight_radiance", 431382 );
  }
  if ( talents.herald_of_the_sun.suns_avatar->ok() )
    s.suns_avatar = new hpal::simple_heal_t( this, "suns_avatar_heal", 431939, false );
  if ( talents.herald_of_the_sun.second_sunrise->ok() )
  {
    // measured: the echo heals for 13.5% of the first hit (the 30% in the tooltip, on a target the first hit topped up)
    s.hs_echo = new hpal::holy_shock_heal_t( this, "holy_shock_second_sunrise", 0.135 );
    s.lod_echo = new hpal::light_of_dawn_heal_t(
        this, "light_of_dawn_second_sunrise", "", talents.herald_of_the_sun.second_sunrise->effectN( 2 ).percent() );
  }
  if ( s.afterimage.ok() )
  {
    // the shared Holy Power code counts the Holy Power and fires active.afterimage
    s.ef_echo         = new hpal::eternal_flame_t( this, "eternal_flame_afterimage", "", s.afterimage->effectN( 1 ).percent() );
    active.afterimage = s.ef_echo;
  }
  s.truth_heal  = new hpal::truth_prevails_t( this );
  s.truth_spill = new hpal::flat_heal_t( this, "truth_prevails_spill", 461529 );
  s.selfless_heal = new hpal::flat_heal_t( this, "selfless_healer", 469435 );

  if ( s.light_of_the_martyr.ok() )
  {
    s.lotm = make_buff( this, "light_of_the_martyr", find_spell( 447988 ) );
    if ( s.bestow_light.ok() )
      s.bestow = make_buff( this, "bestow_light", find_spell( 448087 ) );
  }

  // Beacon of the Savior follows the most injured ally (checked every 2 s); a new injured target gets the shield.
  // Glistening Radiance and Light of the Martyr run on the same beat.
  register_combat_begin( [ this ]( player_t* ) {
    auto& st         = *hpal;
    st.savior_target = nullptr;
    st.afterimage_spent = 0;
    make_repeating_event( *sim, 2_s, [ this ] {
      auto& st = *hpal;
      if ( st.savior.ok() )
      {
        auto l = st.injured( 1 );
        player_t* t = l.empty() ? st.savior_target : l.front();
        if ( t && t != st.savior_target )
        {
          st.savior_target = t;
          st.savior_shield->execute_on_target( t, savior_shield_amount() );
        }
      }
      if ( st.lotm )
      {
        double thr = st.bestow_light.ok() ? st.bestow_light->effectN( 4 ).base_value() + st.bestow_light->effectN( 1 ).base_value()
                                          : st.light_of_the_martyr->effectN( 1 ).base_value();
        if ( resources.pct( RESOURCE_HEALTH ) * 100 > thr )
        {
          if ( !st.lotm->check() )
            st.lotm->trigger();
        }
        else
        {
          st.lotm->expire();
          if ( st.bestow )
            st.bestow->expire();
        }
      }
    } );
    if ( st.savior_shield )
      make_repeating_event( *sim, timespan_t::from_millis( hpal->savior_2.ok() ? hpal->savior_2->effectN( 1 ).period().total_millis() : 8000 ), [ this ] {
        auto& st = *hpal;
        if ( st.savior_target )
          st.savior_shield->execute_on_target( st.savior_target, savior_shield_amount() );
      } );
    if ( st.savior_hot )
      make_repeating_event( *sim, 3_s, [ this ] {
        auto& st = *hpal;
        if ( st.savior_target )
          st.savior_hot->execute_on_target( st.savior_target );
      } );
    if ( st.glisten )
      make_repeating_event( *sim, 3_s, [ this ] {
        auto& st = *hpal;
        double share = st.glistening_radiance->effectN( 1 ).percent();
        auto give    = [ & ]( player_t* t ) {
          if ( t->resources.pct( RESOURCE_HEALTH ) >= 1.0 )
            st.glisten->execute_on_target( t, t->max_health() * share );
        };
        for ( auto& [ t, b ] : st.bov )
          if ( b->check() )
            give( t );
        if ( st.savior_target )
          give( st.savior_target );
      } );
    if ( st.bestow )
      make_repeating_event( *sim, 5_s, [ this ] {
        auto& st = *hpal;
        if ( st.lotm->check() )
          st.bestow->trigger();
      } );
  } );
}

action_t* paladin_t::create_action_holy_heal( util::string_view name, util::string_view options_str )
{
  if ( name == "holy_shock" )
    return new hpal::holy_shock_t( this, options_str );
  if ( name == "eternal_flame" || name == "word_of_glory" )
    return new hpal::eternal_flame_t( this, "eternal_flame", options_str );
  if ( name == "light_of_dawn" )
    return new hpal::light_of_dawn_heal_t( this, "light_of_dawn", options_str );
  if ( name == "flash_of_light" )
    return new hpal::flash_of_light_t( this, options_str );
  if ( name == "beacon_of_virtue" )
    return new hpal::beacon_of_virtue_t( this, options_str );
  if ( name == "divine_toll" )
    return new hpal::divine_toll_heal_t( this, options_str );
  if ( name == "judgment" )
    return new hpal::judgment_truth_t( this, options_str );
  return nullptr;
}

// Healing priority, measured on top Heroic logs (5 minutes): Holy Shock ~80, Eternal Flame ~60, Flash of Light
// ~37 (mostly Infusion of Light), Beacon of Virtue ~14 (not on cooldown: after Holy Shock and Eternal Flame),
// Judgment ~20, Light of Dawn ~11, Divine Toll ~8.
void paladin_t::apl_holy_heal()
{
  action_priority_list_t* precombat = get_action_priority_list( "precombat" );
  action_priority_list_t* def       = get_action_priority_list( "default" );
  precombat->add_action( "snapshot_stats" );

  def->add_action( "potion" );
  def->add_action( "use_items" );
  def->add_action( "avenging_wrath" );
  def->add_action( "divine_toll,if=holy_power<=2" );
  def->add_action( "light_of_dawn,if=holy_power=5&group_size>=10&buff.divine_purpose.down&cooldown.holy_shock.charges>=1" );
  def->add_action( "eternal_flame,if=holy_power>=3|buff.divine_purpose.up" );
  def->add_action( "holy_shock,if=holy_power<5" );
  def->add_action( "beacon_of_virtue" );
  def->add_action( "flash_of_light,if=buff.infusion_of_light.up" );
  def->add_action( "holy_shock" );
  def->add_action( "flash_of_light,if=mana.pct>=40+50*fight_remains%(time+fight_remains)" );
  def->add_action( "judgment" );
  def->add_action( "crusader_strike" );
}
}  // namespace paladin
