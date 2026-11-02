// Copyright (C) 2026 Pushnarity (WoW Hub). Added to SimulationCraft in this fork; GPL-3.0 like the rest (see LICENSE).
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
constexpr double golden_hour_scale = 0.77;  // log: 38k raw per Golden Hour against 50k from the replayed damage
constexpr int blossom_targets        = 6;    // Emerald Blossom hits per cast (log: 6.4)
constexpr int seedlings_per_blossom  = 4;    // Fluttering Seedlings per Emerald Blossom (log: 4.0)
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
  bool k_echo = false, k_db = false, k_ta = false, k_stasis = false, k_rewind = false, k_merithra = false, k_ve = false,
       k_fb = false, k_leaping = false;  // what the character has learned (the priority list only names those)

  action_t *reversion_hot = nullptr, *golden = nullptr, *reversal = nullptr, *consume = nullptr, *db_hot = nullptr,
           *db_echo = nullptr, *lf_set = nullptr, *twin_flame = nullptr, *merithra_chain = nullptr;
  action_t *seedling = nullptr, *panacea_heal = nullptr, *ta_shield = nullptr;

  action_t* echo_twin = nullptr;

  // Mythic+ build (Lifebind, Life-Giver's Flame, Lifespark, Flow State, Dream Simulacrum): see the key replays
  std::map<std::string, double> tune;  // pres_tune=name=value/... fits, for trying numbers without a rebuild
  double T( const char* n, double d ) const
  {
    auto it = tune.find( n );
    return it == tune.end() ? d : it->second;
  }
  bool k_lifebind = false, k_lgf = false, k_dsim = false, k_lifespark = false, k_flow = false, k_energy = false,
       k_mplus = false;
  buff_t *lifebind = nullptr, *lifespark = nullptr, *flow = nullptr;
  player_t* lb_partner = nullptr;
  action_t *lifebind_heal = nullptr, *lgf_heal = nullptr, *av_heal = nullptr, *instincts = nullptr;
  gain_t* energy_gain = nullptr;
  player_t* av_target = nullptr;  // Preternatural Antivenom: the ally infused now
  timespan_t av_until = timespan_t::min();
  double av_left = 0.0;

  // a fitted number: the raid-log value, or the Mythic+-log value when this is the Mythic+ build
  double C( const char* n, double raid, double mplus ) const
  {
    return T( n, k_mplus ? mplus : raid );
  }

  // Lifebind: healing on either partner heals the other for 60% of the raw amount
  void lifebind_transfer( player_t* t, double raw )
  {
    if ( !lifebind || !lifebind->check() || !lifebind_heal || raw <= 0 )
      return;
    player_t* other = t == p ? lb_partner : ( t == lb_partner ? p : nullptr );
    if ( !other || other->is_sleeping() )
      return;
    lifebind_heal->execute_on_target( other, lifebind->data().effectN( 1 ).percent() * T( "lifebind", 1.9 ) * raw );
  }

  // Merithra's Blessing: Essence abilities may charge the next Reversion (Mythic+: Emerald Blossom and Disintegrate roll too)
  void roll_merithra()
  {
    if ( k_mplus && merithra_3->ok() && p->rng().roll( merithra_chance ) )
      merithra->trigger();
  }

  double stasis_start = 0;
  bool stasis_on = false;  // Stasis stores the next three Dream Breath / Temporal Anomaly casts
  std::vector<std::function<void()>> stored;
  double merithra_chance = 0.10;  // pres_tune=merithra=<x> overrides  // data 20%; logs show 7-20% of Essence casts end in a Merithra's Blessing

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

  // The next Echo-able heal repeats on every Echoed ally (log: Merithra's Blessing hits 17 allies once and then
  // chains again from each Echoed one).
  std::vector<std::pair<player_t*, double>> take_echoes()
  {
    std::vector<std::pair<player_t*, double>> l;
    for ( auto& [ t, b ] : echo )
      if ( b->check() && !t->is_sleeping() )
      {
        l.emplace_back( t, b->check_value() );
        b->expire();
      }
    return l;
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
  bool transfers = true;  // heals Lifebind passes on (the Lifebind heal itself does not)
  void init() override
  {
    heal_t::init();
    snapshot_flags = update_flags = 0;
  }
  void assess_damage( result_amount_type t, action_state_t* s ) override
  {
    heal_t::assess_damage( t, s );
    if ( transfers )
      static_cast<evoker_t*>( player )->pres->lifebind_transfer( s->target, s->result_total );
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

  // Logs: heals crit 51% of the time, the stat sheet alone gives 41% (Rune of Critical Power etc. are in; the rest is not found)
  double composite_crit_chance() const override
  {
    return evoker_heal_t::composite_crit_chance() + 0.10;
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

  double cal = 1.0;  // fitted per spell against the mean heal of the log (the spec aura's -20% only fits some spells)

  double mult() const
  {
    return eff * cal;  // talent and tier modifiers come with the spell data ("Affected By" in the report)
  }

  void assess_damage( result_amount_type t, action_state_t* s ) override
  {
    evoker_heal_t::assess_damage( t, s );
    st().lifebind_transfer( s->target, s->result_total );
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
    random_targets = true;
    aoe = p->talent.flameshaper.fire_torrent.ok() ? 1 + as<int>( p->talent.flameshaper.fire_torrent->effectN( 1 ).base_value() ) : 1;
  }

  void execute() override
  {
    // Mythic+ logs: about 0.45 Twin Flames per Essence Burst spent (209 for ~470); the raid logs fit 1.0
    if ( !st().k_mplus || rng().roll( st().T( "tf", 0.45 ) ) )
      pres_heal_t::execute();
    // 12.1 Preservation tier 2-piece: consuming Essence Burst sends a Living Flame as well
    if ( st().lf_set && rng().roll( st().C( "lf_tier", 0.6, 0.1 ) ) )  // logs: 38-56 tier Living Flames for 73-76 Essence Burst consumptions
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
    cal               = p->pres->C( "lf_cal", 1.0, 0.7 );
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
    double amount = tick * ( consumed_time / tt.total_seconds() ) * pct * s.C( "cf_scale", consume_flame_scale, 1.3 );
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
    cal            = p->pres->C( "db_cal", 1.22, 0.6 );
    random_targets = true;  // the cone catches whoever stands in it
    aoe        = dream_breath_targets;
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
    cooldown->duration = timespan_t::from_seconds( 30.0 * p->pres->T( "db_cd", p->pres->k_mplus ? 1.15 : 1.0 ) );
    cooldown->hasted   = true;  // logs: a Dream Breath charge comes back about every 23 s at 30% haste
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
    // logs: an Echoed Dream Breath is repeated once, not once per Echoed ally
    auto echoes = st().take_echoes();
    if ( !echoes.empty() )
    {
      static_cast<pres_heal_t*>( echo_hot )->eff = echoes.front().second;
      echo_hot->execute_on_target( echoes.front().first );
    }
    if ( st().merithra_3->ok() )
      st().merithra->trigger();
    if ( st().essence_well->ok() && rng().roll( st().essence_well->effectN( 1 ).percent() ) )
      p()->buff.essence_burst->trigger();
    if ( st().temporal_compression )
      st().temporal_compression->expire();
    if ( st().flow )
      st().flow->trigger();
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

  void execute() override
  {
    essence_heal_t::execute();
    // Merithra's Blessing: Essence abilities may charge the next Reversion
    auto& s = *p()->pres;
    if ( s.merithra_3->ok() && rng().roll( s.merithra_chance ) )
      s.merithra->trigger();
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
  double composite_crit_chance() const override
  {
    return essence_heal_t::composite_crit_chance() + 0.10;
  }
  double mult() const
  {
    return 1.1;
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
    if ( st.twin_echoes_t->ok() && st.twin_echoes->check() )
    {
      st.twin_echoes->decrement();
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
  void tick( dot_t* d ) override
  {
    pres_heal_t::tick( d );
    auto& s = st();
    if ( s.lifespark && rng().roll( s.T( "ls_chance", 0.03 ) ) )
      s.lifespark->trigger();
  }
};

struct merithra_chain_t : public pres_heal_t
{
  merithra_chain_t( evoker_t* p ) : pres_heal_t( "merithra_blessing_heal", p, p->find_spell( 1256581 ) )
  {
    background = dual = true;
    auto_target       = true;
    cal               = p->pres->C( "mb_cal", 0.86, 0.36 );
    aoe               = 5;
  }
};

// Reversion, or Merithra's Blessing when Dream Breath has charged it.
struct reversion_t : public pres_heal_t
{
  bool merithra_version;
  action_t *chain;
  int golden_budget = 1;

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
      cooldown->duration = 9_s;
      cooldown->charges  = 2;
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
    // Golden Hour: Reversion heals 15% of the damage taken in the last 5 s (log: 12 allies of 17 per cast)
    if ( st().golden_hour->ok() && golden_budget > 0 )
    {
      double dmg = st().taken( t, st().golden_hour->effectN( 2 ).base_value() );
      if ( dmg > 0 )
      {
        st().golden->execute_on_target( t, dmg * st().golden_hour->effectN( 1 ).percent() * golden_hour_scale );
        golden_budget--;
      }
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
      for ( auto& [ ally, e ] : s.take_echoes() )
        apply_reversion( ally );
      return;
    }
    s.merithra->expire();
    auto all = s.allies();
    std::stable_sort( all.begin(), all.end(), [ &s ]( player_t* a, player_t* b ) { return s.taken( a, 5.0 ) > s.taken( b, 5.0 ); } );
    golden_budget = 12;
    for ( auto* t : all )
      apply_reversion( t );
    golden_budget = 1;
    // the first wave heals every ally once; every Echoed ally then chains again about a second later
    chain->aoe = 1;
    for ( auto* t : all )
      chain->execute_on_target( t );
    chain->aoe = 5;
    auto echoes = s.take_echoes();
    // the cast itself chains on from the target a few times (log: 15 hits beyond the first wave)
    for ( int i = 0; i < 5; i++ )
      echoes.emplace_back( target, 1.0 );
    if ( !echoes.empty() )
      make_event( *sim, 1_s, [ this, echoes ] {
        for ( auto& [ ally, e ] : echoes )
          if ( !ally->is_sleeping() )
          {
            static_cast<pres_heal_t*>( chain )->eff = e;
            chain->execute_on_target( ally );
          }
        static_cast<pres_heal_t*>( chain )->eff = 1.0;
      } );
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
    cooldown->hasted = true;
    coeff = 6.0 * 1.4;  // fitted: the log's shields absorb 368k a cast
    may_crit = false;
  }

  void wave()
  {
    auto all = st().allies();
    double sp   = p()->composite_total_spell_power( SCHOOL_MAX );
    double vers = 1.0 + p()->composite_heal_versatility();
    double red  = all.size() > 5 ? std::sqrt( 5.0 / all.size() ) : 1.0;
    for ( auto* t : all )
      if ( !t->is_sleeping() )
        st().ta_shield->execute_on_target( t, coeff * sp * vers * red );
  }

  void execute() override
  {
    pres_heal_t::execute();
    wave();
    if ( st().temporal_compression )
      st().temporal_compression->trigger();
    if ( st().stasis_on && st().stored.size() < 3 )
      st().stored.push_back( [ this ] { wave(); } );
  }
  void impact( action_state_t* ) override {}
};

// Stasis: stores the next three Dream Breath / Temporal Anomaly casts and unleashes them together
struct stasis_t : public pres_heal_t
{
  stasis_t( evoker_t* p, std::string_view o ) : pres_heal_t( "stasis", p, p->find_spell( 370537 ), o )
  {
    green              = false;
    cooldown->duration = 90_s;
    may_crit           = false;
  }
  bool ready() override
  {
    return !st().stasis_on && pres_heal_t::ready();
  }
  void execute() override
  {
    pres_heal_t::execute();
    st().stasis_on = true;
    st().stasis_start = st().now();
    st().stored.clear();
    make_event( *sim, 30_s, [ this ] {
      st().stasis_on = false;
      st().stored.clear();
    } );
  }
  void impact( action_state_t* ) override {}
};

struct stasis_release_t : public pres_heal_t
{
  stasis_release_t( evoker_t* p, std::string_view o ) : pres_heal_t( "stasis_release", p, p->find_spell( 370564 ), o )
  {
    green = false;
    may_crit = false;
    base_costs[ RESOURCE_MANA ] = 0;
  }
  bool ready() override
  {
    return st().stasis_on && ( st().stored.size() >= 3 || ( st().stored.size() >= 2 && st().now() - st().stasis_start >= 12.0 ) );
  }
  void execute() override
  {
    pres_heal_t::execute();
    auto l = st().stored;
    st().stored.clear();
    st().stasis_on = false;
    for ( auto& f : l )
      f();
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
  // cast when the group has just been hurt (logs: Rewind heals 55-60% of the last 5 s of damage taken)
  bool ready() override
  {
    if ( !pres_heal_t::ready() )
      return false;
    double sum = 0;
    auto all   = st().allies();
    for ( auto* t : all )
      sum += t->resources.pct( RESOURCE_HEALTH );
    return !all.empty() && sum / all.size() < 0.7;
  }

  void execute() override
  {
    pres_heal_t::execute();
    for ( auto* t : st().allies() )
      flat->execute_on_target( t, 0.33 * st().taken( t, 5.0 ) );
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
    cal               = p->pres->C( "eb_cal", 1.25, 0.9 );
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
    cal               = p->pres->C( "sd_cal", 1.2, 0.9 );
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
    s.roll_merithra();
    if ( s.twin_echoes_t->ok() )
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
    auto& s            = *p->pres;
    cooldown->duration = timespan_t::from_seconds( s.T( "ve_cd", 28.0 ) );
    auto_target        = false;  // the embrace flies to anyone, not to the most injured
    if ( s.k_dsim )  // Dream Simulacrum: +40% healing, no leap
      impact_action->base_multiplier *= ( 1.0 + s.T( "dsim", 0.40 ) ) * s.T( "ve_cal", 1.0 );
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
    auto& s = *p()->pres;
    if ( s.tier_4pc )  // 4-piece: Verdant Embrace grants Essence Burst
      p()->buff.essence_burst->trigger();
    consume_dream_breath( p(), target );
    if ( s.lifebind )  // Lifebind bonds the healer and the embraced ally for 8 s
    {
      s.lb_partner = target == p() ? nullptr : target;
      s.lifebind->trigger();
    }
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
    if ( st().lifespark && st().lifespark->check() )
      return 0_ms;
    if ( p()->buff.ancient_flame->check() )
      t *= 0.7;
    return t;
  }

  void execute() override
  {
    pres_heal_t::execute();
    int extra = p()->buff.leaping_flames->check();
    hit->aoe  = 1 + extra;
    if ( st().lifespark && st().lifespark->check() )  // Lifespark: instant and +100% healing
    {
      static_cast<pres_heal_t*>( hit )->eff = 1.0 + st().T( "ls_heal", 1.0 );
      st().lifespark->decrement();
    }
    hit->execute_on_target( target );
    int n = hit->num_targets_hit;
    p()->buff.leaping_flames->expire();
    p()->buff.ancient_flame->expire();
    if ( st().essence_burst_t->ok() )
      for ( int i = 0; i < n; i++ )
        if ( rng().roll( st().essence_burst_t->effectN( 1 ).percent() ) )
          p()->buff.essence_burst->trigger();
    static_cast<pres_heal_t*>( hit )->eff = 1.0;
  }
  void impact( action_state_t* ) override {}
};

// Disintegrate: a Preservation player in a key spends spare Essence on it; it heals nobody but its Energy Loop
// returns mana (logs: 246 casts in 1676 s, the channel is up 20% of the time, 1.9M mana back).
struct disintegrate_pres_t : public essence_heal_t
{
  disintegrate_pres_t( evoker_t* p, std::string_view o )
    : essence_heal_t( "disintegrate", p, p->find_class_spell( "Disintegrate" ), o )
  {
    harmful = false;
    channeled = false;
    dot_duration = base_tick_time = 0_ms;
    may_crit = false;
    spell_power_mod.direct = 0;
    base_execute_time = timespan_t::from_seconds( p->pres->T( "dis_time", 1.5 ) );
  }
  void execute() override
  {
    essence_heal_t::execute();
    auto& s = st();
    s.roll_merithra();
    if ( s.k_energy && s.energy_gain )
      p()->resource_gain( RESOURCE_MANA, s.T( "dis_mana", 7800.0 ), s.energy_gain );
  }
  pres_state_t& st() const
  {
    return *p()->pres;
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
    cooldown->duration = timespan_t::from_seconds( 30.0 * p->pres->T( "fb_cd", p->pres->k_mplus ? 1.2 : 1.0 ) );
    cooldown->hasted   = true;
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
  // Life-Giver's Flame: the breath's damage heals the injured, split evenly (logs: 4-5 pulses a cast, ~130k raw each)
  void life_givers_flame()
  {
    auto& s = st();
    if ( !s.k_lgf || !s.lgf_heal )
      return;
    int pulses = static_cast<int>( s.T( "lgf_pulses", 4.0 ) );
    for ( int i = 0; i < pulses; i++ )
      make_event( *sim, timespan_t::from_seconds( 0.5 + 1.33 * i ), [ this ] {
        auto& st2 = st();
        auto l    = st2.injured( 5 );
        if ( l.empty() )
          return;
        double total = st2.T( "lgf", 52.0 ) * p()->composite_total_spell_power( SCHOOL_MAX );
        for ( auto* t : l )
          st2.lgf_heal->execute_on_target( t, total / l.size() );
      } );
  }

  void execute() override
  {
    pres_heal_t::execute();
    life_givers_flame();
    if ( st().flow )
      st().flow->trigger();
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
double evoker_t::pres_flow_haste() const
{
  if ( pres && pres->flow && pres->flow->check() )
    return 1.0 / ( 1.0 + pres->flow->default_value * pres->T( "flow", 1.0 ) );
  return 1.0;
}

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

  for ( auto& kv : util::string_split<std::string_view>( pres_tune, "/" ) )
  {
    auto pr = util::string_split<std::string_view>( kv, "=" );
    if ( pr.size() == 2 )
      s.tune[ std::string( pr[ 0 ] ) ] = util::to_double( pr[ 1 ] );
  }
  s.k_lifebind  = ST( "Lifebind" )->ok();
  s.k_lgf       = ST( "Life-Giver's Flame" )->ok();
  s.k_dsim      = ST( "Dream Simulacrum" )->ok();
  s.k_lifespark = ST( "Lifespark" )->ok();
  s.k_flow      = ST( "Flow State" )->ok();
  s.k_energy    = ST( "Energy Loop" )->ok();
  s.k_mplus     = s.k_lifebind && !s.twin_echoes_t->ok();  // the Mythic+ build bonds with Verdant Embrace instead of Echoing
  s.merithra_chance = s.T( "merithra", s.k_mplus ? 0.11 : s.merithra_chance );
  if ( s.k_lifebind )
    s.lifebind = make_buff( this, "lifebind", find_spell( 373267 ) )->set_duration( timespan_t::from_seconds( s.T( "lifebind_dur", 8.0 ) ) );
  if ( s.k_lifespark )
    s.lifespark = make_buff( this, "lifespark", find_spell( 443176 ) )->set_max_stack( 2 );
  if ( s.k_flow )
    s.flow = make_buff( this, "flow_state", find_spell( 390148 ) )
                 ->set_default_value( ST( "Flow State" )->effectN( 1 ).percent() )
                 ->set_duration( 10_s );
  s.energy_gain = get_gain( "energy_loop" );

  s.merithra            = make_buff( this, "merithra_blessing_ready" )->set_duration( 30_s );
  s.temporal_compression = make_buff( this, "temporal_compression", find_spell( 362877 ) );
  s.twin_echoes = make_buff( this, "twin_echoes", find_spell( 1242759 ) );
  s.k_echo      = ST( "Echo" )->ok();
  s.k_db        = ST( "Dream Breath" )->ok();
  s.k_ta        = ST( "Temporal Anomaly" )->ok();
  s.k_stasis    = ST( "Stasis" )->ok();
  s.k_rewind    = ST( "Rewind" )->ok();
  s.k_merithra  = s.merithra_3->ok();
  s.k_ve        = talent.verdant_embrace.ok();
  s.k_fb        = s.consume_flame->ok() || s.essence_well->ok() || s.fire_torrent->ok();
  s.k_leaping   = talent.leaping_flames.ok();

  sim->heal_damage_callbacks.push_back( [ this ]( player_t* t, double amount ) {
    if ( !pres )
      return;
    auto& d = pres->hurt[ t ];
    d.emplace_back( pres->now(), amount );
    // Merithra's Blessing: Reversion turns 5% of the damage into healing
    if ( pres->reversal && pres->merithra_3->ok() && pres->reversion_hot )
      if ( dot_t* dt = pres->reversion_hot->find_dot( t ); dt && dt->is_ticking() )
        pres->reversal->execute_on_target( t, amount * 0.005 * 2.5 );  // data: 0.5% of the damage; logs heal 2.5x that
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
  if ( s.lifebind )
  {
    auto* a      = new flat_heal_t( this, "lifebind", 373268 );
    a->transfers = false;
    s.lifebind_heal = a;
  }
  if ( s.k_lgf )
    s.lgf_heal = new flat_heal_t( this, "life_givers_flame", 371441 );
  // Preternatural Antivenom (270171): about every 22 s an ally is infused and heals most of the damage it takes
  if ( find_item_by_id( 270171 ) )
  {
    s.av_heal = new flat_heal_t( this, "preternatural_antivenom", 1305921 );
    sim->heal_damage_callbacks.push_back( [ this ]( player_t* t, double amount ) {
      auto& r = *pres;
      if ( !r.av_heal || t != r.av_target || sim->current_time() > r.av_until || r.av_left <= 0 )
        return;
      double h = std::min( r.T( "av_share", 0.75 ) * amount, r.av_left );
      r.av_left -= h;
      r.av_heal->execute_on_target( t, h );
    } );
    register_combat_begin( [ this ]( player_t* ) {
      pres->av_target = nullptr;
      pres->av_left   = 0.0;
      make_repeating_event( *sim, timespan_t::from_seconds( pres->T( "av_period", 21.7 ) ), [ this ] {
        auto& r     = *pres;
        auto l      = r.allies();
        if ( l.empty() )
          return;
        r.av_target = l[ rng().range( l.size() ) ];
        r.av_until  = sim->current_time() + 8_s;
        r.av_left   = r.T( "av_cap", 1600000.0 );
      } );
    } );
  }
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
  if ( name == "disintegrate" )
    return new disintegrate_pres_t( this, options_str );
  if ( name == "stasis" )
    return new stasis_t( this, options_str );
  if ( name == "stasis_release" )
    return new stasis_release_t( this, options_str );
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

  auto& k = *pres;
  // mana paced over the fight: spend more freely early, keep a floor at the end
  const std::string pace = "(fight_remains>600|mana.pct>=8+80*fight_remains%(time+fight_remains))";
  def->add_action( "potion" );
  def->add_action( "use_items" );
  if ( k.k_rewind )
    def->add_action( "rewind,if=time>=30" );
  if ( k.k_stasis )
  {
    def->add_action( "stasis_release" );
    def->add_action( "stasis,if=cooldown.dream_breath.up" );
  }
  if ( k.k_ta )
    def->add_action( "temporal_anomaly" );
  if ( k.k_mplus )
  {
    // Mythic+ Preservation (logs: Merithra's Blessing on every charge, Verdant Embrace / Dream Breath / Fire Breath on
    // cooldown, Emerald Blossom on every free Essence, Living Flame and Disintegrate in between)
    if ( k.k_merithra )
      def->add_action( "merithra_blessing" );
    else
      def->add_action( "reversion" );
    if ( k.k_ve )
      def->add_action( "verdant_embrace" );
    if ( k.k_db )
      def->add_action( "dream_breath" );
    def->add_action( "fire_breath,if=injured_90>=1" );
    // "need" gates stand for the healer looking at the raid frames: cast the spender when enough allies are hurt
    auto need = [ &k ]( const char* name, int hp, int n ) {
      return "injured_" + std::to_string( static_cast<int>( k.T( ( std::string( name ) + "_hp" ).c_str(), hp ) ) ) +
             ">=" + std::to_string( static_cast<int>( k.T( ( std::string( name ) + "_n" ).c_str(), n ) ) );
    };
    if ( k.fluttering->ok() )
      def->add_action( "emerald_blossom,if=buff.essence_burst.up&" + need( "eb", 85, 3 ) );
    else if ( k.k_echo )  // without Seedlings the free Essence goes into Echo, which repeats the next heal
      def->add_action( "echo,if=buff.essence_burst.up&echoed_allies<" + std::to_string( static_cast<int>( k.T( "echo_max", 2 ) ) ) + "&" +
                       need( "echo", 85, 2 ) );
    def->add_action( "living_flame,if=(buff.lifespark.up|buff.leaping_flames.up)&" + need( "lfs", 90, 2 ) );
    def->add_action( "disintegrate,if=(essence>=3|buff.essence_burst.up)&mana.pct<" +
                     std::to_string( static_cast<int>( k.T( "dis_mana", 95 ) ) ) );
    def->add_action( "living_flame,if=" + need( "lf", 60, 4 ) );
    return;
  }
  if ( k.k_echo && k.k_merithra )
    def->add_action( "merithra_blessing,if=echoed_allies>=3" );
  // the log's cycle: Emerald Blossoms charge Twin Echoes, Echoes pile up, then the spender repeats them on every ally
  if ( k.k_echo )
  {
    if ( k.twin_echoes_t->ok() )
    {
      def->add_action( "echo,if=(buff.essence_burst.up|essence>=2&" + pace + ")&echoed_allies<4&buff.twin_echoes.up" );
      def->add_action( "emerald_blossom,if=(buff.essence_burst.up|essence>=3&" + pace + ")&buff.twin_echoes.stack<2&echoed_allies<4" );
    }
    def->add_action( "echo,if=(buff.essence_burst.up|essence>=2&" + pace + ")&echoed_allies<4" );
  }
  if ( k.k_merithra )
    def->add_action( "merithra_blessing" );
  else
    def->add_action( "reversion" );
  if ( k.k_db )
    def->add_action( "dream_breath" );
  def->add_action( "emerald_blossom,if=buff.essence_burst.up|essence>=4&" + pace );
  if ( k.k_ve )
    def->add_action( "verdant_embrace,if=" + pace );
  if ( k.k_fb )
    def->add_action( "fire_breath,if=" + pace );
  if ( k.k_leaping )
    def->add_action( "living_flame,if=buff.leaping_flames.up&" + pace );
  else
    def->add_action( "living_flame,if=mana.pct>60&" + pace );
}
