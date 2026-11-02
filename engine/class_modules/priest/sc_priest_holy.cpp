// ==========================================================================
// Holy Priest Sim File
// Contact: https://github.com/orgs/simulationcraft/teams/priest/members
// Wiki: https://github.com/simulationcraft/simc/wiki/Priests
// ==========================================================================

#include "sc_priest.hpp"

#include "simulationcraft.hpp"

namespace priestspace
{
namespace actions::heals
{
struct echo_of_light_t final : public residual_action::residual_periodic_action_t<priest_heal_t>
{
  using ab = residual_action::residual_periodic_action_t<priest_heal_t>;

  echo_of_light_t( priest_t& p ) : ab( "echo_of_light", p, p.specs.echo_of_light )
  {
    harmful      = false;
    background   = true;
    holy_mastery = false;
    tick_may_crit = false;  // wowhub: Echo of Light never crits in logs
  }
};
struct holy_word_sanctify_t final : public priest_heal_t
{
  holy_word_sanctify_t( priest_t& p, util::string_view options_str )
    : priest_heal_t( "holy_word_sanctify", p, p.talents.holy.holy_word_sanctify )
  {
    parse_options( options_str );

    harmful      = false;
    holy_mastery = true;

    if ( data().ok() )
      aoe = as<int>( data().effectN( 2 ).base_value() );
  }

  double cost() const override
  {
    if ( priest().buffs.apotheosis->check() )
    {
      return 0;
    }

    return priest_heal_t::cost();
  }

  void execute() override
  {
    priest_heal_t::execute();

    priest().trigger_epiphany();

    if ( priest().talents.holy.divine_image.enabled() )
    {
      priest().buffs.divine_image->trigger();
      priest().procs.divine_image->occur();
    }
  }
};

struct holy_word_serenity_t final : public priest_heal_t
{
  holy_word_serenity_t( priest_t& p, util::string_view options_str )
    : priest_heal_t( "holy_word_serenity", p, p.talents.holy.holy_word_serenity )
  {
    parse_options( options_str );
    harmful      = false;
    holy_mastery = true;
    target_pool  = p.options.holy_serenity_pool;
  }

  double cost() const override
  {
    if ( priest().buffs.apotheosis->check() )
    {
      return 0;
    }

    return priest_heal_t::cost();
  }

  void execute() override
  {
    priest_heal_t::execute();

    priest().trigger_epiphany();

    if ( priest().talents.holy.divine_image.enabled() )
    {
      priest().buffs.divine_image->trigger();
      priest().procs.divine_image->occur();
    }

    priest().divine_image_heals( false );

    // wowhub: Ultimate Serenity, Lasting Words, Cosmic Ripple (Serenity coming off cooldown), Benediction
    priest().holy_serenity_extras( target );
  }
};

// ==========================================================================
// wowhub: Holy healing sims
// ==========================================================================
std::vector<player_t*> holy_injured_allies( const priest_t& p, player_t* exclude, size_t n )
{
  std::vector<player_t*> v;
  for ( auto* t : p.sim->healing_no_pet_list )
    if ( t != exclude && !t->is_sleeping() )
      v.push_back( t );
  n = std::min( n, v.size() );
  std::partial_sort( v.begin(), v.begin() + n, v.end(), []( player_t* a, player_t* b ) {
    return a->resources.pct( RESOURCE_HEALTH ) < b->resources.pct( RESOURCE_HEALTH );
  } );
  v.resize( n );
  return v;
}

struct cosmic_ripple_t final : public priest_heal_t
{
  double scale = 1.0;

  cosmic_ripple_t( priest_t& p ) : priest_heal_t( "cosmic_ripple", p, p.find_spell( 243241 ) )
  {
    background = true;
    harmful    = false;
    auto_target  = false;
    holy_mastery = true;
    smart_aoe    = true;
    aoe          = std::max( 1, as<int>( p.talents.holy.cosmic_ripple->effectN( 1 ).base_value() ) );
  }

  double composite_da_multiplier( const action_state_t* s ) const override
  {
    return priest_heal_t::composite_da_multiplier( s ) * scale;
  }
};

struct dispersing_light_t final : public priest_heal_t
{
  int targets;

  dispersing_light_t( priest_t& p )
    : priest_heal_t( "dispersing_light", p, p.find_spell( 1215266 ) ),
      targets( std::max( 1, as<int>( p.talents.holy.dispersing_light->effectN( 2 ).base_value() ) ) )
  {
    background = proc = true;
    harmful = false;
    may_crit = callbacks = false;
    divine_aegis = false;
    auto_target  = false;
    aoe          = 0;
  }

  void init() override
  {
    priest_heal_t::init();
    snapshot_flags = update_flags = 0;  // a copy of the Flash Heal, no new modifiers
  }

  void spread( player_t* primary, double raw )
  {
    for ( auto* t : holy_injured_allies( p(), primary, as<size_t>( targets ) ) )
      execute_on_target( t, raw * p().talents.holy.dispersing_light->effectN( 1 ).percent() );
  }
};

struct holy_renew_t final : public priest_heal_t
{
  holy_renew_t( priest_t& p ) : priest_heal_t( "renew", p, p.find_spell( 139 ) )
  {
    background = true;
    harmful    = false;
    auto_target = false;
    aoe         = 0;
  }

  double composite_ta_multiplier( const action_state_t* s ) const override
  {
    return priest_heal_t::composite_ta_multiplier( s ) * ( 1.0 + p().cache.mastery_value() );
  }

  double cost() const override
  {
    return 0;
  }

  void apply( player_t* t, timespan_t base_duration )
  {
    double mult = 1.0;
    if ( p().talents.oracle.preemptive_care.enabled() )
      mult += p().talents.oracle.preemptive_care->effectN( 2 ).percent();
    dot_duration = base_duration * mult;
    execute_on_target( t );
  }
};

struct simple_holy_heal_t final : public priest_heal_t
{
  simple_holy_heal_t( priest_t& p, util::string_view n, unsigned id ) : priest_heal_t( n, p, p.find_spell( id ) )
  {
    background = proc = true;
    harmful      = false;
    auto_target  = false;
    holy_mastery = true;
    aoe          = 0;
  }
};

struct pom_shield_t final : public priest_absorb_t
{
  pom_shield_t( priest_t& p ) : priest_absorb_t( "power_word_shield_assured_safety", p, p.find_spell( 1246768 ) )
  {
    background = proc = true;
    harmful = false;
    aoe     = 0;
  }
};

// Divine Hymn: a channel that heals everyone each pulse; every pulse stacks +4% healing taken on all allies
struct divine_hymn_pulse_t final : public priest_heal_t
{
  divine_hymn_pulse_t( priest_t& p ) : priest_heal_t( "divine_hymn_pulse", p, p.find_spell( 64844 ) )
  {
    background = true;
    harmful      = false;
    auto_target  = false;
    holy_mastery = true;
    aoe          = -1;
  }

  // the spell data coefficient is the whole channel's worth; a pulse is 0.26 (logs: ~1.2 per pulse)
  double composite_da_multiplier( const action_state_t* s ) const override
  {
    return priest_heal_t::composite_da_multiplier( s ) * 0.26;
  }
};

struct divine_hymn_t final : public priest_heal_t
{
  divine_hymn_pulse_t* pulse;

  divine_hymn_t( priest_t& p, util::string_view options_str )
    : priest_heal_t( "divine_hymn", p, p.find_spell( 64843 ) ), pulse( new divine_hymn_pulse_t( p ) )
  {
    parse_options( options_str );
    harmful       = false;
    channeled     = true;
    tick_zero     = true;
    hasted_ticks  = true;
    dot_duration  = data().duration();
    base_tick_time = timespan_t::from_millis( 1250 );
    add_child( pulse );
  }

  void execute() override
  {
    priest().hymn_stacks = 0;
    priest_heal_t::execute();
  }

  void tick( dot_t* d ) override
  {
    priest_heal_t::tick( d );
    auto inj = holy_injured_allies( p(), nullptr, 1 );
    if ( !inj.empty() )
      pulse->execute_on_target( inj[ 0 ] );
    p().hymn_stacks     = std::min( 5, p().hymn_stacks + 1 );
    p().hymn_until      = sim->current_time() + 15_s;
    if ( p().talents.holy.gales_of_song.enabled() )
    {
      int stacks = as<int>( p().talents.holy.gales_of_song->effectN( 2 ).base_value() );
      size_t n   = as<size_t>( p().talents.holy.gales_of_song->effectN( 3 ).base_value() );
      for ( auto* t : holy_injured_allies( p(), nullptr, 8 ) )
      {
        if ( n == 0 )
          break;
        p().trigger_prayer_of_mending( t, stacks );
        n--;
      }
    }
    if ( p().talents.holy.benediction_serenity.enabled() )
      p().trigger_cosmic_ripple( p().talents.holy.benediction_serenity->effectN( 1 ).percent() );
  }
};

// wowhub: Divine Image's Naaru casts a heal beside yours (spell ids 196813 Blessed Light, 196809 Healing Light)
struct di_heal_t final : public priest_heal_t
{
  di_heal_t( priest_t& p, util::string_view n, unsigned id ) : priest_heal_t( n, p, p.find_spell( id ) )
  {
    background = proc = true;
    harmful      = false;
    auto_target  = false;
    holy_mastery = false;
    aoe          = 0;
  }

  double composite_da_multiplier( const action_state_t* s ) const override
  {
    return priest_heal_t::composite_da_multiplier( s ) * scale;
  }

  double scale = 1.0;
};

struct holy_prayer_of_mending_t final : public priest_heal_t
{
  cooldown_t* epiphany_cooldown;

  holy_prayer_of_mending_t( priest_t& p, util::string_view options_str )
    : priest_heal_t( "prayer_of_mending", p, p.find_spell( 33076 ) )
  {
    parse_options( options_str );
    harmful = false;
    epiphany_cooldown = p.get_cooldown( "prayer_of_mending_epiphany", this );
  }

  // wowhub: the ward goes on one of the few most injured allies, not always the same one
  player_t* pick_target() override
  {
    // wowhub: in a five-man group the ward goes on the hurt one: one of the two most injured
    size_t pool = p().sim->healing_no_pet_list.size() <= 6 ? as<size_t>( p().holy_tune( "pom_pool", 2 ) ) : 5;
    auto inj = holy_injured_allies( p(), nullptr, pool );
    return inj.empty() ? priest_heal_t::pick_target() : inj[ rng().range( inj.size() ) ];
  }

  // wowhub: Epiphany (a Holy Word gives 25%): the next cast ignores the cooldown and leaves it running
  bool ready() override
  {
    if ( p().buffs.epiphany->check() )
    {
      cooldown_t* original = cooldown;
      cooldown             = epiphany_cooldown;
      bool r               = priest_heal_t::ready();
      cooldown             = original;
      return r;
    }
    return priest_heal_t::ready();
  }

  void queue_execute( execute_type type ) override
  {
    cooldown_t* original = cooldown;
    if ( p().buffs.epiphany->check() )
      cooldown = epiphany_cooldown;
    priest_heal_t::queue_execute( type );
    cooldown = original;
  }

  void update_ready( timespan_t cd_duration = timespan_t::min() ) override
  {
    if ( p().buffs.epiphany->check() )
      return;
    priest_heal_t::update_ready( cd_duration );
  }

  void execute() override
  {
    bool free_cast = p().buffs.epiphany->check();
    priest_heal_t::execute();
    if ( free_cast )
      p().buffs.epiphany->expire();
    priest().holy_pom_cast_extras( target );
    priest().divine_image_heals( true );
  }
};
}  // namespace actions::heals
namespace actions::spells
{
struct apotheosis_t final : public priest_spell_t
{
  apotheosis_t( priest_t& p, util::string_view options_str )
    : priest_spell_t( "apotheosis", p, p.talents.holy.apotheosis )
  {
    parse_options( options_str );
    harmful = false;
  }

  void execute() override
  {
    priest_spell_t::execute();

    priest().buffs.apotheosis->trigger();
    priest().cooldowns.holy_word_chastise->reset( false );
    priest().cooldowns.holy_word_serenity->reset( false, 1 );
    priest().cooldowns.holy_word_sanctify->reset( false, 1 );
  }
};

// holy word cast -> Divine Image [Talent] (392988) -> Divine Image [Buff] (405963) -> Divine Image [Summon] (392990) ->
// Searing Light (196811) procs from Holy Fire, Chastise, Shadow Word: Pain, Shadow Word: Death, and Smite
struct searing_light_di_t final : public priest_spell_t
{
  searing_light_di_t( priest_t& p )
    : priest_spell_t( "searing_light_divine_image", p, p.talents.holy.divine_image_searing_light )
  {
    background = true;
    may_miss   = false;
  }
};

// holy word cast -> Divine Image [Talent] (392988) -> Divine Image [Buff] (405963) -> Divine Image [Summon] (392990) ->
// Light Eruption (196811) procs from Holy Nova casts
struct light_eruption_t final : public priest_spell_t
{
  light_eruption_t( priest_t& p ) : priest_spell_t( "light_eruption", p, p.talents.holy.divine_image_light_eruption )
  {
    background = true;
    may_miss   = false;
    aoe        = -1;
    radius     = 8;  // Base in spell data is incorrect (100yd)
  }
};

struct burning_vehemence_t final : public priest_spell_t
{
  burning_vehemence_t( priest_t& p ) : priest_spell_t( "burning_vehemence", p, p.talents.holy.burning_vehemence_damage )
  {
    background             = true;
    may_crit               = false;
    may_miss               = false;
    aoe                    = -1;
    full_amount_targets    = as<int>( priest().talents.holy.burning_vehemence_damage->effectN( 2 ).base_value() );
    reduced_aoe_targets    = priest().talents.holy.burning_vehemence_damage->effectN( 2 ).base_value();
    base_multiplier        = priest().talents.holy.burning_vehemence->effectN( 3 ).percent();
    target_filter_callback = secondary_targets_only();
  }

  void trigger( double initial_hit_damage )
  {
    if ( target_list().size() == 0 )
      return;

    double bv_damage = initial_hit_damage * base_multiplier;
    sim->print_debug(
        "burning_vehemence splash damage calculating: initial holy_fire hit: {}, multiplier: {}, result: {}",
        initial_hit_damage, base_multiplier, bv_damage );
    base_dd_min = base_dd_max = bv_damage;
    execute();
  }
};

struct holy_fire_t final : public priest_spell_t
{
  propagate_const<burning_vehemence_t*> child_burning_vehemence;
  action_t* child_searing_light;
  cooldown_t* dummy_cooldown;

  holy_fire_t( priest_t& p, util::string_view options_str )
    : priest_spell_t( "holy_fire", p, p.talents.holy_fire ),
      child_burning_vehemence( nullptr ),
      child_searing_light( priest().background_actions.searing_light )
  {
    if ( proc )
    {
      base_dd_max            = 0.0;
      base_dd_min            = 0.0;
      spell_power_mod.direct = 0;
    }

    if ( p.talents.holy.empyreal_blaze.enabled() )
    {
      dot_behavior = DOT_EXTEND;
    }

    if ( priest().talents.holy.burning_vehemence.enabled() && !proc )
    {
      child_burning_vehemence = new burning_vehemence_t( priest() );
      add_child( child_burning_vehemence );
    }

    dummy_cooldown = p.get_cooldown( "holy_fire_empyreal", this );
    dummy_cooldown->add_execute_type( execute_type::FOREGROUND );

    parse_options( options_str );
  }

  void queue_execute( execute_type type ) override
  {
    cooldown_t* original_cd = cooldown;
    if ( p().buffs.empyreal_blaze->check() )
    {
      cooldown = dummy_cooldown;
    }
    priest_spell_t::queue_execute( type );
    cooldown = original_cd;
  }

  double cost() const override
  {
    if ( priest().buffs.empyreal_blaze->check() || proc )
    {
      return 0;
    }

    return priest_spell_t::cost();
  }

  timespan_t execute_time() const override
  {
    if ( priest().buffs.empyreal_blaze->check() )
    {
      return 0_ms;
    }
    return priest_spell_t::execute_time();
  }

  // Brutal implementation of Ignore Spell Cooldown.
  bool ready() override
  {
    // Check conditions that do NOT pertain to the target before cycle_targets
    if ( !cooldown->is_ready() && !p().buffs.empyreal_blaze->check() )
      return false;

    if ( internal_cooldown->down() )
      return false;

    if ( player->is_moving() && !usable_moving() )
      return false;

    auto resource = current_resource();
    if ( resource != RESOURCE_NONE && !player->resource_available( resource, cost() ) )
    {
      if ( starved_proc )
        starved_proc->occur();
      return false;
    }

    if ( usable_while_casting )
    {
      if ( execute_time() > timespan_t::zero() )
      {
        return false;
      }

      // Don't allow cast-while-casting spells that trigger the GCD to be ready if the GCD is still
      // ongoing (during the cast)
      if ( ( player->executing || player->channeling ) && gcd() > timespan_t::zero() &&
           player->gcd_ready > sim->current_time() )
      {
        return false;
      }
    }

    return true;
  }

  void update_ready( timespan_t cd_duration /* = timespan_t::min() */ ) override
  {
    if ( proc )
      return;

    if ( cd_waste_data )
      cd_waste_data->add( cd_duration, time_to_execute );

    if ( ( cd_duration > 0_ms || ( cd_duration == timespan_t::min() && cooldown_duration() > 0_ms ) ) && !dual )
    {
      timespan_t delay = 0_ms;

      if ( !background && !proc )
      { /*This doesn't happen anymore due to the gcd queue, in WoD if an ability has a cooldown of 20 seconds,
        it is usable exactly every 20 seconds with proper Lag tolerance set in game.
        The only situation that this could happen is when world lag is over 400, as blizzard does not allow
        custom lag tolerance to go over 400.
        */
        delay = rng().gauss( player->world_lag );
        if ( delay > 400_ms )
        {
          delay -= 400_ms;  // Even high latency players get some benefit from CLT.
          sim->print_debug( "{} delaying the cooldown finish of {} by {}", *player, *this, delay );
        }
        else
          delay = 0_ms;
      }

      if ( !p().buffs.empyreal_blaze->check() )
      {
        cooldown->start( this, cd_duration, delay );

        sim->print_debug(
            "{} starts cooldown for {} ({}, {}/{}). Duration={} Delay={}. Will "
            "be ready at {}",
            *player, *this, *cooldown, cooldown->current_charge, cooldown->charges, cd_duration, delay,
            cooldown->ready );
      }

      if ( internal_cooldown->duration > 0_ms )
      {
        internal_cooldown->start( this );

        sim->print_debug( "{} starts internal_cooldown for {} ({}). Will be ready at {}", *player, *this,
                          *internal_cooldown, internal_cooldown->ready );
      }
    }
  }

  void execute() override
  {
    cooldown_t* original_cd = cooldown;

    if ( p().buffs.empyreal_blaze->up() )
    {
      cooldown = dummy_cooldown;
    }

    priest_spell_t::execute();

    if ( !proc )
      priest().buffs.empyreal_blaze->decrement();

    cooldown = original_cd;

    if ( priest().talents.holy.holy_word_chastise.enabled() && priest().talents.holy.voice_of_harmony.enabled() )
    {
      timespan_t chastise_cdr =
          timespan_t::from_seconds( priest().talents.holy.voice_of_harmony->effectN( 3 ).base_value() );

      priest().do_holy_word_cdr( priest().cooldowns.holy_word_chastise, chastise_cdr );
    }
  }

  void impact( action_state_t* s ) override
  {
    priest_spell_t::impact( s );
    if ( result_is_hit( s->result ) )
    {
      if ( child_searing_light && priest().buffs.divine_image->up() )
      {
        for ( int i = 1; i <= priest().buffs.divine_image->stack(); i++ )
        {
          child_searing_light->execute();
        }
      }
    }

    if ( child_burning_vehemence )
    {
      child_burning_vehemence->trigger( s->result_amount );
    }
  }
};

struct holy_word_chastise_t final : public priest_spell_t
{
  action_t* child_searing_light;
  holy_word_chastise_t( priest_t& p, util::string_view options_str )
    : priest_spell_t( "holy_word_chastise", p, p.talents.holy.holy_word_chastise ),
      child_searing_light( priest().background_actions.searing_light )
  {
    parse_options( options_str );
  }

  double cost() const override
  {
    if ( priest().buffs.apotheosis->check() )
    {
      return 0;
    }

    return priest_spell_t::cost();
  }

  void execute() override
  {
    priest_spell_t::execute();

    priest().trigger_epiphany();

    if ( priest().talents.holy.divine_image.enabled() )
    {
      priest().buffs.divine_image->trigger();
      priest().procs.divine_image->occur();
      // Activating cast also immediately executes searing light
      for ( int i = 1; i <= priest().buffs.divine_image->stack(); i++ )
      {
        child_searing_light->execute();
      }
    }

    if ( priest().talents.holy.empyreal_blaze.ok() )
    {
      priest().buffs.empyreal_blaze->trigger();
    }
  }

  void impact( action_state_t* s ) override
  {
    priest_spell_t::impact( s );
    sim->print_debug( "divine_image_buff: {}", priest().buffs.divine_image->up() );
    if ( child_searing_light && priest().buffs.divine_image->up() )
    {
      for ( int i = 1; i <= priest().buffs.divine_image->stack(); i++ )
      {
        child_searing_light->execute();
      }
    }
  }
};

struct guardian_spirit_t final : public priest_spell_t
{
  target_specific_t<buff_t> buff_initialized;

  guardian_spirit_t( priest_t& p, std::string_view options_str )
    : priest_spell_t( "guardian_spirit", p, p.talents.holy.guardian_spirit )
  {
    parse_options( options_str );

    harmful = false;

    target_debuff = p.talents.holy.guardian_spirit;
  }

  buff_t* create_debuff( player_t* t ) override
  {
    auto gs_buff = priest_spell_t::create_debuff( t )
                       ->set_default_value_from_effect_type( A_MOD_HEALING_RECEIVED_PCT )
                       ->set_cooldown( 0_ms );  // Let the ability handle the CD

    t->assessor_out_damage.add( assessor::LOG - 1, [ gs_buff, t ]( auto, action_state_t* s ) {
      auto max_hp = t->resources.max[ RESOURCE_HEALTH ];
      auto cur_hp = t->resources.current[ RESOURCE_HEALTH ];
      auto new_hp = std::max( 0.0, cur_hp - s->result_amount );

      if ( gs_buff->check() && new_hp / max_hp * 100.0 <= t->death_pct && s->result_amount <= max_hp * 2.0 &&
           !t->resources.is_infinite( RESOURCE_HEALTH ) && !t->demise_event )
      {
        s->result_amount = cur_hp - new_hp + 1;
        gs_buff->expire();
      }

      return assessor::CONTINUE;
    } );

    return gs_buff;
  }

  void execute() override
  {
    priest_spell_t::execute();

    get_debuff( target )->trigger();
  }
};

}  // namespace actions::spells

namespace buffs
{
}  // namespace buffs

void priest_t::do_holy_word_cdr( cooldown_t* cd, timespan_t amount, bool affected_by_apotheosis,
                                 bool affected_by_naaru )
{
  if ( !cd )
    return;

  if ( affected_by_apotheosis && buffs.apotheosis->check() )
  {
    amount *= 1 + buffs.apotheosis->value();
  }

  if ( affected_by_naaru && talents.holy.light_of_the_naaru.enabled() )
  {
    amount *= 1 + talents.holy.light_of_the_naaru->effectN( 1 ).percent();
  }

  if ( sim->debug )
    sim->print_debug( "{} adjusted cooldown of {}, by {}, with light_of_the_naaru: {} - {}, apotheosis: {} - {}.",
                      *this, *cd, amount, talents.holy.light_of_the_naaru.rank(), affected_by_naaru,
                      buffs.apotheosis->check(), affected_by_apotheosis );

  cd->adjust( -amount );
}

void priest_t::create_buffs_holy()
{
  buffs.apotheosis = make_buff( this, "apotheosis", talents.holy.apotheosis )->set_default_value_from_effect( 1, 0.01 );

  buffs.benediction = make_buff( this, "benediction", find_spell( 1262766 ) );

  buffs.empyreal_blaze = make_buff( this, "empyreal_blaze", talents.holy.empyreal_blaze_buff )->set_reverse( true );

  buffs.divine_favor_chastise = make_buff( this, "divine_favor_chastise", talents.holy.divine_favor_chastise );

  buffs.epiphany = make_buff( this, "epiphany", find_spell( 414556 ) );

  buffs.divine_image = make_buff( this, "divine_image", talents.holy.divine_image_buff )
                           ->set_refresh_behavior( buff_refresh_behavior::DURATION )
                           ->set_stack_behavior( buff_stack_behavior::ASYNCHRONOUS );
}

void priest_t::init_rng_holy()
{
}

void priest_t::init_spells_holy()
{
  auto ST = [ this ]( std::string_view n ) { return find_talent_spell( talent_tree::SPECIALIZATION, n ); };
  // Row 1
  talents.holy.holy_word_serenity = ST( "Holy Word: Serenity" );
  // Row 2
  talents.holy.holy_word_sanctify = ST( "Holy Word: Sanctify" );
  talents.holy.guardian_spirit    = ST( "Guardian Spirit" );
  talents.holy.holy_word_chastise = ST( "Holy Word: Chastise" );
  // Row 3
  talents.holy.prayer_of_healing   = ST( "Prayer of Healing" );
  talents.holy.restitution         = ST( "Restitution" );
  talents.holy.guardian_angel      = ST( "Guardian Angel" );
  talents.holy.censure             = ST( "Censure" );
  talents.holy.empyreal_blaze      = ST( "Empyreal Blaze" );
  talents.holy.empyreal_blaze_buff = find_spell( 372617 );
  // Row 4
  talents.holy.prayerful_litany         = ST( "Prayerful Litany" );  // NYI
  talents.holy.cosmic_ripple            = ST( "Cosmic Ripple" );
  talents.holy.afterlife                = ST( "Afterlife" );
  talents.holy.voice_of_harmony         = ST( "Voice of Harmony" );
  talents.holy.burning_vehemence        = ST( "Burning Vehemence" );
  talents.holy.burning_vehemence_damage = find_spell( 400370 );
  // Row 5
  talents.holy.uplifting_words = ST( "Uplifting Words" );  // NYI
  talents.holy.cosmic_wave     = ST( "Cosmic Wave" );      // NYI
  talents.holy.divine_hymn     = ST( "Divine Hymn" );
  talents.holy.enlightenment   = ST( "Enlightenment" );
  talents.holy.benediction     = ST( "Benediction" );
  // Row 6
  talents.holy.efficient_prayers  = ST( "Efficient Prayers" );  // NYI
  talents.holy.healing_focus      = ST( "Healing Focus" );      // NYI
  talents.holy.seraphic_crescendo = ST( "Seraphic Crescendo" );
  talents.holy.gales_of_song      = ST( "Gales of Song" );
  talents.holy.divine_service     = ST( "Divine Service" );
  talents.holy.renewed_faith      = ST( "Renewed Faith" );
  // Row 7
  talents.holy.angelic_touch           = ST( "Angelic Touch" );  // NYI
  talents.holy.apotheosis              = ST( "Apotheosis" );
  talents.holy.prayers_of_the_virtuous = ST( "Prayers of the Virtuous" );
  // Row 8
  talents.holy.dispersing_light = ST( "Dispersing Light" );
  talents.holy.trail_of_light   = ST( "Trail of Light" );
  talents.holy.miracle_worker   = ST( "Miracle Worker" );
  talents.holy.eternal_sanctity = ST( "Eternal Sanctity" );
  talents.holy.divinity         = ST( "Divinity" );
  talents.holy.holy_celerity    = ST( "Holy Celerity" );
  talents.holy.say_your_prayers = ST( "Say Your Prayers" );
  // Row 9
  talents.holy.crisis_management     = ST( "Crisis Management" );
  talents.holy.light_of_the_naaru    = ST( "Light of the Naaru" );
  talents.holy.light_in_the_darkness = ST( "Light in the Darkness" );
  talents.holy.prismatic_echoes      = ST( "Prismatic Echoes" );
  talents.holy.desperate_times       = ST( "Desperate Times" );
  talents.holy.radiant_plea          = ST( "Radiant Plea" );  // NYI
  // Row 10
  talents.holy.lightweaver                 = ST( "Lightweaver" );
  talents.holy.ultimate_serenity           = ST( "Ultimate Serenity" );  // NYI
  talents.holy.divine_image                = ST( "Divine Image" );
  talents.holy.divine_image_buff           = find_spell( 405963 );
  talents.holy.divine_image_summon         = find_spell( 392990 );
  talents.holy.divine_image_searing_light  = find_spell( 196811 );
  talents.holy.divine_image_light_eruption = find_spell( 196812 );
  talents.holy.lasting_words               = ST( "Lasting Words" );
  talents.holy.divine_favor_chastise       = find_spell( 372761 );
  talents.holy.epiphany                    = ST( "Epiphany" );

  // wowhub: Benediction is one node with three choices that are all taken
  talents.holy.benediction_pom      = find_talent_spell( 136995u );
  talents.holy.benediction_ripple   = find_talent_spell( 136994u );
  talents.holy.benediction_serenity = find_talent_spell( 136993u );
  sim->print_debug( "{} Benediction nodes: pom={} (id {}) ripple={} (id {}) serenity={} (id {})", *this,
                    talents.holy.benediction_pom.enabled(), talents.holy.benediction_pom->id(),
                    talents.holy.benediction_ripple.enabled(), talents.holy.benediction_ripple->id(),
                    talents.holy.benediction_serenity.enabled(), talents.holy.benediction_serenity->id() );
  talents.holy.lights_resurgence = find_talent_spell( talent_tree::SPECIALIZATION, 193157 );

  // General Spells
  specs.echo_of_light = find_spell( 77489 );
}

action_t* priest_t::create_action_holy( util::string_view name, util::string_view options_str )
{
  using namespace actions::spells;
  using namespace actions::heals;

  if ( name == "holy_fire" )
  {
    return new holy_fire_t( *this, options_str );
  }
  if ( name == "apotheosis" )
  {
    return new apotheosis_t( *this, options_str );
  }
  if ( name == "holy_word_chastise" )
  {
    return new holy_word_chastise_t( *this, options_str );
  }
  if ( name == "holy_word_serenity" )
  {
    return new holy_word_serenity_t( *this, options_str );
  }
  if ( name == "holy_word_sanctify" )
  {
    return new holy_word_sanctify_t( *this, options_str );
  }
  if ( name == "guardian_spirit" )
  {
    return new guardian_spirit_t( *this, options_str );
  }
  if ( name == "divine_hymn" && specialization() == PRIEST_HOLY )
  {
    return new divine_hymn_t( *this, options_str );
  }
  if ( name == "prayer_of_mending" && specialization() == PRIEST_HOLY )
  {
    return new holy_prayer_of_mending_t( *this, options_str );
  }

  return nullptr;
}

void priest_t::init_background_actions_holy()
{
  if ( talents.holy.divine_image.enabled() )
  {
    background_actions.searing_light  = new actions::spells::searing_light_di_t( *this );
    background_actions.light_eruption = new actions::spells::light_eruption_t( *this );
  }

  background_actions.echo_of_light = new actions::heals::echo_of_light_t( *this );

  if ( holy_heal_mode() )
  {
    using namespace actions::heals;
    if ( talents.holy.cosmic_ripple.enabled() )
      background_actions.cosmic_ripple = new cosmic_ripple_t( *this );
    if ( talents.holy.dispersing_light.enabled() )
      background_actions.dispersing_light = new dispersing_light_t( *this );
    background_actions.holy_renew = new holy_renew_t( *this );
    if ( talents.oracle.prompt_prognosis.enabled() )
      background_actions.prompt_prognosis = new simple_holy_heal_t( *this, "prompt_prognosis", 1246798 );
    if ( talents.oracle.assured_safety.enabled() )
      background_actions.pom_shield = new pom_shield_t( *this );
    if ( talents.holy.divine_image.enabled() )
    {
      background_actions.di_blessed_light = new di_heal_t( *this, "divine_image_blessed_light", 196813 );
      background_actions.di_healing_light = new di_heal_t( *this, "divine_image_healing_light", 196809 );
    }
    if ( talents.holy.ultimate_serenity.enabled() )
      background_actions.ultimate_serenity = new simple_holy_heal_t( *this, "ultimate_serenity", 1250581 );
  }
}

bool priest_t::holy_heal_mode() const
{
  return specialization() == PRIEST_HOLY && primary_role() == ROLE_HEAL && sim->heal_target;
}

void priest_t::trigger_cosmic_ripple( double scale )
{
  auto* a = debug_cast<actions::heals::cosmic_ripple_t*>( background_actions.cosmic_ripple.get() );
  if ( !a )
    return;
  auto inj = actions::heals::holy_injured_allies( *this, nullptr, 1 );
  if ( inj.empty() )
    return;
  a->scale = scale;
  a->execute_on_target( inj[ 0 ] );
}

void priest_t::trigger_dispersing_light( player_t* primary, double raw )
{
  auto* a = debug_cast<actions::heals::dispersing_light_t*>( background_actions.dispersing_light.get() );
  if ( a )
    a->spread( primary, raw );
}

void priest_t::trigger_holy_renew( player_t* target, timespan_t duration )
{
  auto* a = debug_cast<actions::heals::holy_renew_t*>( background_actions.holy_renew.get() );
  if ( !a )
    return;
  if ( duration <= 0_s )
    duration = a->data().duration();
  a->apply( target, duration );
}

double priest_t::holy_tune( util::string_view name, double def ) const
{
  for ( auto& kv : util::string_split<std::string_view>( options.holy_tune, "/" ) )
  {
    auto pr = util::string_split<std::string_view>( kv, "=" );
    if ( pr.size() == 2 && pr[ 0 ] == name )
      return util::to_double( pr[ 1 ] );
  }
  return def;
}

// wowhub: Epiphany - a Holy Word has a chance to make the next Prayer of Mending ignore its cooldown
void priest_t::trigger_epiphany()
{
  if ( talents.holy.epiphany.enabled() && rng().roll( talents.holy.epiphany->effectN( 1 ).percent() ) )
    buffs.epiphany->trigger();
}

// wowhub: Divine Image - while the image is up, its Naaru heals beside your Holy Word: Serenity and Flash Heal
// (Healing Light, once per cast) and Prayer of Mending (Blessed Light, about two heals 0.3-1.7 s after the cast)
void priest_t::divine_image_heals( bool blessed )
{
  if ( !holy_heal_mode() || !buffs.divine_image->check() )
    return;
  if ( !blessed )
  {
    auto* a = debug_cast<actions::heals::di_heal_t*>( background_actions.di_healing_light.get() );
    if ( !a )
      return;
    a->scale = holy_tune( "di_healing", 0.85 );
    auto inj = actions::heals::holy_injured_allies( *this, nullptr, 3 );
    if ( !inj.empty() )
      a->execute_on_target( inj[ rng().range( inj.size() ) ] );
    return;
  }
  auto* a = debug_cast<actions::heals::di_heal_t*>( background_actions.di_blessed_light.get() );
  if ( !a )
    return;
  double n_mean = holy_tune( "di_blessed_n", 2.2 );
  int n         = as<int>( n_mean );
  if ( rng().roll( n_mean - n ) )
    n++;
  for ( int i = 0; i < n; i++ )
  {
    timespan_t delay = timespan_t::from_millis( 300 + rng().range( 1400 ) );
    make_event( *sim, delay, [ this, a ] {
      a->scale = holy_tune( "di_blessed", 1.2 );
      auto inj = actions::heals::holy_injured_allies( *this, nullptr, 3 );
      if ( !inj.empty() )
        a->execute_on_target( inj[ rng().range( inj.size() ) ] );
    } );
  }
}

// Holy Word: Serenity: Ultimate Serenity heals, Lasting Words Renews, Cosmic Ripple
void priest_t::holy_serenity_extras( player_t* target )
{
  if ( !holy_heal_mode() )
    return;

  if ( background_actions.ultimate_serenity )
  {
    for ( auto* t : actions::heals::holy_injured_allies( *this, target, 4 ) )
    {
      background_actions.ultimate_serenity->execute_on_target( t );
      if ( talents.holy.lasting_words.enabled() )
        trigger_holy_renew( t, timespan_t::from_millis( talents.holy.lasting_words->effectN( 2 ).base_value() ) );
    }
  }
  if ( talents.holy.lasting_words.enabled() )
    trigger_holy_renew( target, timespan_t::from_millis( talents.holy.lasting_words->effectN( 1 ).base_value() ) );

  if ( talents.holy.cosmic_ripple.enabled() )
    trigger_cosmic_ripple();

  // wowhub: in the Mythic+ build logs the upgrade follows about 1 in 6 Serenity casts (the raid build: every one)
  double bene_roll = talents.holy.benediction_serenity->effectN( 2 ).percent();
  if ( talents.holy.divine_image.enabled() || talents.holy.burning_vehemence.enabled() )
    bene_roll = holy_tune( "bene_serenity", 0.15 );
  if ( talents.holy.benediction_serenity.enabled() && rng().roll( bene_roll ) )
    buffs.benediction->trigger();
}

// Prayer of Mending cast: the ward, Twinsight, Prompt Prognosis, Assured Safety, Voice of Harmony
void priest_t::holy_pom_cast_extras( player_t* target )
{
  int stacks = as<int>( find_spell( 33076 )->effectN( 1 ).base_value() );
  if ( talents.holy.prayers_of_the_virtuous.enabled() )
    stacks += as<int>( talents.holy.prayers_of_the_virtuous->effectN( 1 ).base_value() );
  trigger_prayer_of_mending( target, stacks );

  if ( talents.oracle.twinsight.enabled() )
  {
    auto inj = actions::heals::holy_injured_allies( *this, target, 3 );
    if ( !inj.empty() )
      trigger_prayer_of_mending( inj[ rng().range( inj.size() ) ],
                                 as<int>( talents.oracle.twinsight->effectN( 1 ).base_value() ) );
  }
  if ( background_actions.prompt_prognosis )
    background_actions.prompt_prognosis->execute_on_target( target );
  if ( background_actions.pom_shield )
    background_actions.pom_shield->execute_on_target( target );
  if ( talents.holy.voice_of_harmony.enabled() )
    do_holy_word_cdr( cooldowns.holy_word_serenity,
                      timespan_t::from_seconds( talents.holy.voice_of_harmony->effectN( 2 ).base_value() ) );
}

std::unique_ptr<expr_t> priest_t::create_expression_holy( util::string_view name_str )
{
  if ( !holy_heal_mode() )
    return nullptr;
  // wowhub: raid health for the healing priority (same names as the Restoration Shaman's)
  auto count_below = [ this ]( double pct ) {
    return [ this, pct ]() {
      int n = 0;
      for ( auto* t : sim->healing_no_pet_list )
        if ( !t->is_sleeping() && t->resources.pct( RESOURCE_HEALTH ) < pct )
          n++;
      return as<double>( n );
    };
  };
  if ( util::str_compare_ci( name_str, "injured_95" ) )
    return make_fn_expr( name_str, count_below( 0.95 ) );
  if ( util::str_compare_ci( name_str, "injured_90" ) )
    return make_fn_expr( name_str, count_below( 0.90 ) );
  if ( util::str_compare_ci( name_str, "injured_80" ) )
    return make_fn_expr( name_str, count_below( 0.80 ) );
  if ( util::str_compare_ci( name_str, "injured_70" ) )
    return make_fn_expr( name_str, count_below( 0.70 ) );
  if ( util::str_compare_ci( name_str, "injured_60" ) )
    return make_fn_expr( name_str, count_below( 0.60 ) );
  if ( util::str_compare_ci( name_str, "raid_deficit" ) )  // average missing health of the group, in percent
    return make_fn_expr( name_str, [ this ]() {
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
  return nullptr;
}


// ==========================================================================
// wowhub: Holy healing priority list
// ==========================================================================
void priest_t::apl_holy_heal()
{
  action_priority_list_t* default_  = get_action_priority_list( "default" );
  action_priority_list_t* precombat = get_action_priority_list( "precombat" );

  precombat->add_action( "snapshot_stats" );
  precombat->add_action( "variable,name=trinket_1_ogcd_cast,value=trinket.1.is.soulcoiler_ritual_vessel" );
  precombat->add_action( "variable,name=trinket_2_ogcd_cast,value=trinket.2.is.soulcoiler_ritual_vessel" );
  precombat->add_action( "prayer_of_mending" );

  default_->add_action( "power_infusion" );
  default_->add_action( "invoke_external_buff,name=power_infusion" );
  default_->add_action( "potion" );
  default_->add_action( "desperate_prayer,if=health.pct<=40" );
  default_->add_action( "arcane_torrent,if=mana.pct<=95" );
  default_->add_action( "use_item,slot=trinket1,if=!variable.trinket_1_ogcd_cast" );
  default_->add_action( "use_item,slot=trinket2,if=!variable.trinket_2_ogcd_cast" );
  default_->add_action( "use_item,use_off_gcd=1,slot=trinket1,if=variable.trinket_1_ogcd_cast&gcd.remains>=0.75*gcd.max" );
  default_->add_action( "use_item,use_off_gcd=1,slot=trinket2,if=variable.trinket_2_ogcd_cast&gcd.remains>=0.75*gcd.max" );
  // wowhub: the Mythic+ build (Divine Image / Burning Vehemence) weaves damage between the heals: Holy Fire off its
  // cooldown and Smite as the filler, both feed the Holy Words; Divine Hymn waits for a hurt group
  bool mplus = talents.holy.divine_image.enabled() || talents.holy.burning_vehemence.enabled();
  if ( mplus )
  {
    default_->add_action( "divine_hymn,if=time>30&injured_80>=2" );
    default_->add_action( "holy_word_chastise" );
    default_->add_action( "holy_fire,if=buff.empyreal_blaze.up" );
    default_->add_action( "apotheosis,if=cooldown.holy_word_serenity.charges_fractional<1.5" );
    default_->add_action( "holy_word_serenity" );
    default_->add_action( "prayer_of_mending" );
    default_->add_action( "benediction" );
    default_->add_action( "holy_fire" );
    default_->add_action( "flash_heal,if=mana.pct>=90*fight_remains%(time+fight_remains)" );
    default_->add_action( "smite" );
    return;
  }

  default_->add_action( "divine_hymn,if=time>30" );
  default_->add_action( "holy_word_chastise" );
  default_->add_action( "holy_fire,if=buff.empyreal_blaze.up" );
  default_->add_action( "apotheosis,if=cooldown.holy_word_serenity.charges_fractional<1.5" );
  default_->add_action( "holy_word_serenity" );
  default_->add_action( "prayer_of_mending" );
  default_->add_action( "benediction" );
  default_->add_action( "flash_heal,if=group_size>5&mana.pct>=80*fight_remains%(time+fight_remains)" );
  default_->add_action( "flash_heal,if=group_size<=5&mana.pct>=90*fight_remains%(time+fight_remains)" );
  default_->add_action( "smite,if=group_size<=5" );
}

}  // namespace priestspace
