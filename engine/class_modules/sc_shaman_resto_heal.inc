// wowhub: Restoration Shaman healing for healing sims (role=heal with healing targets).
// Upstream SimC refuses Restoration healing; everything here is ours, is only created in healing sims, and is
// included from sc_shaman.cpp (shaman_t lives in that file) so the weekly engine update rebases cleanly.
// Numbers were checked against Warcraft Logs events of top Heroic Nek'zali Restoration Shamans (Midnight 12.1.5).

struct resto_heal_state_t
{
  shaman_t* p;

  // spec talents
  player_talent_t riptide, ancestral_reach, deluge, undercurrent, wavespeakers, coalescing_water, tidal_waves, earthliving,
      improved_earthliving, deeply_rooted, echo_of_elements, rip_current, overflowing_shores, soothing_rain, tidewaters,
      swelling_tides, primal_tide_core, living_stream, water_totem_mastery, resurgence, white_water, healing_rain,
      spirit_link, stormstream_proc, stormstream_power, stormstream_ns, current_control, acid_rain;
  player_talent_t earth_shield_t;

  // buffs
  buff_t *tidal = nullptr, *coalescing = nullptr, *asc = nullptr, *surging = nullptr, *whirling_earth = nullptr,
         *whirling_water = nullptr, *whirling_air = nullptr, *stormstream_ready = nullptr, *slt = nullptr, *hst_up = nullptr, *storm_up = nullptr;

  // actions
  action_t *riptide_a = nullptr, *chain_a = nullptr, *rain_tick = nullptr, *tidewaters_a = nullptr,
           *overflowing_a = nullptr, *hst_pulse = nullptr, *storm_pulse = nullptr, *stormswell_a = nullptr,
           *earthliving_a = nullptr, *rebound_a = nullptr, *asc_heal = nullptr, *condensation_a = nullptr,
           *whirl_water_a = nullptr, *cond_a = nullptr, *wind_barrier_a = nullptr, *extra_rain = nullptr, *lively_a = nullptr;
  bool tier2 = false, tier4 = false;
  int extra_rain_gen = 0;

  int rain_gen = 0, hst_gen = 0, storm_gen = 0, riptide_casts = 0;
  timespan_t cached_at = timespan_t::min();
  int cached_riptides = 0;
  double rain_cover = 0.85;      // share of the raid standing in the rain/totem area
  double spec_mult  = 1.0;       // core passive of the spec that the tooltip data does not spell out

  explicit resto_heal_state_t( shaman_t* pl ) : p( pl ) {}

  bool surging_up() const { return surging && surging->check(); }

  dot_t* rt_dot( player_t* t ) const { return riptide_a ? riptide_a->find_dot( t ) : nullptr; }

  bool has_riptide( player_t* t ) const
  {
    auto* d = rt_dot( t );
    return d && d->is_ticking();
  }

  int riptide_count()
  {
    if ( cached_at == p->sim->current_time() )
      return cached_riptides;
    int n = 0;
    for ( auto* t : p->sim->healing_no_pet_list )
      if ( !t->is_sleeping() && has_riptide( t ) )
        n++;
    cached_at       = p->sim->current_time();
    cached_riptides = n;
    return n;
  }

  // most injured allies first
  std::vector<player_t*> injured( size_t n, player_t* skip = nullptr, bool need_hurt = true ) const
  {
    std::vector<player_t*> l;
    for ( auto* t : p->sim->healing_no_pet_list )
      if ( !t->is_sleeping() && t != skip && ( !need_hurt || t->resources.pct( RESOURCE_HEALTH ) < 1.0 ) )
        l.push_back( t );
    std::sort( l.begin(), l.end(), []( player_t* a, player_t* b ) {
      return a->resources.pct( RESOURCE_HEALTH ) < b->resources.pct( RESOURCE_HEALTH );
    } );
    if ( l.size() > n )
      l.resize( n );
    return l;
  }

  player_t* most_injured( player_t* skip = nullptr ) const
  {
    auto l = injured( 1, skip, false );
    return l.empty() ? nullptr : l.front();
  }

  player_t* random_ally( player_t* skip = nullptr ) const
  {
    std::vector<player_t*> l;
    for ( auto* t : p->sim->healing_no_pet_list )
      if ( !t->is_sleeping() && t != skip )
        l.push_back( t );
    return l.empty() ? nullptr : l[ p->rng().range( l.size() ) ];
  }

  double global_mult() const
  {
    double m = spec_mult;
    if ( undercurrent.ok() )
      m *= 1.0 + undercurrent->effectN( 1 ).percent() * const_cast<resto_heal_state_t*>( this )->riptide_count();
    if ( p->talent.amplification_core.ok() && surging_up() )
      m *= 1.0 + p->find_spell( 456369 )->effectN( 1 ).percent();
    return m;
  }
};

namespace rheal
{
// Base of every Restoration heal: Mastery: Deep Healing, Undercurrent, Amplification Core, spec core passive.
struct rh_heal_t : public heal_t
{
  double effectiveness   = 1.0;
  double mastery_share   = 1.0;
  bool use_mastery       = true;
  bool deluge_applies    = false;

  rh_heal_t( util::string_view n, shaman_t* pl, const spell_data_t* s ) : heal_t( n, pl, s ) {}

  shaman_t* p() const { return debug_cast<shaman_t*>( player ); }
  resto_heal_state_t& st() const { return *p()->rh; }

  virtual double extra_mult( const action_state_t* ) const { return 1.0; }

  double heal_mult( const action_state_t* s ) const
  {
    double m = effectiveness * extra_mult( s );
    if ( use_mastery )
      m *= 1.0 + p()->cache.mastery_value() * mastery_share * ( 1.0 - s->target->resources.pct( RESOURCE_HEALTH ) );
    m *= st().global_mult();
    if ( p()->talent.earthsurge.ok() && st().earthliving_a )
      if ( auto* d = st().earthliving_a->find_dot( s->target ) )
        if ( d->is_ticking() )
          m *= 1.0 + p()->talent.earthsurge->effectN( 3 ).percent();
    if ( deluge_applies && st().deluge.ok() &&
         ( st().has_riptide( s->target ) || ( st().surging_up() && p()->rng().roll( st().rain_cover ) ) ) )
      m *= 1.0 + st().deluge->effectN( 1 ).percent();
    return m;
  }

  double composite_da_multiplier( const action_state_t* s ) const override
  {
    return heal_t::composite_da_multiplier( s ) * heal_mult( s );
  }
  double composite_ta_multiplier( const action_state_t* s ) const override
  {
    return heal_t::composite_ta_multiplier( s ) * heal_mult( s );
  }

  // MID2 two-piece: Healing Wave and Chain Heal sometimes make a second Healing Rain (8 s) at the target
  void roll_tier2( const action_state_t* s );

  // Earthliving rolls on direct heals of Riptide, Healing Wave and Chain Heal
  void roll_earthliving( const action_state_t* s, bool always = false )
  {
    if ( !st().earthliving.ok() || !st().earthliving_a )
      return;
    double chance = st().earthliving->effectN( 1 ).percent();
    chance        = 0.20;
    if ( always || rng().roll( chance ) )
      st().earthliving_a->execute_on_target( s->target );
  }
};

// A heal fired by our own code at a chosen target (procs, totem pulses, set bonuses).
struct simple_heal_t : public rh_heal_t
{
  simple_heal_t( util::string_view n, shaman_t* pl, unsigned id, bool mastery = true ) : rh_heal_t( n, pl, pl->find_spell( id ) )
  {
    background = proc = true;
    use_mastery       = mastery;
    auto_target       = false;
    aoe               = 0;
  }
};

// Earthliving: HoT on the target
struct earthliving_t : public rh_heal_t
{
  earthliving_t( shaman_t* pl ) : rh_heal_t( "earthliving", pl, pl->find_spell( 382024 ) )
  {
    background = proc = true;
    auto_target       = false;
    dot_duration      = data().duration();
    if ( pl->rh->improved_earthliving.ok() )
      mastery_share = 1.0 + pl->rh->improved_earthliving->effectN( 1 ).percent();
    effectiveness = 1.45;  // logs: Earthliving ticks are ~1.45x what the tooltip data gives (unexplained)
    base_tick_time = pl->find_spell( 382024 )->effectN( 1 ).period();
    hasted_ticks   = true;
    tick_may_crit  = true;
  }

};

// A shield with a fixed amount (set bonus, Wind Barrier)
struct flat_absorb_t : public absorb_t
{
  flat_absorb_t( shaman_t* pl, util::string_view n, unsigned id ) : absorb_t( n, pl, pl->find_spell( id ) )
  {
    background = proc = true;
    may_crit = callbacks = false;
    harmful = false;
    target  = pl;
  }
  void init() override
  {
    absorb_t::init();
    snapshot_flags = update_flags = 0;
  }
  void assess_damage( result_amount_type, action_state_t* s ) override
  {
    if ( target_specific[ s->target ] == nullptr )
      target_specific[ s->target ] = create_buff( s );
    auto* b = target_specific[ s->target ];
    stats->add_result( 0.0, s->result_amount, result_amount_type::ABSORB, s->result, s->block_result, s->target );
    b->trigger( 1, s->result_amount );
  }
};

struct riptide_t : public rh_heal_t
{
  bool free_cast = false;

  void update_ready( timespan_t cd = timespan_t::min() ) override
  {
    if ( !free_cast )
      rh_heal_t::update_ready( cd );
  }

  riptide_t( shaman_t* pl, util::string_view options_str ) : rh_heal_t( "riptide", pl, pl->find_spell( 61295 ) )
  {
    parse_options( options_str );
    auto& s = *pl->rh;
    s.riptide_a = this;
    // Wavespeaker's Blessing, Echo of the Elements and Rip Current come from the engine's talent parsing
    dot_behavior = dot_behavior_e::DOT_REFRESH_PANDEMIC;
  }

  void tick( dot_t* d ) override
  {
    rh_heal_t::tick( d );
    roll_earthliving( d->state );
  }

  double composite_da_multiplier( const action_state_t* s ) const override
  {
    double m = rh_heal_t::composite_da_multiplier( s );
    if ( st().coalescing_water.ok() && st().coalescing->check() )
      m *= 1.0 + st().coalescing->check() * p()->find_spell( 470077 )->effectN( 1 ).percent();
    return m;
  }

  void impact( action_state_t* s ) override
  {
    rh_heal_t::impact( s );
    if ( s->result == RESULT_CRIT && st().resurgence.ok() )
      p()->resource_gain( RESOURCE_MANA, 0.0048 * p()->resources.max[ RESOURCE_MANA ], p()->gain.resurgence );
    roll_earthliving( s );
  }

  void execute() override
  {
    int coal = st().coalescing->check();
    st().riptide_a = this;
    rh_heal_t::execute();
    if ( free_cast )
      return;
    if ( coal )
      st().coalescing->expire();
    p()->resource_gain( RESOURCE_MANA, 0.0039 * p()->resources.max[ RESOURCE_MANA ], p()->gain.resurgence );
    auto& s = st();
    if ( s.tidal_waves.ok() )
      s.tidal->trigger();
    s.riptide_casts++;
    // Primal Tide Core: every Nth Riptide also lands on another ally
    if ( s.primal_tide_core.ok() && s.riptide_casts % as<int>( s.primal_tide_core->effectN( 1 ).base_value() ) == 0 )
      if ( auto* t = s.most_injured( execute_state->target ) )
        execute_on_target_free( t );
    if ( s.deeply_rooted.ok() && rng().roll( s.deeply_rooted->effectN( 4 ).percent() ) )
      s.asc->trigger( timespan_t::from_millis( s.deeply_rooted->effectN( 1 ).base_value() ) );
    if ( s.stormstream_proc.ok() && rng().roll( s.stormstream_proc->effectN( 1 ).percent() ) )
      s.stormstream_ready->trigger();
  }

  void execute_on_target_free( player_t* t )
  {
    // the extra Riptide costs nothing and does not touch the cooldown
    double c = base_costs[ RESOURCE_MANA ];
    base_costs[ RESOURCE_MANA ] = 0;
    bool bg = background;
    background = true;
    free_cast  = true;
    set_target( t );
    execute();
    free_cast  = false;
    background = bg;
    base_costs[ RESOURCE_MANA ] = c;
  }
};

struct chain_heal_t : public rh_heal_t
{
  int base_aoe;
  double base_chain;
  bool lively;  // Lively Totems: the free instant Chain Heal on Healing Stream / Spirit Link / Healing Tide Totem casts

  chain_heal_t( shaman_t* pl, util::string_view options_str, bool free_ = false )
    : rh_heal_t( free_ ? "lively_chain_heal" : "chain_heal", pl, pl->find_spell( 1064 ) ), lively( free_ )
  {
    parse_options( options_str );
    if ( lively )
    {
      background = proc = true;
      auto_target       = true;
      base_costs[ RESOURCE_MANA ] = 0;
      base_execute_time = 0_ms;
      trigger_gcd       = 0_ms;
    }
    deluge_applies = true;
    auto& s        = *pl->rh;
    s.chain_a      = this;
    base_aoe       = 0;
    base_chain     = 1.0;
  }

  timespan_t execute_time() const override
  {
    if ( p()->buff.natures_swiftness->check() )
      return 0_ms;
    timespan_t t = rh_heal_t::execute_time();
    if ( st().tidal->check() )
      t *= 1.0 - st().tidal->data().effectN( 1 ).percent() * -1.0 * 1.0;
    if ( st().whirling_air->check() )
      t *= 1.0 + st().whirling_air->data().effectN( 1 ).percent();
    return t;
  }

  double cost_pct_multiplier() const override
  {
    double c = rh_heal_t::cost_pct_multiplier();
    if ( st().asc->check() )
      c *= 1.0 + p()->find_spell( 114052 )->effectN( 16 ).percent();
    if ( p()->buff.natures_swiftness->check() )
      c = 0.0;
    return c;
  }

  void execute() override
  {
    auto& s = st();
    if ( !lively )
      s.chain_a = this;
    if ( !base_aoe )  // the engine has applied Ancestral Reach by now
    {
      base_aoe   = aoe;
      base_chain = chain_multiplier;
    }
    aoe              = base_aoe;
    chain_multiplier = base_chain;
    if ( s.asc->check() )
    {
      aoe += as<int>( p()->find_spell( 114052 )->effectN( 9 ).base_value() );
      chain_multiplier = 1.0 - p()->find_spell( 114052 )->effectN( 17 ).percent();
    }
    target_cache.is_valid = false;
    bool ns               = !lively && p()->buff.natures_swiftness->check();
    bool wearth           = s.whirling_earth->check();
    if ( lively )
      target_cache.is_valid = false;
    rh_heal_t::execute();
    if ( ns )
      p()->buff.natures_swiftness->decrement();
    if ( s.tidal->check() && !ns && !lively )
      s.tidal->decrement();
    if ( wearth )
      s.whirling_earth->expire();
    if ( s.coalescing_water.ok() )
      s.coalescing->trigger();
    // Totemic Rebound: after the last target the heal jumps to the Surging Totem, which casts its own Chain Heal
    if ( s.rebound_a && s.surging_up() && !lively )
    {
      s.rebound_a->aoe = 3 + ( s.asc->check() ? as<int>( p()->find_spell( 114052 )->effectN( 9 ).base_value() ) : 0 );
      s.rebound_a->execute_on_target( s.most_injured() );
    }
  }

  void impact( action_state_t* s ) override
  {
    rh_heal_t::impact( s );
    if ( s->result == RESULT_CRIT && st().resurgence.ok() )
      p()->resource_gain( RESOURCE_MANA, 0.0020 * p()->resources.max[ RESOURCE_MANA ], p()->gain.resurgence );
    roll_earthliving( s, st().whirling_earth->check() );
    if ( s->chain_target == 0 )
      roll_tier2( s );
  }
};

struct healing_wave_t : public rh_heal_t
{
  healing_wave_t( shaman_t* pl, util::string_view options_str ) : rh_heal_t( "healing_wave", pl, pl->find_spell( 77472 ) )
  {
    parse_options( options_str );
    deluge_applies = true;
  }

  timespan_t execute_time() const override
  {
    if ( p()->buff.natures_swiftness->check() )
      return 0_ms;
    timespan_t t = rh_heal_t::execute_time();
    if ( st().tidal->check() )
      t *= 1.0 - st().tidal->data().effectN( 1 ).percent() * -1.0;
    if ( st().whirling_air->check() )
      t *= 1.0 + st().whirling_air->data().effectN( 1 ).percent();
    return t;
  }

  double cost_pct_multiplier() const override
  {
    double c = rh_heal_t::cost_pct_multiplier();
    if ( st().asc->check() )
      c *= 1.0 + p()->find_spell( 114052 )->effectN( 16 ).percent();
    if ( p()->buff.natures_swiftness->check() )
      c = 0.0;
    return c;
  }

  double composite_crit_chance() const override
  {
    if ( st().asc->check() )
      return 1.0;
    return rh_heal_t::composite_crit_chance();
  }

  void execute() override
  {
    auto& s     = st();
    s.chain_a   = this;
    bool ns     = p()->buff.natures_swiftness->check();
    bool wwater = s.whirling_water->check();
    rh_heal_t::execute();
    if ( ns )
    {
      p()->buff.natures_swiftness->decrement();
      if ( s.stormstream_ns.ok() )
        s.stormstream_ready->trigger();
    }
    if ( s.tidal->check() && !ns )
      s.tidal->decrement();
    if ( s.coalescing_water.ok() )
      s.coalescing->trigger();
    if ( wwater )
    {
      s.whirling_water->expire();
      if ( s.whirl_water_a && s.surging_up() )
        s.whirl_water_a->execute_on_target( s.most_injured( execute_state->target ) );
    }
  }

  void impact( action_state_t* s ) override
  {
    rh_heal_t::impact( s );
    if ( s->result == RESULT_CRIT && st().resurgence.ok() )
      p()->resource_gain( RESOURCE_MANA, 0.0080 * p()->resources.max[ RESOURCE_MANA ], p()->gain.resurgence );
    roll_earthliving( s, st().improved_earthliving.ok() );
    roll_tier2( s );
  }
};

struct healing_surge_t : public rh_heal_t
{
  healing_surge_t( shaman_t* pl, util::string_view options_str ) : rh_heal_t( "healing_surge", pl, pl->find_spell( 8004 ) )
  {
    parse_options( options_str );
  }
  timespan_t execute_time() const override
  {
    if ( p()->buff.natures_swiftness->check() )
      return 0_ms;
    return rh_heal_t::execute_time();
  }
  void execute() override
  {
    bool ns = p()->buff.natures_swiftness->check();
    rh_heal_t::execute();
    if ( ns )
      p()->buff.natures_swiftness->decrement();
  }
};

// Healing Rain (Surging Totem replaces it): ticks on up to 6 allies; Tidewaters and Overflowing Shores on totem casts
struct rain_tick_t : public rh_heal_t
{
  rain_tick_t( shaman_t* pl ) : rh_heal_t( "healing_rain", pl, pl->find_spell( 73921 ) )
  {
    background = true;
    aoe        = 6;
    auto_target = true;
  }
};

struct surging_totem_t : public spell_t
{
  surging_totem_t( shaman_t* pl, util::string_view options_str ) : spell_t( "surging_totem", pl, pl->find_spell( 444995 ) )
  {
    parse_options( options_str );
    harmful = false;
    may_miss = may_crit = false;
  }

  shaman_t* p() const { return debug_cast<shaman_t*>( player ); }

  void start_rain()
  {
    auto& s  = *p()->rh;
    int gen  = ++s.rain_gen;
    auto end = sim->current_time() + data().duration();
    s.surging->trigger( data().duration() );
    pulse( gen, end );
  }

  void pulse( int gen, timespan_t end )
  {
    auto& s = *p()->rh;
    timespan_t dt = p()->find_spell( 73920 )->effectN( 3 ).period() * p()->cache.spell_haste();
    if ( sim->current_time() + dt > end )
      return;
    make_event( *sim, dt, [ this, gen, end ] {
      auto& s = *p()->rh;
      if ( gen != s.rain_gen )
        return;
      if ( player_t* t = s.most_injured() )
        s.rain_tick->execute_on_target( t );
      pulse( gen, end );
    } );
  }

  void execute() override
  {
    spell_t::execute();
    auto& s = *p()->rh;
    start_rain();
    p()->talent.whirling_elements.ok();
    if ( p()->talent.whirling_elements.ok() )
    {
      s.whirling_earth->trigger();
      s.whirling_water->trigger();
      s.whirling_air->trigger();
    }
    totem_cast_procs( s, true );
  }

  static void totem_cast_procs( resto_heal_state_t& s, bool surging )
  {
    // Tidewaters (Riptide targets) and Overflowing Shores (6 allies) fire on totem casts in the logs
    s.p->sim->print_debug( "{} totem cast procs: tidewaters={} {} riptides={}", *s.p, s.tidewaters.ok(), s.tidewaters_a != nullptr, s.riptide_count() );
    if ( s.tidewaters.ok() && s.tidewaters_a )
      for ( auto* t : s.p->sim->healing_no_pet_list )
        if ( !t->is_sleeping() && s.has_riptide( t ) )
          s.tidewaters_a->execute_on_target( t );
    if ( s.overflowing_shores.ok() && s.overflowing_a )
      for ( auto* t : s.injured( 6, nullptr, false ) )
        s.overflowing_a->execute_on_target( t );
    if ( !surging && s.lively_a )
      if ( auto* t = s.most_injured() )
        s.lively_a->execute_on_target( t );
  }
};

inline void rh_heal_t::roll_tier2( const action_state_t* s )
{
  auto& r = st();
  if ( !r.tier2 || !r.extra_rain || background )
    return;
  if ( !rng().roll( 0.20 ) )
    return;
  int gen      = ++r.extra_rain_gen;
  timespan_t end = sim->current_time() + p()->find_spell( 1296629 )->effectN( 1 ).time_value();
  struct tick_t
  {
    static void go( shaman_t* pl, int gen, timespan_t end )
    {
      auto& r       = *pl->rh;
      timespan_t dt = pl->find_spell( 73920 )->effectN( 3 ).period() * pl->cache.spell_haste();
      if ( pl->sim->current_time() + dt > end )
        return;
      make_event( *pl->sim, dt, [ pl, gen, end ] {
        auto& r = *pl->rh;
        if ( gen != r.extra_rain_gen )
          return;
        if ( player_t* t = r.most_injured() )
          r.extra_rain->execute_on_target( t );
        go( pl, gen, end );
      } );
      (void)r;
    }
  };
  tick_t::go( p(), gen, end );
  (void)s;
}

// Healing Stream Totem and its Stormstream upgrade
constexpr double hst_calibration = 0.85;  // logs: Healing Stream hits are ~0.6x what the tooltip data gives (unexplained)

struct hst_pulse_t : public rh_heal_t
{
  bool storm;
  double stage = 1.0;
  std::vector<double> weights;  // effectiveness of the 1st, 2nd, ... target
  hst_pulse_t( shaman_t* pl, bool st_ ) : rh_heal_t( st_ ? "stormstream_totem" : "healing_stream_totem", pl, pl->find_spell( st_ ? 1267745 : 52042 ) ), storm( st_ )
  {
    background = true;
    aoe        = 1;
    auto_target = true;
  }
  double extra_mult( const action_state_t* s ) const override
  {
    double m = stage * hst_calibration;
    if ( s->chain_target < weights.size() )
      m *= weights[ s->chain_target ];
    return m;
  }

  void impact( action_state_t* s ) override
  {
    rh_heal_t::impact( s );
    if ( p()->talent.primal_catalyst.ok() && rng().roll( p()->talent.primal_catalyst->effectN( 2 ).percent() ) )
      roll_earthliving( s, true );
  }
};

struct healing_stream_totem_t : public spell_t
{
  bool storm_variant;
  healing_stream_totem_t( shaman_t* pl, util::string_view options_str, bool storm )
    : spell_t( storm ? "stormstream_totem_cast" : "healing_stream_totem_cast", pl, pl->find_spell( 5394 ) ), storm_variant( storm )
  {
    parse_options( options_str );
    harmful = false;
    may_miss = may_crit = false;
    auto& s = *pl->rh;
    if ( storm )
    {
      cooldown = pl->get_cooldown( "stormstream_totem_free" );
      cooldown->duration = 0_ms;
    }
  }

  shaman_t* p() const { return debug_cast<shaman_t*>( player ); }

  bool ready() override
  {
    if ( storm_variant && !p()->rh->stormstream_ready->check() )
      return false;
    return spell_t::ready();
  }

  void execute() override
  {
    auto& s = *p()->rh;
    bool storm = storm_variant;
    spell_t::execute();
    if ( storm )
      s.stormstream_ready->expire();
    int gen = storm ? ++s.storm_gen : ++s.hst_gen;
    timespan_t dur = data().duration() + ( p()->talent.totemic_focus.ok() ? p()->talent.totemic_focus->effectN( 2 ).time_value() : 0_ms );
    ( storm ? s.storm_up : s.hst_up )->trigger( dur );
    int n = 1;
    if ( p()->talent.splitstream.ok() ) n += 1;
    if ( storm && s.stormstream_proc.ok() ) n += as<int>( s.stormstream_proc->effectN( 3 ).base_value() );
    hst_pulse_t* pulse = debug_cast<hst_pulse_t*>( storm ? s.storm_pulse : s.hst_pulse );
    pulse->aoe = n;
    pulse->weights.clear();
    pulse->weights.push_back( 1.0 );
    if ( storm && s.stormstream_proc.ok() )
      for ( int i = 0; i < as<int>( s.stormstream_proc->effectN( 3 ).base_value() ); i++ )
        pulse->weights.push_back( s.stormstream_proc->effectN( 4 ).percent() );
    if ( p()->talent.splitstream.ok() )
      pulse->weights.push_back( p()->talent.splitstream->effectN( 3 ).percent() );
    if ( s.swelling_tides.ok() )
      for ( auto* t : p()->sim->healing_no_pet_list )
        if ( !t->is_sleeping() )
          if ( auto* d = s.rt_dot( t ) )
            if ( d->is_ticking() )
              d->adjust_duration( timespan_t::from_millis( s.swelling_tides->effectN( 1 ).base_value() ) );
    run( gen, sim->current_time(), sim->current_time() + dur, pulse, storm );
    resto_heal_state_t* sp = &s;
    surging_totem_t::totem_cast_procs( *sp, false );
    if ( storm && s.stormswell_a )
      for ( auto* t : s.injured( as<size_t>( s.stormstream_proc->effectN( 5 ).base_value() ), nullptr, false ) )
        s.stormswell_a->execute_on_target( t );
  }

  void run( int gen, timespan_t start, timespan_t end, hst_pulse_t* pulse, bool storm )
  {
    timespan_t dt = p()->find_spell( 5672 )->effectN( 1 ).period() * p()->cache.spell_haste();
    if ( sim->current_time() + dt > end + 1_ms )
      return;
    make_event( *sim, dt, [ this, gen, start, end, pulse, storm ] {
      auto& s = *p()->rh;
      (void)gen;  // totems of the same kind can overlap in the logs
      double progress = ( sim->current_time() - start ) / ( end - start );
      pulse->stage    = s.living_stream.ok() ? 1.0 + s.living_stream->effectN( 1 ).percent() * ( 1.0 - progress ) : 1.0;
      pulse->execute_on_target( s.most_injured() );
      run( gen, start, end, pulse, storm );
    } );
  }
};

struct ascendance_t : public spell_t
{
  ascendance_t( shaman_t* pl, util::string_view options_str ) : spell_t( "ascendance", pl, pl->find_spell( 114052 ) )
  {
    parse_options( options_str );
    harmful = false;
    may_miss = may_crit = false;
  }
  shaman_t* p() const { return debug_cast<shaman_t*>( player ); }
  bool ready() override { return p()->rh->asc && !p()->rh->asc->check() && spell_t::ready(); }
  void execute() override
  {
    spell_t::execute();
    auto& s = *p()->rh;
    timespan_t d = data().duration();
    if ( p()->talent.preeminence.ok() )
      d += timespan_t::from_millis( p()->talent.preeminence->effectN( 1 ).base_value() );
    s.asc->trigger( d );
    if ( s.asc_heal )
      s.asc_heal->execute_on_target( s.most_injured() );
  }
};

struct spirit_link_totem_t : public spell_t
{
  spirit_link_totem_t( shaman_t* pl, util::string_view options_str ) : spell_t( "spirit_link_totem", pl, pl->find_spell( 98008 ) )
  {
    parse_options( options_str );
    harmful = false;
    may_miss = may_crit = false;
  }
  shaman_t* p() const { return debug_cast<shaman_t*>( player ); }
  void execute() override
  {
    spell_t::execute();
    surging_totem_t::totem_cast_procs( *p()->rh, false );
  }
};
}  // namespace rheal

// ==========================================================================
// Hooks
// ==========================================================================
bool shaman_t::resto_heal_mode() const
{
  return specialization() == SHAMAN_RESTORATION && primary_role() == ROLE_HEAL && sim->heal_target;
}

void shaman_t::init_resto_heal()
{
  rh      = new resto_heal_state_t( this );
  auto& s = *rh;
  // the class passive's -36% mana regeneration is meant for the damage specs
  resources.base_regen_per_second[ RESOURCE_MANA ] /= 1.0 + find_spell( 137038 )->effectN( 1 ).percent();
  auto ST = [ this ]( std::string_view n ) { return find_talent_spell( talent_tree::SPECIALIZATION, n ); };
  auto CT = [ this ]( std::string_view n ) { return find_talent_spell( talent_tree::CLASS, n ); };
  s.riptide              = ST( "Riptide" );
  s.ancestral_reach      = ST( "Ancestral Reach" );
  s.deluge               = ST( "Deluge" );
  s.undercurrent         = ST( "Undercurrent" );
  s.wavespeakers         = ST( "Wavespeaker's Blessing" );
  s.coalescing_water     = ST( "Coalescing Water" );
  s.tidal_waves          = ST( "Tidal Waves" );
  s.earthliving          = ST( "Earthliving Weapon" );
  s.improved_earthliving = ST( "Improved Earthliving Weapon" );
  s.deeply_rooted        = ST( "Deeply Rooted Elements" );
  s.echo_of_elements     = ST( "Echo of the Elements" );
  s.rip_current          = ST( "Rip Current" );
  s.overflowing_shores   = ST( "Overflowing Shores" );
  s.soothing_rain        = ST( "Soothing Rain" );
  s.tidewaters           = ST( "Tidewaters" );
  s.swelling_tides       = ST( "Swelling Tides" );
  s.primal_tide_core     = ST( "Primal Tide Core" );
  s.living_stream        = ST( "Living Stream" );
  s.water_totem_mastery  = ST( "Water Totem Mastery" );
  s.resurgence           = ST( "Resurgence" );
  s.white_water          = ST( "White Water" );
  s.healing_rain         = ST( "Healing Rain" );
  s.spirit_link          = ST( "Spirit Link Totem" );
  s.current_control      = ST( "Current Control" );
  s.acid_rain            = ST( "Acid Rain" );
  s.stormstream_ns       = find_talent_spell( talent_tree::SPECIALIZATION, 1267120u, SHAMAN_RESTORATION );
  s.stormstream_power    = find_talent_spell( talent_tree::SPECIALIZATION, 1267093u, SHAMAN_RESTORATION );
  s.stormstream_proc     = find_talent_spell( talent_tree::SPECIALIZATION, 1267016u, SHAMAN_RESTORATION );
}

void shaman_t::create_resto_heal_actions()
{
  auto& s = *rh;
  s.tidal      = make_buff( this, "tidal_waves_rh", find_spell( 53390 ) )->set_max_stack( 2 );
  s.coalescing = make_buff( this, "coalescing_water_rh", find_spell( 470077 ) );
  s.asc        = make_buff( this, "ascendance_rh", find_spell( 114052 ) )->set_duration( 10_s );
  if ( talent.preeminence.ok() )
    s.asc->set_default_value( talent.preeminence->effectN( 2 ).percent() )
        ->set_pct_buff_type( STAT_PCT_BUFF_HASTE );
  s.surging        = make_buff( this, "surging_totem_rh", find_spell( 444995 ) )->set_cooldown( 0_ms );
  s.whirling_earth = make_buff( this, "whirling_earth_rh", find_spell( 453406 ) );
  s.whirling_water = make_buff( this, "whirling_water_rh", find_spell( 453407 ) );
  s.whirling_air   = make_buff( this, "whirling_air_rh", find_spell( 453409 ) );
  s.hst_up   = make_buff( this, "healing_stream_totem_rh", find_spell( 5394 ) )->set_cooldown( 0_ms );
  s.storm_up = make_buff( this, "stormstream_totem_rh", find_spell( 1267068 ) )->set_cooldown( 0_ms );
  s.stormstream_ready = make_buff( this, "stormstream_ready_rh", find_spell( 1267016 ) )->set_max_stack( 1 )->set_duration( 120_s );

  if ( talent.lively_totems.ok() )
    s.lively_a = new rheal::chain_heal_t( this, "", true );
  s.earthliving_a = new rheal::earthliving_t( this );
  s.rain_tick     = new rheal::rain_tick_t( this );
  if ( s.tidewaters.ok() )
    s.tidewaters_a = new rheal::simple_heal_t( "tidewaters", this, 462425 );
  if ( s.overflowing_shores.ok() )
    s.overflowing_a = new rheal::simple_heal_t( "overflowing_shores", this, 383223 );
  s.hst_pulse   = new rheal::hst_pulse_t( this, false );
  s.storm_pulse = new rheal::hst_pulse_t( this, true );
  if ( s.stormstream_proc.ok() )
    s.stormswell_a = new rheal::simple_heal_t( "stormswell", this, 1268684 );
  if ( talent.totemic_rebound.ok() )
  {
    auto* r = new rheal::simple_heal_t( "totemic_rebound_chain_heal", this, 458357 );
    r->aoe = 3;
    r->chain_multiplier = 0.7;
    r->auto_target = true;
    s.rebound_a = r;
  }
  s.tier2 = sets->has_set_bonus( SHAMAN_RESTORATION, MID2, B2 );
  s.tier4 = sets->has_set_bonus( SHAMAN_RESTORATION, MID2, B4 );
  if ( s.tier2 )
  {
    auto* r = new rheal::rain_tick_t( this );
    r->name_str = "healing_rain_tier";
    s.extra_rain = r;
  }
  if ( s.tier4 )
  {
    s.cond_a = new rheal::flat_absorb_t( this, "condensation", 1300642 );
  }
  {
    auto* a = new rheal::simple_heal_t( "restorative_mists", this, 294020 );
    a->aoe = 15;
    a->effectiveness = 1.0 / 15.0;
    a->auto_target = true;
    s.asc_heal = a;
  }
  if ( find_talent_spell( talent_tree::HERO, "Wind Barrier" ).ok() )
    s.wind_barrier_a = new rheal::flat_absorb_t( this, "wind_barrier", 457387 );
  register_combat_begin( [ this ]( player_t* ) {
    auto& st = *rh;
    if ( st.cond_a )
      make_repeating_event( *sim, 1_s, [ this ] {
        auto& st = *rh;
        if ( !st.surging_up() )
          return;
        double amt = find_spell( 1300642 )->effectN( 1 ).sp_coeff() * cache.spell_power( SCHOOL_NATURE ) *
                     composite_spell_power_multiplier() * ( 1.0 + cache.heal_versatility() );
        // logs: the shields absorb ~0.6x of their size on average
        for ( int i = 0; i < 2; i++ )
          if ( auto* t = st.random_ally() )
            st.cond_a->execute_on_target( t, amt * 0.62 );
      } );
    if ( st.wind_barrier_a )
      make_repeating_event( *sim, 30_s, [ this ] {
        auto& st = *rh;
        if ( st.surging_up() )
          st.wind_barrier_a->execute_on_target( this, resources.max[ RESOURCE_HEALTH ] * 0.06 );
      } );
  } );
  if ( talent.whirling_elements.ok() )
    s.whirl_water_a = new rheal::simple_heal_t( "whirling_water_healing_wave", this, 77472 );
}

action_t* shaman_t::create_action_resto_heal( util::string_view name, util::string_view options_str )
{
  if ( name == "riptide" )
    return new rheal::riptide_t( this, options_str );
  if ( name == "chain_heal" )
    return new rheal::chain_heal_t( this, options_str );
  if ( name == "healing_wave" )
    return new rheal::healing_wave_t( this, options_str );
  if ( name == "healing_surge" )
    return new rheal::healing_surge_t( this, options_str );
  if ( name == "surging_totem" || name == "healing_rain" )
    return new rheal::surging_totem_t( this, options_str );
  if ( name == "healing_stream_totem" )
    return new rheal::healing_stream_totem_t( this, options_str, false );
  if ( name == "stormstream_totem" )
    return new rheal::healing_stream_totem_t( this, options_str, true );
  if ( name == "ascendance" )
    return new rheal::ascendance_t( this, options_str );
  if ( name == "spirit_link_totem" )
    return new rheal::spirit_link_totem_t( this, options_str );
  return nullptr;
}

// Healing priority (measured on top Heroic logs, ~236 s): Chain Heal ~93, Riptide ~43, Healing Stream Totem ~21,
// Surging Totem ~10 (on cooldown), Healing Wave ~7 (Nature's Swiftness), Ascendance 2, Spirit Link Totem 2.
void shaman_t::apl_resto_heal()
{
  action_priority_list_t* precombat = get_action_priority_list( "precombat" );
  action_priority_list_t* def       = get_action_priority_list( "default" );
  precombat->add_action( "snapshot_stats" );
  precombat->add_action( "surging_totem" );

  def->add_action( "potion" );
  def->add_action( "use_items" );
  def->add_action( "ascendance" );
  def->add_action( "surging_totem" );
  def->add_action( "spirit_link_totem,if=group_size>=10&time>30&cooldown.spirit_link_totem.up&fight_remains>15" );
  def->add_action( "stormstream_totem" );
  def->add_action( "healing_stream_totem" );
  def->add_action( "natures_swiftness,if=group_size>=10" );
  def->add_action( "healing_wave,if=buff.natures_swiftness.up" );
  def->add_action( "riptide" );
  def->add_action( "chain_heal,if=injured_90>=1+group_size%7&mana.pct>=10+80*fight_remains%(time+fight_remains)" );
  def->add_action( "chain_heal,if=buff.ascendance_rh.up&injured_95>=1" );
}
