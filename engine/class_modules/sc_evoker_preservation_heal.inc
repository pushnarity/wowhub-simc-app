// wowhub: Preservation Evoker healing for healing sims (role=heal with healing targets).
// Included by sc_evoker.cpp (the evoker class lives in that one file). Upstream SimC has almost nothing for
// Preservation; everything here is ours and is only created in healing sims, so weekly engine updates rebase
// cleanly. Mechanics were measured on Warcraft Logs events (Midnight 12.1.5, Heroic Nek'zali top logs).

namespace pres_heal
{
// Numbers measured in top logs that the spell data does not carry (see the healer notes).
constexpr int dream_breath_targets   = 6;    // allies one Dream Breath reaches (log: 6 per cast)
constexpr int dream_breath_echo_targets = 14;  // an Echoed Dream Breath reaches many more (log: 12-14)
constexpr double consume_flame_scale = 0.85;  // log: 81.6k raw per detonation against 92.8k from the data
constexpr int blossom_targets        = 6;    // Emerald Blossom hits per cast (log: 6.4)
constexpr int seedlings_per_blossom  = 4;    // Fluttering Seedlings per Emerald Blossom (log: 4.0)
constexpr double merithra_extra_hits = 2.7;  // chain hits per ally after the first one (log: 3.7 in all)
}  // namespace pres_heal

struct pres_state_t
{
  evoker_t* p;

  // incoming damage per ally, for "healed from damage taken" effects (Golden Hour, Rewind)
  std::map<player_t*, std::deque<std::pair<double, double>>> hurt;
  std::map<player_t*, buff_t*> echo;  // Echo on each ally; its value is the copy's effectiveness

  buff_t* merithra     = nullptr;  // next Reversion is Merithra's Blessing
  buff_t* twin_echoes  = nullptr;
  buff_t* temporal_compression = nullptr;

  // talents (spell data, null data when not taken)
  const spell_data_t *time_lord = nullptr, *twin_echoes_t = nullptr, *golden_hour = nullptr, *fluttering = nullptr,
                     *merithra_3 = nullptr, *fulminous = nullptr, *deep_exhalation = nullptr, *consume_flame = nullptr,
                     *fire_torrent = nullptr, *essence_well = nullptr, *legacy = nullptr, *ashes = nullptr,
                     *essence_burst_t = nullptr;
  bool tier_2pc = false, tier_4pc = false;

  action_t *reversion_hot = nullptr, *golden = nullptr, *reversal = nullptr, *consume = nullptr, *db_hot = nullptr,
           *db_echo = nullptr, *lf_set = nullptr, *twin_flame = nullptr, *merithra_chain = nullptr;
  action_t *seedling = nullptr, *panacea_heal = nullptr, *ta_shield = nullptr;

  action_t* echo_twin = nullptr;

  explicit pres_state_t( evoker_t* pl ) : p( pl ) {}

  double now() const
  {
    return p->sim->current_time().total_seconds();
  }

  double taken( player_t* t, double window )
  {
    auto& d   = hurt[ t ];
    double n  = now();
    while ( !d.empty() && d.front().first < n - 8.0 )
      d.pop_front();
    double sum = 0;
    for ( auto& e : d )
      if ( e.first >= n - window )
        sum += e.second;
    return sum;
  }

  std::vector<player_t*> allies() const
  {
    std::vector<player_t*> l;
    for ( auto* t : p->sim->healing_no_pet_list )
      if ( !t->is_sleeping() )
        l.push_back( t );
    return l;
  }

  std::vector<player_t*> injured( size_t n, player_t* skip = nullptr ) const
  {
    auto l = allies();
    l.erase( std::remove( l.begin(), l.end(), skip ), l.end() );
    std::stable_sort( l.begin(), l.end(), []( player_t* a, player_t* b ) {
      return a->resources.pct( RESOURCE_HEALTH ) < b->resources.pct( RESOURCE_HEALTH );
    } );
    if ( l.size() > n )
      l.resize( n );
    return l;
  }

  buff_t* echo_buff( player_t* t )
  {
    auto& b = echo[ t ];
    if ( !b )
      b = make_buff( actor_pair_t( t, p ), "echo", p->find_spell( 364343 ) )->set_cooldown( 0_ms )->set_duration( 15_s );
    return b;
  }

  void apply_echo( player_t* t, double eff )
  {
    echo_buff( t )->trigger( 1, eff );
  }

  // The ally the player has Echoed; the next Echo-able heal is cast on it and repeats.
  player_t* echoed_ally()
  {
    player_t* best = nullptr;
    for ( auto& [ t, b ] : echo )
      if ( b->check() && !t->is_sleeping() && ( !best || t->resources.pct( RESOURCE_HEALTH ) < best->resources.pct( RESOURCE_HEALTH ) ) )
        best = t;
    return best;
  }

  double take_echo( player_t* t )
  {
    auto it = echo.find( t );
    if ( it == echo.end() || !it->second->check() )
      return 0.0;
    double v = it->second->check_value();
    it->second->expire();
    return v;
  }
};

namespace pres_heal
{
using essence_heal_t = heals::essence_heal_t;
// A heal that only carries an amount: no multipliers (Golden Hour, damage reversal, Consume Flame).
struct flat_heal_t : public heal_t
{
  flat_heal_t( evoker_t* p, std::string_view n, unsigned id ) : heal_t( n, p, p->find_spell( id ) )
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
  flat_absorb_t( evoker_t* p, std::string_view n, unsigned id ) : absorb_t( n, p, p->find_spell( id ) )
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
    double v = std::max( b->check_value(), s->result_amount );
    b->trigger( 1, v );
    stats->add_result( 0.0, s->result_amount, result_amount_type::ABSORB, s->result, s->block_result, s->target );
    (void)t;
  }
};

// Every Preservation heal.
struct pres_heal_t : public heals::evoker_heal_t
{
  double eff   = 1.0;
  bool green   = true;   // Green spells: tier 4-piece
  bool echoed_target = false;

  pres_heal_t( std::string_view n, evoker_t* p, const spell_data_t* s, std::string_view o = {} )
    : evoker_heal_t( n, p, s, o )
  {
    harmful = false;
  }

  pres_state_t& st() const
  {
    return *p()->pres;
  }

  bool random_targets = false;  // Emerald Blossom and its seedlings fly to whoever is near, not to the most injured

  void init() override
  {
    evoker_heal_t::init();
    if ( background )  // helper heals share their cast's mana, they do not pay again
      proc = true;
  }

  size_t available_targets( std::vector<player_t*>& tl ) const override
  {
    size_t n = evoker_heal_t::available_targets( tl );
    if ( random_targets && tl.size() > 1 )
      rng().shuffle( tl.begin() + 1, tl.end() );
    return n;
  }

  // Critical heals are 270% in the logs (Essence Burst's Exhilarating Burst and a gem on top of the 200%)
  double total_crit_bonus( const action_state_t* ) const override
  {
    return 1.7;
  }

  // Life-Binder: logs show no step at the healer's own health, so every ally gets the mastery bonus
  double composite_target_multiplier( player_t* t ) const override
  {
    return evoker_action_t<heal_t>::composite_target_multiplier( t ) * ( 1.0 + p()->cache.mastery_value() );
  }

  double mult() const
  {
    return eff;  // talent and tier modifiers come with the spell data ("Affected By" in the report)
  }

  double composite_da_multiplier( const action_state_t* s ) const override
  {
    return evoker_heal_t::composite_da_multiplier( s ) * mult();
  }
  double composite_ta_multiplier( const action_state_t* s ) const override
  {
    return evoker_heal_t::composite_ta_multiplier( s ) * mult();
  }

  // casts follow the Echo
  void schedule_execute( action_state_t* state = nullptr ) override
  {
    bool a = auto_target;
    if ( !state && echoed_target )
      if ( player_t* e = st().echoed_ally() )
      {
        auto_target = false;
        set_target( e );
      }
    evoker_heal_t::schedule_execute( state );
    auto_target = a;
  }
};

// Twin Flame (hero talent): Essence Burst consumption heals the target and bounces.
struct twin_flame_heal_t : public pres_heal_t
{
  twin_flame_heal_t( evoker_t* p ) : pres_heal_t( "twin_flame_heal", p, p->find_spell( 1265991 ) )
  {
    background = true;
    green      = false;
    auto_target = true;
    aoe = p->talent.flameshaper.fire_torrent.ok() ? 1 + as<int>( p->talent.flameshaper.fire_torrent->effectN( 1 ).base_value() ) : 1;
  }

  void execute() override
  {
    pres_heal_t::execute();
    // 12.1 Preservation tier 2-piece: consuming Essence Burst sends a Living Flame as well
    if ( st().lf_set )
      st().lf_set->execute_on_target( target );
  }
};

struct living_flame_hit_t : public pres_heal_t
{
  living_flame_hit_t( evoker_t* p, std::string_view n, double e ) : pres_heal_t( n, p, p->spec.living_flame_heal )
  {
    background = dual = true;
    eff               = e;
    auto_target       = true;
    green             = false;
    dot_duration      = base_tick_time = 0_ms;  // Ruby Embers is a Devastation talent
  }
  int n_targets() const override
  {
    return aoe > 0 ? aoe : 1;
  }
  void impact( action_state_t* s ) override
  {
    pres_heal_t::impact( s );
    // Titanic Precision: a critical Living Flame has an extra chance of Essence Burst
    if ( s->result == RESULT_CRIT && p()->talent.flameshaper.titanic_precision.ok() &&
         rng().roll( p()->talent.flameshaper.titanic_precision->effectN( 2 ).percent() ) )
      p()->buff.essence_burst->trigger();
  }
};

// Consume Flame: Emerald Blossom, Fluttering Seedlings and Verdant Embrace detonate Dream Breath.
inline void consume_dream_breath( evoker_t* p, player_t* t )
{
  auto& s = *p->pres;
  if ( !s.consume_flame->ok() || !s.db_hot )
    return;
  double consumed_time = s.consume_flame->effectN( 1 ).base_value() / 1000.0;
  double pct           = s.consume_flame->effectN( 6 ).percent();
  for ( action_t* a : { s.db_hot, s.db_echo } )
  {
    if ( !a )
      continue;
    dot_t* d = a->find_dot( t );
    if ( !d || !d->is_ticking() )
      continue;
    timespan_t tt = a->tick_time( d->state );
    double tick   = a->calculate_tick_amount( d->state, d->current_stack() );
    double amount = tick * ( consumed_time / tt.total_seconds() ) * pct * consume_flame_scale;
    d->adjust_duration( -timespan_t::from_seconds( consumed_time ) );
    s.consume->execute_on_target( t, amount );
  }
}

struct dream_breath_hot_t : public pres_heal_t
{
  dream_breath_hot_t( evoker_t* p, std::string_view n, double e ) : pres_heal_t( n, p, p->find_spell( 355941 ) )
  {
    background = true;
    eff        = e;
    auto_target = true;
    random_targets = true;  // the cone catches whoever stands in it
    aoe        = e < 1.0 || n.find( "echo" ) != std::string_view::npos ? dream_breath_echo_targets : dream_breath_targets;
    dot_duration   = 14_s + timespan_t::from_seconds( p->pres->deep_exhalation->ok() ? p->pres->deep_exhalation->effectN( 2 ).base_value() / 1000.0 : 0.0 );
    spell_power_mod.direct = 0;  // the log has no separate first hit: the first tick lands on the cast
    base_tick_time = 2_s;
    if ( p->pres->fulminous->ok() )
      base_tick_time *= 1.0 + p->pres->fulminous->effectN( 1 ).percent();
    hasted_ticks = true;
    tick_zero    = true;
  }
};

struct dream_breath_t : public pres_heal_t
{
  action_t *hot, *echo_hot;
  double max_cast;

  dream_breath_t( evoker_t* p, std::string_view o ) : pres_heal_t( "dream_breath", p, p->find_spell( 355936 ), o )
  {
    channeled = false;
    dot_duration = 0_ms;
    base_tick_time = 0_ms;
    echoed_target  = true;
    base_execute_time = 2500_ms;  // rank 3, the log's usual level
    hot      = p->pres->db_hot;
    echo_hot = p->pres->db_echo;
    add_child( hot );
    add_child( echo_hot );
    if ( p->pres->legacy->ok() )
      cooldown->charges = 2;
    cooldown->duration = 30_s;
    gcd_type = gcd_haste_type::NONE;
    trigger_gcd = 500_ms;
    may_crit = false;
  }

  timespan_t execute_time() const override
  {
    timespan_t t = pres_heal_t::execute_time();
    if ( st().temporal_compression )
      t *= 1.0 - std::min( 0.5, 0.10 * st().temporal_compression->check() );
    return t;
  }

  void execute() override
  {
    pres_heal_t::execute();
    player_t* tgt = target;
    hot->execute_on_target( tgt );
    if ( double e = st().take_echo( tgt ) ; e > 0 )
    {
      static_cast<pres_heal_t*>( echo_hot )->eff = e;
      echo_hot->execute_on_target( tgt );
    }
    if ( st().merithra_3->ok() )
      st().merithra->trigger();
    if ( st().essence_well->ok() && rng().roll( st().essence_well->effectN( 1 ).percent() ) )
      p()->buff.essence_burst->trigger();
    if ( st().temporal_compression )
      st().temporal_compression->expire();
  }
};

// Essence spells with a mana cost on top (Echo, Emerald Blossom): a free cast from Essence Burst skips both.
struct mana_essence_t : public essence_heal_t
{
  bool burst_free = false;
  mana_essence_t( std::string_view n, evoker_t* p, const spell_data_t* s, std::string_view o )
    : essence_heal_t( n, p, s, o )
  {
  }

  void consume_resource() override
  {
    burst_free = p()->buff.essence_burst->check() > 0;
    essence_heal_t::consume_resource();
    if ( burst_free || proc || base_costs[ RESOURCE_MANA ] == 0 )
      return;
    resource_e prev = resource_current;
    resource_current = RESOURCE_MANA;
    last_resource_cost = cost();
    player->resource_loss( RESOURCE_MANA, last_resource_cost, nullptr, this );
    stats->consume_resource( RESOURCE_MANA, last_resource_cost );
    resource_current = prev;
  }

  bool ready() override
  {
    if ( !essence_heal_t::ready() )
      return false;
    if ( p()->buff.essence_burst->check() || base_costs[ RESOURCE_MANA ] == 0 )
      return true;
    resource_e prev  = resource_current;
    resource_current = RESOURCE_MANA;
    bool ok          = player->resource_available( RESOURCE_MANA, cost() );
    resource_current = prev;
    return ok;
  }
};

struct echo_t : public mana_essence_t
{
  // mirror pres_heal_t's multipliers here
  double total_crit_bonus( const action_state_t* ) const override
  {
    return 1.7;
  }
  double mult() const
  {
    return 1.0;
  }
  echo_t( evoker_t* p, std::string_view o ) : mana_essence_t( "echo", p, p->find_spell( 364343 ), o )
  {
    harmful = false;
  }
  double composite_da_multiplier( const action_state_t* s ) const override
  {
    return essence_heal_t::composite_da_multiplier( s ) * mult();
  }
  void impact( action_state_t* s ) override
  {
    essence_heal_t::impact( s );
    auto& st      = *p()->pres;
    double e      = 0.70 * ( 1.0 + ( st.time_lord->ok() ? st.time_lord->effectN( 1 ).percent() : 0.0 ) );
    st.apply_echo( s->target, e );
    if ( st.twin_echoes && st.twin_echoes->check() )
    {
      st.twin_echoes->expire();
      auto l = st.injured( 1, s->target );
      if ( !l.empty() )
      {
        st.apply_echo( l.front(), st.twin_echoes_t->effectN( 1 ).percent() );
        if ( st.echo_twin )
          st.echo_twin->execute_on_target( l.front() );
      }
    }
    if ( st.temporal_compression )
      st.temporal_compression->trigger();
  }
};

// Twin Echoes: the Echo reaches a second ally at a quarter of its strength
struct echo_twin_t : public pres_heal_t
{
  echo_twin_t( evoker_t* p ) : pres_heal_t( "echo_twin", p, p->find_spell( 364343 ) )
  {
    background = true;
    auto_target = false;
    green       = false;
    eff         = p->pres->twin_echoes_t->ok() ? p->pres->twin_echoes_t->effectN( 1 ).percent() : 0.25;
  }
};

struct reversion_hot_t : public pres_heal_t
{
  reversion_hot_t( evoker_t* p, std::string_view n ) : pres_heal_t( n, p, p->find_spell( 367364 ) )
  {
    background     = true;
    auto_target    = false;
    dot_duration   = 12_s;
    base_tick_time = 2_s;
    hasted_ticks   = true;
    tick_zero      = false;
    may_crit       = true;
  }
};

struct merithra_chain_t : public pres_heal_t
{
  merithra_chain_t( evoker_t* p ) : pres_heal_t( "merithra_blessing_heal", p, p->find_spell( 1256581 ) )
  {
    background = dual = true;
    auto_target       = true;
    aoe               = 5;
  }
};

// Reversion, or Merithra's Blessing when Dream Breath has charged it.
struct reversion_t : public pres_heal_t
{
  bool merithra_version;
  action_t *chain;

  reversion_t( evoker_t* p, std::string_view o, bool m )
    : pres_heal_t( m ? "merithra_blessing" : "reversion", p, p->find_spell( m ? 1256581 : 366155 ), o ), merithra_version( m )
  {
    echoed_target = true;
    chain         = p->pres->merithra_chain;
    add_child( p->pres->reversion_hot );
    add_child( p->pres->golden );
    if ( m )
      add_child( chain );
    if ( !m )
    {
      cooldown->duration = 9_s;
      cooldown->charges  = 2;
    }
    else
    {
      cooldown->duration = 0_ms;
      aoe                = 0;
    }
    may_crit = false;
    spell_power_mod.direct = 0;
  }

  bool ready() override
  {
    if ( merithra_version && !st().merithra->check() )
      return false;
    if ( !merithra_version && st().merithra->check() )
      return false;
    return pres_heal_t::ready();
  }

  void apply_reversion( player_t* t )
  {
    st().reversion_hot->execute_on_target( t );
    // Golden Hour: Reversion heals 15% of the damage taken in the last 5 s
    if ( st().golden_hour->ok() )
    {
      double dmg = st().taken( t, st().golden_hour->effectN( 2 ).base_value() );
      if ( dmg > 0 )
        st().golden->execute_on_target( t, dmg * st().golden_hour->effectN( 1 ).percent() );
    }
    if ( st().essence_burst_t->ok() && rng().roll( st().essence_burst_t->effectN( 2 ).percent() ) )
      p()->buff.essence_burst->trigger();
  }

  void execute() override
  {
    pres_heal_t::execute();
    auto& s = st();
    if ( !merithra_version )
    {
      apply_reversion( target );
      if ( double e = s.take_echo( target ); e > 0 )
        apply_reversion( target );
      return;
    }
    s.merithra->expire();
    auto all = s.allies();
    for ( auto* t : all )
      apply_reversion( t );
    // the first wave heals every ally once, the chains follow about a second later
    chain->aoe = 1;
    for ( auto* t : all )
      chain->execute_on_target( t );
    chain->aoe = 5;
    double extra = pres_heal::merithra_extra_hits * all.size() / 5.0;
    int n        = int( extra );
    if ( rng().roll( extra - n ) )
      n++;
    player_t* t0 = target;
    make_event( *sim, 1_s, [ this, n, t0 ] {
      for ( int i = 0; i < n; i++ )
        chain->execute_on_target( t0 );
    } );
    if ( double e = s.take_echo( target ); e > 0 )
      chain->execute_on_target( target );
  }

  // the chain heals are the cast's own effect; the cast itself heals nobody
  void impact( action_state_t* s ) override
  {
    (void)s;
  }
};

// Temporal Anomaly: shields everyone in its path.
struct temporal_anomaly_t : public pres_heal_t
{
  double coeff;
  temporal_anomaly_t( evoker_t* p, std::string_view o ) : pres_heal_t( "temporal_anomaly", p, p->find_spell( 373861 ), o )
  {
    green = false;
    add_child( p->pres->ta_shield );
    coeff = 6.0 * 1.4;  // fitted: the log's shields absorb 368k a cast
    may_crit = false;
  }

  void execute() override
  {
    pres_heal_t::execute();
    auto all = st().allies();
    double sp   = p()->composite_total_spell_power( SCHOOL_MAX );
    double vers = 1.0 + p()->composite_heal_versatility();
    double red  = all.size() > 5 ? std::sqrt( 5.0 / all.size() ) : 1.0;
    // the vortex passes three times (4 s)
    for ( int k = 0; k < 1; k++ )
      make_event( *sim, timespan_t::from_seconds( 1.3 * k ), [ this, all, sp, vers, red ] {
        for ( auto* t : all )
          if ( !t->is_sleeping() )
            st().ta_shield->execute_on_target( t, coeff * sp * vers * red );
      } );
    if ( st().temporal_compression )
      st().temporal_compression->trigger();
  }
  void impact( action_state_t* ) override {}
};

// Rewind: heals everyone for part of what they took in the last 5 s (log: 2.7M over 138 hits from one cast)
struct rewind_t : public pres_heal_t
{
  action_t* flat;
  rewind_t( evoker_t* p, std::string_view o ) : pres_heal_t( "rewind", p, p->find_spell( 363534 ), o )
  {
    green = false;
    flat  = new flat_heal_t( p, "rewind_heal", 363534 );
    add_child( flat );
    may_crit               = false;
    spell_power_mod.direct = 0;
    spell_power_mod.tick   = 0;
    dot_duration = base_tick_time = 0_ms;
    cooldown->duration = 240_s;
  }
  void execute() override
  {
    pres_heal_t::execute();
    for ( auto* t : st().allies() )
      flat->execute_on_target( t, 1.5 * 0.55 * st().taken( t, 5.0 ) );
    if ( st().temporal_compression )
      st().temporal_compression->trigger();
  }
  void impact( action_state_t* ) override {}
};

// Emerald Blossom: heals, seedlings and a Consume Flame detonation
struct blossom_heal_t : public pres_heal_t
{
  blossom_heal_t( evoker_t* p ) : pres_heal_t( "emerald_blossom_heal", p, p->spec.emerald_blossom_heal )
  {
    background = dual = true;
    auto_target       = true;
    random_targets    = true;
    aoe               = blossom_targets;
  }
  void impact( action_state_t* s ) override
  {
    pres_heal_t::impact( s );
    consume_dream_breath( p(), s->target );
  }
};

struct seedling_t : public pres_heal_t
{
  seedling_t( evoker_t* p ) : pres_heal_t( "fluttering_seedlings", p, p->find_spell( 361361 ) )
  {
    background = dual = true;
    auto_target       = true;
    random_targets    = true;
  }
  void impact( action_state_t* s ) override
  {
    pres_heal_t::impact( s );
    consume_dream_breath( p(), s->target );
  }
};

struct emerald_blossom_t : public mana_essence_t
{
  action_t *heal, *seedling, *panacea;
  emerald_blossom_t( evoker_t* p, std::string_view o ) : mana_essence_t( "emerald_blossom", p, p->spec.emerald_blossom, o )
  {
    harmful  = false;
    heal     = p->get_secondary_action<blossom_heal_t>( "emerald_blossom_heal" );
    seedling = p->pres->seedling;
    panacea  = p->pres->panacea_heal;
    add_child( heal );
    if ( seedling )
      add_child( seedling );
    if ( panacea )
      add_child( panacea );
    cooldown->duration = 0_ms;
    may_crit           = false;
    spell_power_mod.direct = 0;
  }

  void execute() override
  {
    essence_heal_t::execute();
    auto& s = *p()->pres;
    if ( s.twin_echoes )
      s.twin_echoes->trigger();
    if ( p()->talent.ancient_flame.ok() )
      p()->buff.ancient_flame->trigger();
    if ( panacea )
      panacea->execute_on_target( p() );
    make_event( *sim, 1500_ms, [ this ] {
      heal->execute_on_target( target );
      if ( seedling )
      {
        auto l = p()->pres->allies();
        rng().shuffle( l.begin(), l.end() );
        if ( l.size() > size_t( pres_heal::seedlings_per_blossom ) )
          l.resize( pres_heal::seedlings_per_blossom );
        for ( auto* t : l )
          seedling->execute_on_target( t );
      }
    } );
  }
  void impact( action_state_t* ) override {}
};

struct verdant_embrace_t : public heals::verdant_embrace_t
{
  verdant_embrace_t( evoker_t* p, std::string_view o ) : heals::verdant_embrace_t( p, o )
  {
    cooldown->duration = 28_s;
    auto_target        = false;  // the embrace flies to anyone, not to the most injured
  }
  void schedule_execute( action_state_t* state = nullptr ) override
  {
    if ( !state )
    {
      auto l = p()->pres->allies();
      if ( !l.empty() )
        set_target( l[ rng().range( l.size() ) ] );
    }
    heals::verdant_embrace_t::schedule_execute( state );
  }
  void execute() override
  {
    heals::verdant_embrace_t::execute();
    if ( p()->pres->tier_4pc )  // 4-piece: Verdant Embrace grants Essence Burst
      p()->buff.essence_burst->trigger();
    consume_dream_breath( p(), target );
  }
};

struct living_flame_t : public pres_heal_t
{
  action_t* hit;
  living_flame_t( evoker_t* p, std::string_view o ) : pres_heal_t( "living_flame", p, p->find_class_spell( "Living Flame" ), o )
  {
    green         = false;
    echoed_target = true;
    hit           = p->get_secondary_action<living_flame_hit_t>( "living_flame_heal_pres", "living_flame_heal_pres", 1.0 );
    add_child( hit );
    may_crit = false;
    spell_power_mod.direct = 0;
  }

  timespan_t execute_time() const override
  {
    timespan_t t = pres_heal_t::execute_time();
    if ( p()->buff.ancient_flame->check() )
      t *= 0.7;
    return t;
  }

  void execute() override
  {
    pres_heal_t::execute();
    int extra = p()->buff.leaping_flames->check();
    hit->aoe  = 1 + extra;
    hit->execute_on_target( target );
    int n = hit->num_targets_hit;
    p()->buff.leaping_flames->expire();
    p()->buff.ancient_flame->expire();
    if ( st().essence_burst_t->ok() )
      for ( int i = 0; i < n; i++ )
        if ( rng().roll( st().essence_burst_t->effectN( 1 ).percent() ) )
          p()->buff.essence_burst->trigger();
    if ( double e = st().take_echo( target ); e > 0 )
    {
      static_cast<pres_heal_t*>( hit )->eff = e;
      hit->aoe = 1;
      hit->execute_on_target( target );
      static_cast<pres_heal_t*>( hit )->eff = 1.0;
    }
  }
  void impact( action_state_t* ) override {}
};

// Fire Breath: for Preservation it only feeds Living Flame (Leaping Flames) and Essence Burst.
struct fire_breath_t : public pres_heal_t
{
  fire_breath_t( evoker_t* p, std::string_view o ) : pres_heal_t( "fire_breath", p, p->find_spell( 357208 ), o )
  {
    channeled = false;
    dot_duration = base_tick_time = 0_ms;
    base_execute_time = 2500_ms;
    green             = false;
    cooldown->duration = 30_s;
    if ( p->pres->legacy->ok() )
      cooldown->charges = 2;
    gcd_type    = gcd_haste_type::NONE;
    trigger_gcd = 500_ms;
    may_crit    = false;
  }
  timespan_t execute_time() const override
  {
    timespan_t t = pres_heal_t::execute_time();
    if ( st().temporal_compression )
      t *= 1.0 - std::min( 0.5, 0.10 * st().temporal_compression->check() );
    return t;
  }
  void execute() override
  {
    pres_heal_t::execute();
    if ( p()->talent.leaping_flames.ok() )
      p()->buff.leaping_flames->trigger( 3 );
    if ( st().ashes->ok() )
      cooldown->adjust( timespan_t::from_millis( st().ashes->effectN( 1 ).base_value() ) );
    if ( st().essence_well->ok() && rng().roll( st().essence_well->effectN( 1 ).percent() ) )
      p()->buff.essence_burst->trigger();
    if ( st().temporal_compression )
      st().temporal_compression->expire();
  }
  void impact( action_state_t* ) override {}
};
}  // namespace pres_heal

// ==========================================================================
// Hooks
// ==========================================================================
bool evoker_t::pres_heal_mode() const
{
  return specialization() == EVOKER_PRESERVATION && primary_role() == ROLE_HEAL && sim->heal_target;
}

void evoker_t::init_pres_heal()
{
  if ( pres )
    return;
  pres    = new pres_state_t( this );
  auto& s = *pres;
  auto ST = [ this ]( std::string_view n ) {
    auto t = find_talent_spell( talent_tree::SPECIALIZATION, n );
    return t.ok() ? t.spell() : spell_data_t::not_found();
  };
  auto HT = [ this ]( std::string_view n ) {
    auto t = find_talent_spell( talent_tree::HERO, n );
    return t.ok() ? t.spell() : spell_data_t::not_found();
  };
  s.time_lord       = ST( "Time Lord" );
  s.essence_burst_t = ST( "Essence Burst" );
  s.twin_echoes_t   = ST( "Twin Echoes" );
  s.golden_hour     = ST( "Golden Hour" );
  s.fluttering      = ST( "Fluttering Seedlings" );
  s.merithra_3      = ST( "Merithra's Blessing" )->ok() ? find_spell( 1256689 ) : spell_data_t::not_found();
  s.fulminous       = HT( "Fulminous Roar" );
  s.deep_exhalation = HT( "Deep Exhalation" );
  s.consume_flame   = HT( "Consume Flame" );
  s.fire_torrent    = HT( "Fire Torrent" );
  s.essence_well    = HT( "Essence Well" );
  s.legacy          = HT( "Legacy of the Lifebinder" );
  s.ashes           = HT( "Ashes in Motion" );
  s.tier_2pc        = sets->has_set_bonus( EVOKER_PRESERVATION, MID2, B2 );
  s.tier_4pc        = sets->has_set_bonus( EVOKER_PRESERVATION, MID2, B4 );

  s.merithra            = make_buff( this, "merithra_blessing_ready" )->set_duration( 30_s );
  s.temporal_compression = make_buff( this, "temporal_compression", find_spell( 362877 ) );
  if ( s.twin_echoes_t->ok() )
    s.twin_echoes = make_buff( this, "twin_echoes", find_spell( 1242759 ) );

  sim->heal_damage_callbacks.push_back( [ this ]( player_t* t, double amount ) {
    if ( !pres )
      return;
    auto& d = pres->hurt[ t ];
    d.emplace_back( pres->now(), amount );
    // Merithra's Blessing: Reversion turns 5% of the damage into healing
    if ( pres->reversal && pres->merithra_3->ok() && pres->reversion_hot )
      if ( dot_t* dt = pres->reversion_hot->find_dot( t ); dt && dt->is_ticking() )
        pres->reversal->execute_on_target( t, amount * 0.05 );
  } );
  register_combat_begin( [ this ]( player_t* ) {
    pres->hurt.clear();
  } );
}

void evoker_t::create_pres_heal_actions()
{
  init_pres_heal();
  auto& s = *pres;
  using namespace pres_heal;
  s.echo_twin       = get_secondary_action<echo_twin_t>( "echo_twin" );
  s.reversion_hot   = get_secondary_action<reversion_hot_t>( "reversion_hot", "reversion_hot" );
  s.golden          = new flat_heal_t( this, "golden_hour", 378213 );
  s.reversal        = new flat_heal_t( this, "merithra_blessing_reversal", 1256688 );
  s.consume         = new flat_heal_t( this, "consume_flame", 445495 );
  s.merithra_chain  = get_secondary_action<merithra_chain_t>( "merithra_blessing_heal" );
  s.db_hot          = get_secondary_action<dream_breath_hot_t>( "dream_breath_hot", "dream_breath_hot", 1.0 );
  s.db_echo         = get_secondary_action<dream_breath_hot_t>( "dream_breath_echo", "dream_breath_echo", 1.0 );
  s.ta_shield       = new flat_absorb_t( this, "temporal_anomaly_shield", 373862 );
  s.seedling        = s.fluttering->ok() ? get_secondary_action<seedling_t>( "fluttering_seedlings" ) : nullptr;
  if ( talent.panacea.ok() )
    s.panacea_heal = get_secondary_action<heals::panacea_t>( "panacea_pres", "panacea_pres" );
  if ( s.tier_2pc )
    s.lf_set = get_secondary_action<living_flame_hit_t>( "living_flame_tier", "living_flame_tier", 1.0 );
  if ( talent.flameshaper.twin_flame.ok() )
  {
    s.twin_flame = get_secondary_action<twin_flame_heal_t>( "twin_flame_heal" );
    action.twin_flame = s.twin_flame;
  }
}

action_t* evoker_t::create_action_pres_heal( std::string_view name, std::string_view options_str )
{
  using namespace pres_heal;
  init_pres_heal();
  if ( name == "echo" )
    return new echo_t( this, options_str );
  if ( name == "dream_breath" )
    return new dream_breath_t( this, options_str );
  if ( name == "merithra_blessing" )
    return new reversion_t( this, options_str, true );
  if ( name == "reversion" )
    return new reversion_t( this, options_str, false );
  if ( name == "temporal_anomaly" )
    return new temporal_anomaly_t( this, options_str );
  if ( name == "emerald_blossom" )
    return new emerald_blossom_t( this, options_str );
  if ( name == "living_flame" )
    return new living_flame_t( this, options_str );
  if ( name == "fire_breath" )
    return new fire_breath_t( this, options_str );
  if ( name == "rewind" )
    return new rewind_t( this, options_str );
  if ( name == "verdant_embrace" )
    return new pres_heal::verdant_embrace_t( this, options_str );
  return nullptr;
}

void evoker_t::apl_pres_heal()
{
  action_priority_list_t* precombat = get_action_priority_list( "precombat" );
  action_priority_list_t* def       = get_action_priority_list( "default" );
  precombat->add_action( "snapshot_stats" );

  def->add_action( "potion" );
  def->add_action( "use_items" );
  def->add_action( "rewind,if=time>=60" );
  def->add_action( "temporal_anomaly" );
  def->add_action( "merithra_blessing" );
  def->add_action( "dream_breath" );
  def->add_action( "echo,if=(buff.essence_burst.up|essence>=2)&echoed_allies=0" );
  def->add_action( "emerald_blossom,if=buff.essence_burst.up|essence>=3" );
  def->add_action( "echo,if=buff.essence_burst.up|essence>=2" );
  def->add_action( "verdant_embrace" );
  def->add_action( "fire_breath" );
  def->add_action( "living_flame,if=buff.leaping_flames.up" );
}
