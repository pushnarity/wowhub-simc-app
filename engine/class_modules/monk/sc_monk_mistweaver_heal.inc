// wowhub: Mistweaver Monk healing for healing sims (role=heal with healing targets).
// Upstream SimC refuses Mistweaver; everything here is ours, is only created in healing sims, and is included from
// sc_monk.cpp (monk_t and the monk action templates live in that file) so the weekly engine update rebases cleanly.
// Numbers were checked against Warcraft Logs events of top Heroic Nek'zali Mistweavers (Midnight 12.1.5).

struct mw_heal_state_t
{
  monk_t* p;
  std::map<std::string, double> tune;

  // spec / class / hero talents (looked up by name; unlisted ones simply stay off)
  player_talent_t invigorating_mists, rapid_diffusion, dancing_mists, jade_bond, misty_peaks, focused_thunder,
      rising_mist, lotus_infusion, deep_clarity, mistline, secret_infusion, celestial_harmony, invoke_yulon,
      revival_t, mana_tea_t, thunder_focus_tea_t, enveloping_mist_t, rushing_wind_kick_t, spiritfont, life_cocoon_t,
      zen_pulse, misty_coalescence, way_of_the_serpent, mist_wrap, ancient_teachings, mist_caller, vital_clarity,
      silent_sanctuary, chi_warding, vivacious, yulons_grace, soothing_mist_t, chi_wave_t;
  player_talent_t celestial_conduit_t, courage_of_the_white_tiger, strength_of_the_black_ox, heart_of_the_jade_serpent,
      yulons_avatar, flowing_wisdom, unity_within, chijis_swiftness, jade_sanctuary;

  // buffs
  buff_t *tft = nullptr, *viv_free = nullptr, *zen_ready = nullptr, *spiritfont_b = nullptr, *yulon_b = nullptr,
         *conduit_b = nullptr, *mana_tea_stacks = nullptr, *secret = nullptr;

  // actions
  action_t *rm_hot = nullptr, *rm_hot2 = nullptr, *em_hot = nullptr, *em_hot2 = nullptr, *gust_a = nullptr, *im_a = nullptr, *zen_a = nullptr,
           *rwk_heal_a = nullptr, *rwk_dmg_a = nullptr, *soothing_breath_a = nullptr, *conduit_heal_a = nullptr,
           *ancient_a = nullptr, *cocoon_a = nullptr, *chi_wave_a = nullptr, *sanctuary_a = nullptr,
           *revival_a = nullptr, *courage_a = nullptr, *yulons_grace_a = nullptr, *sf_soothing_a = nullptr,
           *sf_cocoon_a = nullptr, *sanctuary_heal_a = nullptr;

  // fistweaving build (Chi-Ji, Ancient Teachings, Way of the Crane): only when those talents are picked
  bool fist = false;
  player_talent_t way_of_the_crane, jadefire_teachings, crane_style, invoke_chiji_t, sheiluns_gift_t, emperors_elixir,
      morning_breeze, xuens_guidance, healing_elixir_t, mists_of_life;
  buff_t *totm = nullptr, *chiji_b = nullptr;
  action_t *at_a = nullptr, *at_k[ 5 ] = { nullptr, nullptr, nullptr, nullptr, nullptr }, *wotc_a = nullptr, *chiji_gust_a = nullptr, *sg_main_a = nullptr, *sg_other_a = nullptr,
           *chiji_coc_a = nullptr, *he_a = nullptr, *grace_a = nullptr, *hit_rsk = nullptr, *hit_tp = nullptr,
           *hit_bok = nullptr, *hit_sck = nullptr, *hit_jf = nullptr;
  cooldown_t* cd_rsk = nullptr;
  action_t *ox_a = nullptr, *ward_a = nullptr, *felheart_a = nullptr;
  timespan_t last_he = 0_ms;

  timespan_t cached_at = timespan_t::min();
  int cached_rm = 0;
  player_t* lowest_rm = nullptr;
  double mana_spent = 0.0;
  int extra_rm_gen = 0;

  explicit mw_heal_state_t( monk_t* pl ) : p( pl ) {}

  double t( const char* k, double d = 1.0 ) const
  {
    auto it = tune.find( k );
    return it == tune.end() ? d : it->second;
  }

  void reset()
  {
    cached_at = timespan_t::min();
    cached_rm = 0;
    mana_spent = 0.0;
    last_he    = 0_ms;
  }

  dot_t* dot_of( action_t* a, player_t* tg ) const { return a ? a->find_dot( tg ) : nullptr; }
  bool has_rm( player_t* tg ) const
  {
    auto* d = dot_of( rm_hot, tg );
    if ( d && d->is_ticking() )
      return true;
    d = dot_of( rm_hot2, tg );
    return d && d->is_ticking();
  }
  bool has_em( player_t* tg ) const
  {
    auto* d = dot_of( em_hot, tg );
    if ( d && d->is_ticking() )
      return true;
    d = dot_of( em_hot2, tg );
    return d && d->is_ticking();
  }

  int rm_count()
  {
    if ( cached_at == p->sim->current_time() )
      return cached_rm;
    int n      = 0;
    double low = 2.0;
    lowest_rm  = nullptr;
    for ( auto* tg : p->sim->healing_no_pet_list )
      if ( !tg->is_sleeping() && has_rm( tg ) )
      {
        n++;
        if ( tg->resources.pct( RESOURCE_HEALTH ) < low )
        {
          low       = tg->resources.pct( RESOURCE_HEALTH );
          lowest_rm = tg;
        }
      }
    cached_at = p->sim->current_time();
    cached_rm = n;
    return n;
  }

  // most injured allies first
  std::vector<player_t*> injured( size_t n, player_t* skip = nullptr, bool need_hurt = true ) const
  {
    std::vector<player_t*> l;
    for ( auto* tg : p->sim->healing_no_pet_list )
      if ( !tg->is_sleeping() && tg != skip && ( !need_hurt || tg->resources.pct( RESOURCE_HEALTH ) < 1.0 ) )
        l.push_back( tg );
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

  // most injured ally that does not carry the given HoT yet
  player_t* most_injured_without( action_t* hot, player_t* skip = nullptr ) const
  {
    player_t* best = nullptr;
    double bp      = 2.0;
    for ( auto* tg : p->sim->healing_no_pet_list )
    {
      if ( tg->is_sleeping() || tg == skip )
        continue;
      if ( hot == rm_hot ? has_rm( tg ) : ( hot == em_hot ? has_em( tg ) : false ) )
        continue;
      double pc = tg->resources.pct( RESOURCE_HEALTH );
      if ( pc < bp )
      {
        bp   = pc;
        best = tg;
      }
    }
    return best;
  }

  player_t* random_ally( player_t* skip = nullptr ) const
  {
    std::vector<player_t*> l;
    for ( auto* tg : p->sim->healing_no_pet_list )
      if ( !tg->is_sleeping() && tg != skip )
        l.push_back( tg );
    return l.empty() ? nullptr : l[ p->rng().range( l.size() ) ];
  }

  // a tuning value with its own default for the fistweaving build
  double tf( const char* k, double raid, double fistv ) const { return t( k, fist ? fistv : raid ); }

  void spiritfont_proc( bool force = false )
  {
    if ( !spiritfont.ok() || !sf_soothing_a || ( !force && !p->rng().roll( tf( "sf_chance", 0.13, 0.05 ) ) ) )
      return;
    double amt = ( fist ? 15.1 : 15300.0 / 3532.0 ) * p->cache.spell_power( SCHOOL_NATURE ) * tf( "sf_cocoon", 1.4, 1.0 );
    for ( auto* tg : injured( (size_t)tf( "sf_ncoc", 10, 3 ), nullptr, false ) )
      sf_cocoon_a->execute_on_target( tg, amt );
    int n = (int)tf( "sf_nsm", 34, 19 );
    for ( int i = 0; i < n; i++ )
      if ( auto* tg = most_injured() )
        sf_soothing_a->execute_on_target( tg );
  }

  // Ancient Teachings and Way of the Crane: what one strike of a kick does for the group
  // kinds: 0 Rising Sun Kick, 1 Tiger Palm, 2 Blackout Kick, 3 Spinning Crane Kick tick, 4 Jadefire Stomp
  void after_hit( int kind, double amt, double nhit )
  {
    if ( !fist )
      return;
    double one = amt / std::max( nhit, 1.0 );
    if ( kind == 3 )
    {
      int hits = std::max( 1, (int)std::lround( nhit ) );
      for ( int i = 0; i < hits; i++ )
        if ( auto* tg = random_ally() )
          wotc_a->execute_on_target( tg, one * t( "wotc", 2.7 ) );
      return;
    }
    double k = kind == 4 ? t( "at_jf", 0.2 ) : t( "at", 0.82 );
    int reps = kind == 4 ? std::max( 1, (int)std::lround( nhit ) ) : 1;
    for ( int r = 0; r < reps; r++ )
      for ( auto* tg : injured( (size_t)t( "at_n", 3 ) ) )
        if ( tg->resources.pct( RESOURCE_HEALTH ) < t( "at_thr", 1.0 ) )
          at_k[ kind ]->execute_on_target( tg, one * k );
  }

  // Crane Style / Invoke Chi-Ji gusts when a kick is cast
  void kick_gusts( double crane_style_chance )
  {
    if ( !fist )
      return;
    if ( crane_style.ok() && p->rng().roll( crane_style_chance ) )
      for ( auto* tg : injured( (size_t)t( "cs_n", 2 ), nullptr, false ) )
        gust_a->execute_on_target( tg );
    if ( chiji_b && chiji_b->check() )
    {
      auto l = injured( 5, nullptr, false );
      if ( l.empty() )
        return;
      int n = (int)std::lround( t( "chiji_n", 12.6 ) );
      for ( int i = 0; i < n; i++ )
        chiji_gust_a->execute_on_target( l[ i % l.size() ] );
    }
  }

  // multiplier on everything the monk heals on one target
  double target_mult( player_t* tg ) const
  {
    double m = 1.0;
    if ( has_em( tg ) )
      m *= 1.0 + t( "em_bonus", 0.10 );
    if ( lotus_infusion.ok() && has_rm( tg ) )
      m *= 1.0 + lotus_infusion->effectN( 1 ).percent();
    return m;
  }
};

using namespace actions;  // wowhub
namespace mwheal

{
// Base of every Mistweaver heal. Derived from monk_heal_t so the spec aura and talent passives of the monk are parsed
// onto it by the engine; only what the engine cannot parse is added here.
struct mh_t : public monk_heal_t
{
  double eff        = 1.0;
  bool use_target   = true;  // Enveloping Mist / Lotus Infusion on the target
  bool use_mastery_gust = false;

  mh_t( monk_t* pl, std::string_view n, const spell_data_t* s ) : monk_heal_t( pl, n, s )
  {
    harmful = false;
    ap_type = attack_power_type::NONE;
  }

  mw_heal_state_t& st() const { return *p()->mw; }

  double composite_da_multiplier( const action_state_t* s ) const override
  {
    double m = monk_heal_t::composite_da_multiplier( s ) * eff;
    if ( use_target )
      m *= st().target_mult( s->target );
    return m;
  }
  double composite_ta_multiplier( const action_state_t* s ) const override
  {
    double m = monk_heal_t::composite_ta_multiplier( s ) * eff;
    if ( use_target )
      m *= st().target_mult( s->target );
    return m;
  }
};

// A heal fired by our own code at a chosen target (procs, pulses).
struct simple_heal_t : public mh_t
{
  simple_heal_t( monk_t* pl, std::string_view n, unsigned id ) : mh_t( pl, n, pl->find_spell( id ) )
  {
    background = proc = true;
    auto_target       = false;
    aoe               = 0;
    dot_duration      = 0_ms;
    base_tick_time    = 0_ms;
    spell_power_mod.tick = 0;
    channeled         = false;
  }
};

// A shield with a fixed amount handed in by the caller (Life Cocoon, Chi Cocoon, Yu'lon's Grace).
struct flat_absorb_t : public absorb_t
{
  flat_absorb_t( monk_t* pl, std::string_view n, unsigned id ) : absorb_t( n, pl, pl->find_spell( id ) )
  {
    background = proc = true;
    may_crit = callbacks = false;
    harmful              = false;
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

// Gust of Mists (mastery): Renewing Mist and Enveloping Mist heal their primary target.
struct gust_t : public mh_t
{
  gust_t( monk_t* pl ) : mh_t( pl, "gust_of_mists", pl->find_spell( 191894 ) )
  {
    background = proc = true;
    auto_target       = false;
    spell_power_mod.direct = 1.0;
    base_dd_min = base_dd_max = 0;
    eff = pl->mw ? pl->mw->tf( "gust", 1.0, 0.8 ) : 1.0;
  }
  double composite_da_multiplier( const action_state_t* s ) const override
  {
    return mh_t::composite_da_multiplier( s ) * p()->cache.mastery_value();
  }
};

// Renewing Mist HoT (the cast only starts it)
struct rm_hot_t : public mh_t
{
  bool shortv;
  rm_hot_t( monk_t* pl, bool sv = false ) : mh_t( pl, sv ? "renewing_mist_rapid" : "renewing_mist_hot", pl->find_spell( 119611 ) ), shortv( sv )
  {
    background = proc = true;
    auto_target       = false;
    dot_behavior      = dot_behavior_e::DOT_REFRESH_DURATION;
    hasted_ticks      = true;
    tick_may_crit     = true;
    eff               = pl->mw->tf( "rm", 1.0, 3.5 );
    if ( shortv )
      dot_duration = timespan_t::from_seconds( pl->mw->rapid_diffusion.ok() ? pl->mw->rapid_diffusion->effectN( 1 ).base_value() : 6.0 );
    else if ( pl->mw->lotus_infusion.ok() )
      dot_duration += timespan_t::from_millis( pl->mw->lotus_infusion->effectN( 2 ).base_value() );
    ( shortv ? pl->mw->rm_hot2 : pl->mw->rm_hot ) = this;
  }

  double composite_ta_multiplier( const action_state_t* a ) const override
  {
    double m = mh_t::composite_ta_multiplier( a );
    auto& s  = st();
    int n    = s.rm_count();
    if ( s.misty_coalescence.ok() )
    {
      double cap = s.misty_coalescence->effectN( 2 ).base_value();
      m *= 1.0 + s.misty_coalescence->effectN( 1 ).percent() * std::min( n, (int)cap ) / cap * s.t( "coal", 1.0 );
    }
    if ( s.mistline.ok() && s.lowest_rm == a->target )
      m *= 1.0 + s.mistline->effectN( 1 ).percent() * s.t( "mistline", 1.0 );
    return m;
  }

  void tick( dot_t* d ) override
  {
    mh_t::tick( d );
    auto& s = st();
    if ( s.misty_peaks.ok() && rng().roll( s.t( "peaks", 0.06 ) ) )
      s.em_hot2->execute_on_target( d->target );
    // Renewing Mist moves on when it heals past full health
    if ( d->state->result_total > d->state->result_amount * 1.0 + 1.0 && d->remains() > 1_s )
    {
      player_t* from = d->target;
      player_t* to   = s.most_injured_without( this, from );
      if ( to && to->resources.pct( RESOURCE_HEALTH ) < s.t( "rm_jump_pct", 0.95 ) )
      {
        timespan_t left = d->remains();
        d->cancel();
        execute_on_target( to );
        if ( auto* nd = find_dot( to ) )
          if ( nd->is_ticking() && nd->remains() > left )
            nd->adjust_duration( left - nd->remains(), timespan_t::min(), 0u );
      }
    }
    if ( s.zen_pulse.ok() && rng().roll( s.zen_pulse->effectN( 1 ).percent() * s.t( "zen_proc", 0.1 ) ) )
      s.zen_ready->trigger();
  }
};

struct rm_t : public mh_t
{
  rm_t( monk_t* pl, std::string_view opts ) : mh_t( pl, "renewing_mist", pl->find_spell( 115151 ) )
  {
    parse_options( opts );
    auto_target = false;
    use_target  = false;
    base_dd_min = base_dd_max = 0;
    spell_power_mod.direct    = 0;
    cooldown->hasted          = true;
    travel_speed              = 0;
  }

  player_t* pick_target() override
  {
    auto& s = st();
    if ( auto* tg = s.most_injured_without( s.rm_hot ) )
      return tg;
    return s.most_injured();
  }
  void schedule_execute( action_state_t* st_ = nullptr ) override
  {
    if ( !st_ )
      if ( player_t* tg = pick_target() )
        set_target( tg );
    mh_t::schedule_execute( st_ );
  }

  void execute() override
  {
    mh_t::execute();
    auto& s = st();
    s.rm_hot->execute_on_target( target );
    s.gust_a->execute_on_target( target );
    if ( s.dancing_mists.ok() && rng().roll( s.dancing_mists->effectN( 1 ).percent() ) )
      if ( auto* tg = s.most_injured_without( s.rm_hot, target ) )
        s.rm_hot->execute_on_target( tg );
  }
};

struct em_hot_t : public mh_t
{
  em_hot_t( monk_t* pl, bool shortv = false ) : mh_t( pl, shortv ? "enveloping_mist_peaks" : "enveloping_mist_hot", pl->find_spell( 124682 ) )
  {
    if ( shortv )
      dot_duration = timespan_t::from_seconds( pl->mw->t( "peaks_dur", 4.0 ) );
    background = proc = true;
    auto_target       = false;
    base_dd_min = base_dd_max = 0;
    spell_power_mod.direct    = 0;
    dot_behavior              = dot_behavior_e::DOT_REFRESH_DURATION;
    hasted_ticks              = true;
    tick_may_crit             = true;
    eff                       = pl->mw->t( "em", 1.0 );
    ( shortv ? pl->mw->em_hot2 : pl->mw->em_hot ) = this;
  }
};

struct em_t : public mh_t
{
  em_t( monk_t* pl, std::string_view opts ) : mh_t( pl, "enveloping_mist", pl->find_spell( 124682 ) )
  {
    parse_options( opts );
    use_target = false;
    base_dd_min = base_dd_max = 0;
    spell_power_mod.direct    = 0;
    dot_duration              = 0_ms;
    base_tick_time            = 0_ms;
    spell_power_mod.tick      = 0;
    cooldown->duration        = timespan_t::from_seconds( pl->mw->tf( "em_cd", 9.0, 20.0 ) );
  }
  timespan_t execute_time() const override
  {
    if ( st().tft->check() || ( st().chiji_b && st().chiji_b->check() ) )
      return 0_ms;
    timespan_t t = mh_t::execute_time();
    if ( st().spiritfont_b && st().spiritfont_b->check() )
      t *= 1.0 - st().spiritfont_b->data().effectN( 3 ).percent() * -1.0;
    return t;
  }
  void execute() override
  {
    bool tft_used = st().tft->check();
    mh_t::execute();
    auto& s = st();
    s.em_hot->execute_on_target( target );
    s.gust_a->execute_on_target( target );
    if ( s.rapid_diffusion.ok() )
      if ( auto* tg = s.most_injured_without( s.rm_hot, target ) )
      {
        s.rm_hot2->execute_on_target( tg );
        s.gust_a->execute_on_target( tg );
      }
    if ( tft_used )
      s.tft->decrement();
  }
};

struct viv_t : public mh_t
{
  viv_t( monk_t* pl, std::string_view opts ) : mh_t( pl, "vivify", pl->find_spell( 116670 ) )
  {
    parse_options( opts );
    eff = pl->mw->t( "vivify", 1.95 );
  }
  timespan_t execute_time() const override
  {
    if ( st().viv_free->check() || st().tft->check() )
      return 0_ms;
    return mh_t::execute_time();
  }
  void execute() override
  {
    mh_t::execute();
    auto& s = st();
    if ( s.viv_free->check() )
      s.viv_free->expire();
    if ( s.im_a )
      for ( auto* tg : p()->sim->healing_no_pet_list )
        if ( !tg->is_sleeping() && s.has_rm( tg ) )
          s.im_a->execute_on_target( tg );
    if ( s.mist_caller.ok() )
      s.gust_a->execute_on_target( p() );
    s.spiritfont_proc();
    if ( s.courage_a && p()->rng().roll( s.t( "courage", 0.29 ) ) )
      if ( auto* tg = s.most_injured() )
        s.courage_a->execute_on_target( tg );
    if ( s.zen_ready->check() && s.zen_a )
    {
      s.zen_ready->expire();
      s.zen_a->execute_on_target( target );
      for ( auto* tg : p()->sim->healing_no_pet_list )
        if ( !tg->is_sleeping() && tg != target && s.has_rm( tg ) )
          s.zen_a->execute_on_target( tg );
    }
  }
};

// Rushing Wind Kick: kicks the enemy and heals up to 5 allies that carry one of the monk's HoTs
struct rwk_dmg_t : public monk_spell_t
{
  rwk_dmg_t( monk_t* pl ) : monk_spell_t( pl, "rushing_wind_kick_damage", pl->find_spell( 468179 ) )
  {
    background = dual = true;
    spell_power_mod.direct = data().effectN( 1 ).ap_coeff();
    attack_power_mod.direct = 0;
    ap_type = attack_power_type::NONE;
    base_multiplier         = pl->mw->t( "rwk_dmg", 1.0 );
  }
};

struct rwk_heal_t : public mh_t
{
  rwk_heal_t( monk_t* pl ) : mh_t( pl, "rushing_wind_kick_heal", pl->find_spell( 1269159 ) )
  {
    background = proc = true;
    auto_target       = false;
    eff               = pl->mw->t( "rwk", 1.0 );
    pl->mw->rwk_heal_a = this;
  }
};

struct rwk_t : public monk_spell_t
{
  rwk_t( monk_t* pl, std::string_view opts ) : monk_spell_t( pl, "rushing_wind_kick", pl->find_spell( 467307 ) )
  {
    parse_options( opts );
    base_dd_min = base_dd_max = 0;
    attack_power_mod.direct   = 0;
    ap_type                   = attack_power_type::NONE;
    spell_power_mod.direct    = pl->find_spell( 468179 )->effectN( 1 ).ap_coeff() * pl->mw->t( "rwk_dmg", 1.0 );
    cooldown->duration        = timespan_t::from_seconds( pl->mw->t( "rwk_cd", 5.4 ) );
    cooldown->hasted          = true;
  }
  void execute() override
  {
    monk_spell_t::execute();
    auto& s = *p()->mw;
    // heal up to 5 allies that carry a HoT of the monk, most injured first
    int n = 0;
    std::vector<player_t*> l;
    for ( auto* tg : p()->sim->healing_no_pet_list )
      if ( !tg->is_sleeping() && ( s.has_rm( tg ) || s.has_em( tg ) ) )
        l.push_back( tg );
    std::sort( l.begin(), l.end(), []( player_t* a, player_t* b ) {
      return a->resources.pct( RESOURCE_HEALTH ) < b->resources.pct( RESOURCE_HEALTH );
    } );
    for ( auto* tg : l )
    {
      if ( n++ >= 5 )
        break;
      s.rwk_heal_a->execute_on_target( tg );
    }
    s.spiritfont_proc();
    if ( s.rapid_diffusion.ok() )
      if ( auto* tg = s.most_injured_without( s.rm_hot ) )
      {
        s.rm_hot2->execute_on_target( tg );
        s.gust_a->execute_on_target( tg );
      }
    if ( s.rising_mist.ok() )
    {
      for ( auto* tg : p()->sim->healing_no_pet_list )
      {
        for ( action_t* a : { s.rm_hot, s.rm_hot2, s.em_hot } )
          if ( auto* d = s.dot_of( a, tg ) )
            if ( d->is_ticking() && d->remains() < timespan_t::from_seconds( s.t( "rising_cap", 24.0 ) ) )
              d->adjust_duration( timespan_t::from_seconds( s.t( "rising_ext", 4.0 ) ), timespan_t::min(), 0u, false );
      }
    }
  }
  // the kick needs an enemy
  bool ready() override { return monk_spell_t::ready(); }
};

struct tft_t : public monk_spell_t
{
  tft_t( monk_t* pl, std::string_view opts ) : monk_spell_t( pl, "thunder_focus_tea", pl->find_spell( 116680 ) )
  {
    parse_options( opts );
    harmful = false;
    gcd_type = gcd_haste_type::NONE;
    trigger_gcd = 0_ms;
    // empowers the next spells; the cooldown is reset by the tea itself
    cooldown->duration = timespan_t::from_seconds( pl->mw->tf( "tft_cd", 30.0, 25.0 ) );
  }
  void execute() override
  {
    monk_spell_t::execute();
    p()->mw->tft->trigger( p()->mw->fist ? (int)p()->mw->t( "tft_stacks", 2 ) : 1 );
    if ( p()->mw->spiritfont_b )
      p()->mw->spiritfont_b->trigger();
    if ( p()->mw->fist )
      p()->mw->spiritfont_proc( true );
  }
};

struct mana_tea_t : public monk_spell_t
{
  mana_tea_t( monk_t* pl, std::string_view opts ) : monk_spell_t( pl, "mana_tea", pl->find_spell( 115294 ) )
  {
    parse_options( opts );
    harmful = false;
    channeled = true;
    tick_zero = false;
    base_tick_time = 500_ms;
    dot_duration   = 1.5_s;
    hasted_ticks   = true;
    base_costs[ RESOURCE_MANA ] = 0;
  }
  bool ready() override
  {
    if ( st().mana_tea_stacks->check() < 3 )
      return false;
    return monk_spell_t::ready();
  }
  mw_heal_state_t& st() const { return *p()->mw; }
  void tick( dot_t* d ) override
  {
    monk_spell_t::tick( d );
    if ( st().mana_tea_stacks->check() > 0 )
    {
      st().mana_tea_stacks->decrement();
      p()->resource_gain( RESOURCE_MANA, p()->resources.base[ RESOURCE_MANA ] * 0.0288 * st().t( "mana_tea_mult", 1.0 ),
                          p()->get_gain( "mana_tea_mw" ) );
    }
  }
};

struct yulon_t : public monk_spell_t
{
  yulon_t( monk_t* pl, std::string_view opts ) : monk_spell_t( pl, "invoke_yulon", pl->find_spell( 322118 ) )
  {
    parse_options( opts );
    harmful = false;
  }
  void execute() override
  {
    monk_spell_t::execute();
    p()->mw->yulon_b->trigger();
  }
};

struct revival_t : public mh_t
{
  revival_t( monk_t* pl, std::string_view opts ) : mh_t( pl, "revival", pl->find_spell( 115310 ) )
  {
    parse_options( opts );
    aoe         = -1;
    eff         = pl->mw->tf( "revival", 0.21, 1.0 );
    auto_target = true;
    if ( pl->mw->fist )
      cooldown->duration = timespan_t::from_seconds( pl->mw->t( "revival_cd", 400.0 ) );
  }
};

struct cocoon_t : public mh_t
{
  cocoon_t( monk_t* pl, std::string_view opts ) : mh_t( pl, "life_cocoon", pl->find_spell( 116849 ) )
  {
    parse_options( opts );
    trigger_gcd = 0_ms;
    base_dd_min = base_dd_max = 0;
    spell_power_mod.direct    = 0;
    use_target                = false;
    cooldown->duration        = timespan_t::from_seconds( pl->mw->tf( "cocoon_cd", 80.0, 100.0 ) );
  }
  void execute() override
  {
    mh_t::execute();
    auto& s = *p()->mw;
    double amt = p()->resources.max[ RESOURCE_HEALTH ] * data().effectN( 3 ).percent() *
                 ( 1.0 + p()->cache.heal_versatility() ) * s.tf( "cocoon", 1.15, 1.8 );
    s.cocoon_a->execute_on_target( s.most_injured(), amt );
  }
};

struct conduit_t : public monk_spell_t
{
  conduit_t( monk_t* pl, std::string_view opts ) : monk_spell_t( pl, "celestial_conduit", pl->find_spell( 443028 ) )
  {
    parse_options( opts );
    harmful = false;
    channeled = true;
    tick_zero = true;
    base_tick_time = 1_s;
    dot_duration   = 4_s;
    hasted_ticks   = true;
    if ( pl->mw->fist )
      cooldown->duration = timespan_t::from_seconds( pl->mw->t( "conduit_cd", 145.0 ) );
  }
  void tick( dot_t* d ) override
  {
    monk_spell_t::tick( d );
    auto& s = *p()->mw;
    int n   = 0;
    for ( auto* tg : s.injured( (size_t)s.t( "conduit_n", 19 ) ) )
    {
      (void)n;
      s.conduit_heal_a->execute_on_target( tg );
    }
  }
};

struct simple_cast_t : public monk_spell_t
{
  simple_cast_t( monk_t* pl, std::string_view n, unsigned id, std::string_view opts ) : monk_spell_t( pl, n, pl->find_spell( id ) )
  {
    parse_options( opts );
    harmful = false;
  }
};


// ==========================================================================
// Fistweaving build: kicks that feed Ancient Teachings / Way of the Crane / Crane Style / Invoke Chi-Ji
// ==========================================================================

// One damage strike of a kick; the group heals it causes are handed to the state (after_hit).
struct hit_t : public monk_spell_t
{
  int kind;
  double n = 1.0;  // enemies hit at once (the damage is scaled, one enemy is simulated)
  double fit;
  hit_t( monk_t* pl, std::string_view nm, unsigned dmg_id, int k, double f ) : monk_spell_t( pl, nm, pl->find_spell( dmg_id ) ), kind( k ), fit( f )
  {
    background = true;
    base_dd_min = base_dd_max = 0;
    attack_power_mod.direct   = 0;
    ap_type                   = attack_power_type::NONE;
    spell_power_mod.direct    = pl->find_spell( dmg_id )->effectN( 1 ).ap_coeff();
    base_costs[ RESOURCE_ENERGY ] = base_costs[ RESOURCE_CHI ] = base_costs[ RESOURCE_MANA ] = 0;
  }
  mw_heal_state_t& st() const { return *p()->mw; }
  double composite_da_multiplier( const action_state_t* s ) const override
  {
    double m = monk_spell_t::composite_da_multiplier( s ) * n * fit;
    if ( kind == 0 && st().morning_breeze.ok() )  // Morning Breeze: Rising Sun Kick damage up by the mastery rating (fitted 0.0156% a point)
      m *= 1.0 + p()->composite_mastery_rating() * st().t( "mb", 0.000156 );
    return m;
  }
  void impact( action_state_t* s ) override
  {
    monk_spell_t::impact( s );
    st().after_hit( kind, s->result_amount, n );
  }
};

struct kick_base_t : public monk_spell_t
{
  kick_base_t( monk_t* pl, std::string_view nm, unsigned id, std::string_view opts ) : monk_spell_t( pl, nm, pl->find_spell( id ) )
  {
    parse_options( opts );
    base_dd_min = base_dd_max = 0;
    attack_power_mod.direct   = 0;
    spell_power_mod.direct    = 0;
    ap_type                   = attack_power_type::NONE;
    base_costs[ RESOURCE_ENERGY ] = base_costs[ RESOURCE_CHI ] = 0;
  }
  mw_heal_state_t& st() const { return *p()->mw; }
  void courage()
  {
    auto& s = st();
    if ( s.courage_a && rng().roll( s.t( "courage_f", 0.0 ) ) )
      if ( auto* tg = s.most_injured() )
        s.courage_a->execute_on_target( tg );
  }
};

struct f_rsk_t : public kick_base_t
{
  f_rsk_t( monk_t* pl, std::string_view opts ) : kick_base_t( pl, "rising_sun_kick", 107428, opts )
  {
    cooldown->duration = timespan_t::from_seconds( pl->mw->t( "rsk_cd", 8.0 ) );
    cooldown->hasted   = true;
    pl->mw->cd_rsk     = cooldown;
  }
  void execute() override
  {
    kick_base_t::execute();
    auto& s       = st();
    bool stomp    = s.emperors_elixir.ok() && s.tft->check();
    if ( stomp )
      s.tft->decrement();
    s.hit_rsk->execute_on_target( target );
    if ( rng().roll( s.t( "rsk_again", 0.35 ) ) )
      cooldown->reset( true );
    if ( stomp || ( s.emperors_elixir.ok() && rng().roll( s.t( "jf_extra", 0.09 ) ) ) )
      s.hit_jf->execute_on_target( target );
    s.kick_gusts( 1.0 );
    s.spiritfont_proc();
  }
};

struct f_tp_t : public kick_base_t
{
  f_tp_t( monk_t* pl, std::string_view opts ) : kick_base_t( pl, "tiger_palm", 100780, opts ) {}
  void execute() override
  {
    kick_base_t::execute();
    auto& s = st();
    int hits = s.way_of_the_crane.ok() ? 2 : 1;
    for ( int i = 0; i < hits; i++ )
      s.hit_tp->execute_on_target( target );
    s.totm->trigger();
    courage();
    s.kick_gusts( s.t( "cs_tp", 0.02 ) );
  }
};

struct f_bok_t : public kick_base_t
{
  f_bok_t( monk_t* pl, std::string_view opts ) : kick_base_t( pl, "blackout_kick", 100784, opts )
  {
    cooldown->duration = timespan_t::from_seconds( pl->mw->t( "bok_cd", 3.0 ) );
    cooldown->hasted   = true;
  }
  void execute() override
  {
    kick_base_t::execute();
    auto& s     = st();
    int strikes = 1 + s.totm->check();
    bool had    = s.totm->check() > 0;
    s.totm->expire();
    for ( int i = 0; i < strikes; i++ )
      s.hit_bok->execute_on_target( target );
    if ( had && s.cd_rsk && rng().roll( s.t( "rsk_reset", 0.12 ) ) )
      s.cd_rsk->reset( true );
    s.kick_gusts( s.t( "cs_bok", 0.66 ) );
  }
};

struct f_sck_t : public kick_base_t
{
  f_sck_t( monk_t* pl, std::string_view opts ) : kick_base_t( pl, "spinning_crane_kick", 101546, opts )
  {
    channeled      = true;
    tick_zero      = false;
    harmful        = true;
    base_tick_time = 500_ms;
    dot_duration   = timespan_t::from_seconds( pl->mw->t( "sck_dur", 1.35 ) );
    cooldown->duration = timespan_t::from_seconds( pl->mw->t( "sck_cd", 8.5 ) );
    hasted_ticks   = true;
  }
  void execute() override
  {
    kick_base_t::execute();
    st().kick_gusts( st().t( "cs_sck", 0.48 ) );
  }
  void tick( dot_t* d ) override
  {
    kick_base_t::tick( d );
    st().hit_sck->execute_on_target( target );
  }
};

struct f_sg_t : public mh_t
{
  f_sg_t( monk_t* pl, std::string_view opts ) : mh_t( pl, "sheiluns_gift", pl->find_spell( 399491 ) )
  {
    parse_options( opts );
    use_target = false;
    base_dd_min = base_dd_max = 0;
    spell_power_mod.direct    = 0;
    aoe                       = 0;
    cooldown->duration        = timespan_t::from_seconds( pl->mw->t( "sg_cd", 34.0 ) );
  }
  void execute() override
  {
    mh_t::execute();
    auto& s = st();
    auto l  = s.injured( 3, nullptr, false );
    for ( size_t i = 0; i < l.size(); i++ )
      ( i == 0 ? s.sg_main_a : s.sg_other_a )->execute_on_target( l[ i ] );
    if ( s.courage_a && rng().roll( s.t( "courage_f", 0.0 ) ) )
      if ( auto* tg = s.most_injured() )
        s.courage_a->execute_on_target( tg );
  }
};

struct chiji_t : public monk_spell_t
{
  chiji_t( monk_t* pl, std::string_view opts ) : monk_spell_t( pl, "invoke_chiji", pl->find_spell( 325197 ) )
  {
    parse_options( opts );
    harmful = false;
    base_dd_min = base_dd_max = 0;
    cooldown->duration = timespan_t::from_seconds( pl->mw->t( "chiji_cd", 72.0 ) );
  }
  void execute() override
  {
    monk_spell_t::execute();
    auto& s = *p()->mw;
    s.chiji_b->trigger();
    double amt = 26.6 * p()->cache.spell_power( SCHOOL_NATURE ) * s.t( "chiji_coc", 0.97 );
    for ( auto* tg : s.injured( 5, nullptr, false ) )
      s.chiji_coc_a->execute_on_target( tg, amt );
  }
};

struct cgust_t : public mh_t
{
  cgust_t( monk_t* pl ) : mh_t( pl, "chiji_gust_of_mists", pl->find_spell( 343819 ) )
  {
    background = proc = true;
    auto_target       = false;
    spell_power_mod.direct = 1.0;
    base_dd_min = base_dd_max = 0;
    eff = pl->mw->t( "cgust", 0.5 );
  }
  double composite_da_multiplier( const action_state_t* s ) const override
  {
    return mh_t::composite_da_multiplier( s ) * p()->cache.mastery_value();
  }
};

}  // namespace mwheal

// ==========================================================================
// Hooks
// ==========================================================================
bool monk_t::mw_heal_mode() const
{
  return specialization() == MONK_MISTWEAVER && primary_role() == ROLE_HEAL && sim->heal_target;
}

void monk_t::mw_heal_reset()
{
  if ( mw )
    mw->reset();
}

void monk_t::init_mw_heal()
{
  mw      = new mw_heal_state_t( this );
  auto& s = *mw;
  // tuning fits: "name=value/name=value"
  for ( auto& kv : util::string_split<std::string_view>( mw_tune, "/" ) )
  {
    auto pr = util::string_split<std::string_view>( kv, "=" );
    if ( pr.size() == 2 )
      s.tune[ std::string( pr[ 0 ] ) ] = util::to_double( pr[ 1 ] );
  }
  auto ST = [ this ]( std::string_view n ) { return find_talent_spell( talent_tree::SPECIALIZATION, n, MONK_MISTWEAVER ); };
  auto CT = [ this ]( std::string_view n ) { return find_talent_spell( talent_tree::CLASS, n, MONK_MISTWEAVER ); };
  auto HT = [ this ]( std::string_view n ) { return find_talent_spell( talent_tree::HERO, n ); };
  s.invigorating_mists   = ST( "Invigorating Mists" );
  s.rapid_diffusion      = ST( "Rapid Diffusion" );
  s.dancing_mists        = ST( "Dancing Mists" );
  s.jade_bond            = ST( "Jade Bond" );
  s.misty_peaks          = ST( "Misty Peaks" );
  s.focused_thunder      = ST( "Focused Thunder" );
  s.rising_mist          = ST( "Rising Mist" );
  s.lotus_infusion       = ST( "Lotus Infusion" );
  s.deep_clarity         = ST( "Deep Clarity" );
  s.mistline             = ST( "Mistline" );
  s.secret_infusion      = ST( "Secret Infusion" );
  s.celestial_harmony    = ST( "Celestial Harmony" );
  s.invoke_yulon         = ST( "Invoke Yu'lon, the Jade Serpent" );
  s.revival_t            = ST( "Revival" );
  s.mana_tea_t           = ST( "Mana Tea" );
  s.thunder_focus_tea_t  = ST( "Thunder Focus Tea" );
  s.enveloping_mist_t    = ST( "Enveloping Mist" );
  s.rushing_wind_kick_t  = ST( "Rushing Wind Kick" );
  s.spiritfont           = ST( "Spiritfont" );
  s.life_cocoon_t        = ST( "Life Cocoon" );
  s.zen_pulse            = ST( "Zen Pulse" );
  s.misty_coalescence    = ST( "Misty Coalescence" );
  s.way_of_the_serpent   = ST( "Way of the Serpent" );
  s.mist_wrap            = ST( "Mist Wrap" );
  s.mist_caller          = CT( "Mist Caller" );
  s.vital_clarity        = CT( "Vital Clarity" );
  s.silent_sanctuary     = CT( "Silent Sanctuary" );
  s.chi_warding          = CT( "Chi Warding" );
  s.vivacious            = CT( "Vivacious Vivification" );
  s.yulons_grace         = CT( "Yu'lon's Grace" );
  s.soothing_mist_t      = CT( "Soothing Mist" );
  s.chi_wave_t           = CT( "Chi Wave" );
  s.celestial_conduit_t        = HT( "Celestial Conduit" );
  s.courage_of_the_white_tiger = HT( "Courage of the White Tiger" );
  s.strength_of_the_black_ox   = HT( "Strength of the Black Ox" );
  s.heart_of_the_jade_serpent  = HT( "Heart of the Jade Serpent" );
  s.yulons_avatar              = HT( "Yu'lon's Avatar" );
  s.flowing_wisdom             = HT( "Flowing Wisdom" );
  s.unity_within               = HT( "Unity Within" );
  s.chijis_swiftness           = HT( "Chi-Ji's Swiftness" );
  s.jade_sanctuary             = HT( "Jade Sanctuary" );
  s.way_of_the_crane   = ST( "Way of the Crane" );
  s.jadefire_teachings = ST( "Jadefire Teachings" );
  s.crane_style        = ST( "Crane Style" );
  s.invoke_chiji_t     = ST( "Invoke Chi-Ji, the Red Crane" );
  s.sheiluns_gift_t    = ST( "Sheilun's Gift" );
  s.emperors_elixir    = ST( "Emperor's Elixir" );
  s.morning_breeze     = ST( "Morning Breeze" );
  s.xuens_guidance     = HT( "Xuen's Guidance" );
  s.healing_elixir_t   = ST( "Healing Elixir" );
  s.mists_of_life      = ST( "Mists of Life" );
  s.fist               = s.way_of_the_crane.ok() || s.jadefire_teachings.ok();
}

void monk_t::create_mw_heal_actions()
{
  auto& s = *mw;
  s.tft          = make_buff( this, "tft_mw", find_spell( 116680 ) )->set_max_stack( s.fist ? 2 : 1 )->set_duration( 30_s );
  s.viv_free     = make_buff( this, "vivacious_mw", find_spell( 392883 ) )->set_max_stack( 1 );
  s.zen_ready    = make_buff( this, "zen_pulse_mw", find_spell( 446334 ) )->set_max_stack( 1 )->set_duration( 120_s );
  s.yulon_b      = make_buff( this, "yulon_mw", find_spell( 322118 ) )->set_duration( 25_s );
  s.mana_tea_stacks = make_buff( this, "mana_tea_stacks_mw", find_spell( 115867 ) )->set_max_stack( 20 )->set_duration( 3600_s );
  if ( s.spiritfont.ok() )
    s.spiritfont_b = make_buff( this, "spiritfont_mw", find_spell( 1260565 ) )->set_max_stack( 1 );

  if ( s.fist )
  {
    s.totm    = make_buff( this, "totm_mw", find_spell( 202090 ) )->set_max_stack( 4 )->set_duration( 120_s );
    s.chiji_b = make_buff( this, "chiji_mw", find_spell( 325197 ) )->set_max_stack( 1 )->set_duration( timespan_t::from_seconds( s.t( "chiji_dur", 12.5 ) ) );
  }

  s.rm_hot = new mwheal::rm_hot_t( this );
  s.rm_hot2 = new mwheal::rm_hot_t( this, true );
  s.em_hot = new mwheal::em_hot_t( this );
  s.em_hot2 = new mwheal::em_hot_t( this, true );
  s.gust_a = new mwheal::gust_t( this );
  if ( s.invigorating_mists.ok() )
  {
    auto* a = new mwheal::simple_heal_t( this, "invigorating_mists", 425804 );
    a->eff  = s.t( "im", 1.0 );
    s.im_a  = a;
  }
  if ( s.zen_pulse.ok() )
  {
    auto* a = new mwheal::simple_heal_t( this, "zen_pulse", 198487 );
    a->eff  = s.t( "zen", 1.42 );
    s.zen_a = a;
  }
  {
    auto* a = new mwheal::simple_heal_t( this, "soothing_breath", 343737 );
    a->spell_power_mod.direct = 0.7;
    a->eff  = s.t( "yulon", 8.25 );
    s.soothing_breath_a = a;
  }
  {
    auto* a = new mwheal::simple_heal_t( this, "celestial_conduit_heal", 443039 );
    a->eff  = s.tf( "conduit", 0.174, 1.46 );
    s.conduit_heal_a = a;
  }
  new mwheal::rwk_heal_t( this );
  s.cocoon_a = new mwheal::flat_absorb_t( this, "life_cocoon_shield", 116849 );
  s.sf_cocoon_a = new mwheal::flat_absorb_t( this, "chi_cocoon", 116849 );
  {
    auto* a = new mwheal::simple_heal_t( this, "spiritfont_soothing", 115175 );
    a->spell_power_mod.direct = 0.82 * s.tf( "sf_soothing", 1.0, 5.0 );
    a->use_target = false;
    s.sf_soothing_a = a;
  }
  if ( s.courage_of_the_white_tiger.ok() )
  {
    auto* a = new mwheal::simple_heal_t( this, "courage_claw_heal", 443106 );
    a->spell_power_mod.direct = 18.4 * s.tf( "courage_eff", 1.0, 1.29 );
    a->use_target = false;
    s.courage_a = a;
  }
  if ( s.silent_sanctuary.ok() )
  {
    auto* a = new mwheal::simple_heal_t( this, "silent_sanctuary", 1266720 );
    a->eff = s.t( "sanctuary", 1.5 );
    a->use_target = false;
    s.sanctuary_heal_a = a;
  }

  if ( s.fist )
  {
    auto flat = [ & ]( std::string_view n, unsigned id, bool crit ) {
      auto* a = new mwheal::simple_heal_t( this, n, id );
      a->spell_power_mod.direct = 0;
      a->use_target             = false;
      a->may_crit               = crit;
      return a;
    };
    s.at_a   = flat( "ancient_teachings", 388024, false );
    for ( int i = 0; i < 5; i++ )
      s.at_k[ i ] = flat( std::string( "ancient_teachings_" ) + ( i == 0 ? "rsk" : i == 1 ? "tp" : i == 2 ? "bok" : i == 3 ? "sck" : "jadefire" ), 388024, false );
    s.wotc_a = flat( "way_of_the_crane_heal", 389325, false );
    s.he_a   = flat( "healing_elixir", 428439, true );
    {
      auto* a = new mwheal::simple_heal_t( this, "yulons_grace_heal", 414143 );
      a->spell_power_mod.direct = s.t( "grace_sp", 7.8 );
      a->use_target             = false;
      s.grace_a                 = a;
    }
    if ( s.strength_of_the_black_ox.ok() )
    {
      auto* a = new mwheal::simple_heal_t( this, "strength_of_the_black_ox_heal", 443113 );
      a->spell_power_mod.direct = s.t( "ox_sp", 4.5 );
      a->use_target             = false;
      s.ox_a                    = a;
    }
    if ( s.chi_warding.ok() )
    {
      auto* a = new mwheal::simple_heal_t( this, "chi_warding_heal", 1277446 );
      a->spell_power_mod.direct = s.t( "ward_sp", 6.3 );
      a->use_target             = false;
      s.ward_a                  = a;
    }
    // Unstable Felheart Crystal (250255), an on-use shield
    if ( find_item_by_id( 250255 ) )
      s.felheart_a = new mwheal::flat_absorb_t( this, "unstable_felheart_crystal", 1255200 );
    s.chiji_gust_a = new mwheal::cgust_t( this );
    s.chiji_coc_a  = new mwheal::flat_absorb_t( this, "chiji_chi_cocoon", 116849 );
    {
      auto* a = new mwheal::simple_heal_t( this, "sheiluns_gift_main", 399491 );
      a->spell_power_mod.direct = s.t( "sg_main", 41.0 );
      s.sg_main_a               = a;
      auto* b = new mwheal::simple_heal_t( this, "sheiluns_gift_other", 399491 );
      b->spell_power_mod.direct = s.t( "sg_other", 7.4 );
      s.sg_other_a              = b;
    }
    s.hit_rsk = new mwheal::hit_t( this, "rising_sun_kick_hit", 185099, 0, s.t( "rsk_dmg", 1.72 ) );
    s.hit_tp  = new mwheal::hit_t( this, "tiger_palm_hit", 100780, 1, s.t( "tp_dmg", 1.13 ) );
    s.hit_bok = new mwheal::hit_t( this, "blackout_kick_hit", 228649, 2, s.t( "bok_dmg", 0.61 ) );
    {
      auto* h = new mwheal::hit_t( this, "spinning_crane_kick_hit", 107270, 3, s.t( "sck_dmg", 2.45 ) );
      h->n    = s.t( "sck_n", 7.5 );
      s.hit_sck = h;
      auto* j = new mwheal::hit_t( this, "jadefire_stomp_hit", 1248815, 4, s.t( "jf_dmg", 1.0 ) );
      j->n    = s.t( "jf_n", 4.0 );
      s.hit_jf = j;
    }
  }

  register_combat_begin( [ this ]( player_t* ) {
    auto& st0 = *mw;
    if ( st0.soothing_breath_a )
      make_repeating_event( *sim, timespan_t::from_seconds( st0.t( "yulon_int", 1.15 ) ), [ this ] {
        auto& st = *mw;
        if ( st.yulon_b->check() )
          for ( auto* tg : st.injured( 3, nullptr, false ) )
            st.soothing_breath_a->execute_on_target( tg );
      } );
    if ( st0.sanctuary_heal_a )
      make_repeating_event( *sim, timespan_t::from_seconds( st0.t( "sanct_int", 7.5 ) ), [ this ] {
        mw->sanctuary_heal_a->execute_on_target( this );
      } );
    if ( st0.yulons_grace_a )
      make_repeating_event( *sim, timespan_t::from_seconds( st0.t( "grace_int", 2.2 ) ), [ this ] {
        mw->yulons_grace_a->execute_on_target( this, resources.max[ RESOURCE_HEALTH ] * 0.0087 );
      } );
    if ( st0.fist )
    {
      // Healing Elixir: one every so often, drunk when the monk is hurt
      make_repeating_event( *sim, 1_s, [ this ] {
        auto& st = *mw;
        if ( sim->current_time() - st.last_he >= timespan_t::from_seconds( st.t( "he_int", 38.6 ) ) &&
             resources.pct( RESOURCE_HEALTH ) < st.t( "he_thr", 0.8 ) )
        {
          st.last_he = sim->current_time();
          st.he_a->execute_on_target( this, resources.max[ RESOURCE_HEALTH ] * st.t( "he_pct", 0.154 ) );
        }
      } );
      if ( st0.felheart_a )
        make_repeating_event( *sim, timespan_t::from_seconds( st0.t( "felheart_int", 135.0 ) ), [ this ] {
          if ( auto* tg = mw->most_injured() )
            mw->felheart_a->execute_on_target( tg, mw->t( "felheart_amt", 780000.0 ) );
        } );
      if ( st0.ox_a )
        make_repeating_event( *sim, timespan_t::from_seconds( st0.t( "ox_int", 7.0 ) ), [ this ] {
          if ( auto* tg = mw->most_injured() )
            mw->ox_a->execute_on_target( tg );
        } );
      if ( st0.ward_a )
        make_repeating_event( *sim, timespan_t::from_seconds( st0.t( "ward_int", 13.4 ) ), [ this ] {
          mw->ward_a->execute_on_target( this );
        } );
      if ( st0.yulons_grace.ok() )
        make_repeating_event( *sim, timespan_t::from_seconds( st0.t( "grace_int", 7.2 ) ), [ this ] {
          mw->grace_a->execute_on_target( this );
        } );
    }
    // Mana Tea: one stack per slice of mana spent
    make_repeating_event( *sim, 1_s, [ this ] {
      auto& st      = *mw;
      double lost   = iteration_resource_lost[ RESOURCE_MANA ];
      double slice  = resources.max[ RESOURCE_MANA ] * st.t( "tea_slice", 0.08 );
      while ( lost - st.mana_spent >= slice )
      {
        st.mana_spent += slice;
        st.mana_tea_stacks->trigger();
        if ( rng().roll( cache.attack_crit_chance() ) )
          st.mana_tea_stacks->trigger();
      }
    } );
  } );
}

action_t* monk_t::create_action_mw_heal( std::string_view name, std::string_view options_str )
{
  if ( name == "renewing_mist" )
    return new mwheal::rm_t( this, options_str );
  if ( name == "vivify" )
    return new mwheal::viv_t( this, options_str );
  if ( name == "enveloping_mist" )
    return new mwheal::em_t( this, options_str );
  if ( name == "rushing_wind_kick" )
    return new mwheal::rwk_t( this, options_str );
  if ( name == "thunder_focus_tea" )
    return new mwheal::tft_t( this, options_str );
  if ( name == "mana_tea" )
    return new mwheal::mana_tea_t( this, options_str );
  if ( name == "invoke_yulon" || name == "invoke_yulon_the_jade_serpent" )
    return new mwheal::yulon_t( this, options_str );
  if ( name == "revival" )
    return new mwheal::revival_t( this, options_str );
  if ( name == "life_cocoon" )
    return new mwheal::cocoon_t( this, options_str );
  if ( name == "celestial_conduit" )
    return new mwheal::conduit_t( this, options_str );
  if ( mw->fist )
  {
    if ( name == "rising_sun_kick" )
      return new mwheal::f_rsk_t( this, options_str );
    if ( name == "tiger_palm" )
      return new mwheal::f_tp_t( this, options_str );
    if ( name == "blackout_kick" )
      return new mwheal::f_bok_t( this, options_str );
    if ( name == "spinning_crane_kick" )
      return new mwheal::f_sck_t( this, options_str );
    if ( name == "sheiluns_gift" )
      return new mwheal::f_sg_t( this, options_str );
    if ( name == "invoke_chiji" || name == "invoke_chiji_the_red_crane" )
      return new mwheal::chiji_t( this, options_str );
  }
  return nullptr;
}

std::unique_ptr<expr_t> monk_t::create_expression_mw_heal( std::string_view name )
{
  auto count_below = [ this ]( double pct ) {
    return [ this, pct ]() {
      int n = 0;
      for ( auto* t : sim->healing_no_pet_list )
        if ( !t->is_sleeping() && t->resources.pct( RESOURCE_HEALTH ) < pct )
          n++;
      return as<double>( n );
    };
  };
  if ( util::str_compare_ci( name, "injured_95" ) )
    return make_fn_expr( name, count_below( 0.95 ) );
  if ( util::str_compare_ci( name, "injured_90" ) )
    return make_fn_expr( name, count_below( 0.90 ) );
  if ( util::str_compare_ci( name, "injured_80" ) )
    return make_fn_expr( name, count_below( 0.80 ) );
  if ( util::str_compare_ci( name, "injured_60" ) )
    return make_fn_expr( name, count_below( 0.60 ) );
  if ( util::str_compare_ci( name, "injured_40" ) )
    return make_fn_expr( name, count_below( 0.40 ) );
  if ( util::str_compare_ci( name, "raid_deficit" ) )
    return make_fn_expr( name, [ this ]() {
      double sum = 0;
      int n      = 0;
      for ( auto* t : sim->healing_no_pet_list )
        if ( !t->is_sleeping() )
        {
          sum += 1.0 - t->resources.pct( RESOURCE_HEALTH );
          n++;
        }
      return n ? 100.0 * sum / n : 0.0;
    } );
  if ( util::str_compare_ci( name, "fist_pack" ) )
    return make_fn_expr( name, [ this ]() {
      // stretches of the key spent in a pack of enemies: Spinning Crane Kick instead of Tiger Palm
      double len = mw->t( "pack_len", 20.0 );
      return as<double>( std::fmod( sim->current_time().total_seconds(), len ) < len * mw->t( "pack_frac", 0.4 ) );
    } );
  if ( util::str_compare_ci( name, "rm_count" ) )
    return make_fn_expr( name, [ this ]() { return as<double>( mw->rm_count() ); } );
  if ( util::str_compare_ci( name, "group_size_mw" ) )
    return make_fn_expr( name, [ this ]() { return as<double>( sim->healing_no_pet_list.size() ); } );
  return nullptr;
}

void monk_t::apl_mw_heal()
{
  action_priority_list_t* precombat = get_action_priority_list( "precombat" );
  action_priority_list_t* def       = get_action_priority_list( "default" );
  precombat->add_action( "snapshot_stats" );
  precombat->add_action( "renewing_mist" );

  // the action list is built before the actions exist, so the build is read from the talents
  bool fist_build = find_talent_spell( talent_tree::SPECIALIZATION, "Way of the Crane", MONK_MISTWEAVER )->ok() ||
                    find_talent_spell( talent_tree::SPECIALIZATION, "Jadefire Teachings", MONK_MISTWEAVER )->ok();
  if ( fist_build )
  {
    // fistweaving: kick for Ancient Teachings / Way of the Crane / Crane Style, heal with the cooldowns
    def->add_action( "potion" );
    def->add_action( "use_items" );
    def->add_action( "invoke_chiji" );
    def->add_action( "celestial_conduit" );
    def->add_action( "revival,if=injured_60>=3" );
    def->add_action( "life_cocoon,if=injured_60>=1" );
    def->add_action( "mana_tea,if=mana.pct<85" );
    def->add_action( "thunder_focus_tea" );
    def->add_action( "renewing_mist,if=rm_count<1" );
    def->add_action( "rising_sun_kick" );
    def->add_action( "sheiluns_gift,if=injured_90>=1" );
    def->add_action( "enveloping_mist,if=buff.tft_mw.up|(buff.chiji_mw.up&injured_80>=1)" );
    def->add_action( "enveloping_mist,if=injured_60>=1" );
    def->add_action( "blackout_kick,if=buff.totm_mw.up" );
    def->add_action( "spinning_crane_kick" );
    def->add_action( "tiger_palm" );
    return;
  }

  def->add_action( "potion" );
  def->add_action( "use_items" );
  def->add_action( "invoke_yulon,if=group_size>=10&time>10" );
  def->add_action( "celestial_conduit,if=group_size>=10&time>10" );
  def->add_action( "revival,if=injured_60>=6" );
  def->add_action( "life_cocoon,if=injured_60>=1" );
  def->add_action( "mana_tea,if=mana.pct<85" );
  def->add_action( "thunder_focus_tea" );
  def->add_action( "renewing_mist" );
  def->add_action( "rushing_wind_kick" );
  def->add_action( "enveloping_mist,if=buff.tft_mw.up" );
  def->add_action( "enveloping_mist,if=injured_80>=1" );
  def->add_action( "vivify" );
}
