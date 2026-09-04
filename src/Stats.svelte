<script>
  import { invoke } from './bridge.js';
  import { art } from './skin.svelte.js';
  import { listen } from './bridge.js';
  import { itemName, rarityByName, tierLabel, typeLabel, roomName, zoneLabel, zoneCode, DROP_ZONES, DROP_CHASE, DROP_RATE, RARITY_BY_NAME, TIER_BY_NAME, TIER_LETTERS } from './items.js';
  import { buffInfo, debuffInfo, zoneName } from './buffs.js';
  import { fmt, difficulty, RARITIES, RARITY_CLASS } from './format.js';

  let snap = $state(null);
  let extra = $state(null);

  // pushed by the backend, and only while this window is on screen
  let clock = $state({ secs: 0, at: Date.now() });
  let nowTick = $state(Date.now());
  function received(s) {
    const now = Date.now();
    snap = s;
    clock = { secs: s.session_secs, at: now };
    nowTick = now;
  }

  $effect(() => {
    let disposed = false;
    let statsRevision = 0;
    let extraRevision = 0;
    let settingsRevision = 0;
    const unsubs = [
      listen('settings-changed', (e) => {
        settingsRevision += 1;
        settings = e.payload;
      }),
      listen('stats', (e) => {
        statsRevision += 1;
        received(e.payload);
      }),
      listen('stats-extra', (e) => {
        extraRevision += 1;
        extra = e.payload;
      }),
    ];
    // Register every live listener first. A reply from one of these initial
    // reads is ignored if a newer push for the same state arrived meanwhile.
    Promise.all(unsubs).then(() => {
      const statsAt = statsRevision;
      const extraAt = extraRevision;
      const settingsAt = settingsRevision;
      invoke('snapshot').then((initial) => {
        if (!disposed && statsRevision === statsAt) received(initial);
      }).catch(() => {});
      invoke('get_extra').then((initial) => {
        if (!disposed && extraRevision === extraAt) extra = initial;
      }).catch(() => {});
      invoke('get_settings').then((initial) => {
        if (!disposed && settingsRevision === settingsAt) settings = initial;
      }).catch(() => {});
    }).catch(() => {});
    return () => {
      disposed = true;
      unsubs.forEach((u) => u.then((f) => f()).catch(() => {}));
    };
  });


  function dur(secs) {
    const h = Math.floor(secs / 3600);
    const m = Math.floor((secs % 3600) / 60);
    const s = Math.floor(secs % 60);
    return h > 0
      ? `${h}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`
      : `${m}:${String(s).padStart(2, '0')}`;
  }

  // One formatter, built once. `extra` arrives as a fresh object, so every one
  // of the journal's 400 rows re-runs its template on every push, and building
  // a Date and a fresh options literal per row costs 10.2ms of blocking main
  // thread per push — for timestamps that never change. The same 400 rows
  // through one hoisted Intl.DateTimeFormat: 0.23ms.
  const TIME = new Intl.DateTimeFormat('en-GB', {
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
  });
  const time = (ms) => TIME.format(ms);


  const item = (name) => snap?.items?.[name] ?? { total: 0, mf: 0, per_hour: 0 };

  // the character save counts these; they only appear once something has moved
  let bosses = $derived((snap?.tallies ?? []).filter((t) => t.group === 'boss'));
  let chests = $derived((snap?.tallies ?? []).filter((t) => t.group === 'chest'));

  let charSub = $derived.by(() => {
    const c = extra?.character;
    if (!c) return 'waiting for character…';
    const parts = [];
    if (c.name) parts.push(c.name);
    parts.push(`Lv ${c.level}`, `HLv ${c.herolevel}`, difficulty(c.difficulty, c.hell_sub));
    if (c.hardcore) parts.push('HC');
    return parts.join(' · ');
  });

  // XP and kills may arrive from a stable read-only save snapshot. Gold needs a
  // compatible local event source; a stale number is not proof the tracker froze.
  const ago = (secs) => (secs < 90 ? `${secs}s` : `${Math.floor(secs / 60)}m`);
  let lag = $derived.by(() => {
    const save = snap?.save_age_secs;
    const bank = snap?.bank_age_secs;
    const parts = [];
    if (save != null && save >= 45) parts.push(`character save ${ago(save)} ago`);
    if (bank != null && bank >= 45) parts.push(`balance ${ago(bank)} ago`);
    if (save == null && bank == null) {
      // the totals on screen are then last run's, marked with an asterisk
      return snap?.carried_bank || snap?.carried_totals
        ? 'waiting for save progress or a local event — XP and kills may arrive from saves; gold needs an event source; * marks totals carried over from the last run'
        : 'waiting for save progress or a local event — XP and kills may arrive from saves; gold needs an event source';
    }
    return parts.length ? `last from the game · ${parts.join(' · ')}` : '';
  });

  // The visible session clock advances between backend snapshots.
  $effect(() => {
    const t = setInterval(() => (nowTick = Date.now()), 1000);
    return () => clearInterval(t);
  });

  function dropLabel(d) {
    if (d.name) return d.name;
    const known = itemName(d.item_type, d.item_id, d.weapon_type);
    if (known) return known;
    if (d.item_id > 0) return `${typeLabel(d.item_type, d.weapon_type)} #${d.item_id}`;
    const parts = [];
    if (d.item_type > 0) parts.push(typeLabel(d.item_type, d.weapon_type));
    if (d.seed > 0) parts.push(`Seed ${String(d.seed).slice(-6)}`);
    return parts.join(' · ') || 'Unknown item';
  }

  function dropRarity(d) {
    if (d.rarity) return d.rarity;
    const byName = rarityByName(dropLabel(d));
    return byName ?? 'Drop';
  }

  const rarityCls = {
    Satanic: 'c-sat',
    Heroic: 'c-her',
    Angelic: 'c-ang',
    Unholy: 'c-unh',
    Mythic: 'c-myt',
    Set: 'c-set',
    Runeword: 'c-gold',
  };

  // A drop worth hearing next time is easiest to add the moment it lands, so
  // the timeline can push a name straight into a list of the active filter.
  let settings = $state(null);
  let adding = $state(null);

  /// Bring a just-opened popup fully into the scroller it lives in.
  ///
  /// `block: 'nearest'` moves the list as little as it can, so a picker that is
  /// already visible does not make the page jump under the cursor.
  function reveal(node) {
    node.scrollIntoView({ block: 'nearest', inline: 'nearest' });
  }
  let added = $state(null);
  let addedTimer;

  $effect(() => () => clearTimeout(addedTimer));

  let lists = $derived.by(() => {
    const filter = (settings?.filters ?? []).find((f) => f.id === settings?.filter);
    return filter?.lists ?? [];
  });

  function addTo(list, name) {
    adding = null;
    if (!name || list.items.some((n) => n.toLowerCase() === name.toLowerCase())) return;
    list.items = [...list.items, name].sort((a, b) => a.localeCompare(b));
    invoke('save_settings', { settings: $state.snapshot(settings) }).catch(() => {});
    added = `${name} → ${list.name}`;
    clearTimeout(addedTimer);
    addedTimer = setTimeout(() => (added = null), 2500);
  }

  // ---- the right column: where the character is and what that place offers
  let room = $derived(snap?.room ?? null);
  let roomTitle = $derived(room ? (roomName(room) ?? zoneLabel(room)) : (snap?.act ? `Act ${snap.act}` : 'unknown'));
  let roomSub = $derived.by(() => {
    const m = /^Act_(\d+)_(\d+)/i.exec(String(room ?? ''));
    if (m) return `Act ${Number(m[1])} · Zone ${Number(m[2])}`;
    if (/^Town/i.test(String(room ?? ''))) return 'town';
    return room ? String(room).replace(/_rm$/i, '') : 'waiting for the game';
  });
  let sz = $derived(snap?.satanic_zone ?? null);
  let szHere = $derived.by(() => {
    if (snap?.satanic_here) return true;
    const a = zoneCode(sz?.zone);
    const b = zoneCode(room);
    return !!a && a === b;
  });
  let szAgo = $derived.by(() => {
    const at = snap?.satanic_at;
    if (!at) return '';
    const mins = Math.max(0, Math.floor((nowTick - at) / 60000));
    return mins < 1 ? 'rotated just now' : `rotated ${mins}m ago`;
  });
  // The table's code for a room: "8-2" for act 8 zone 2, "8-D" for one of
  // its dungeons, "8-BD" for its boss dungeon. Town and menus have none.
  function areaCode(name) {
    const text = String(name ?? '');
    const zone = zoneCode(text);
    if (zone) return zone;
    const m = /^Act_(\d+)_/i.exec(text);
    if (!m) return null;
    if (/Boss/i.test(text)) return `${Number(m[1])}-BD`;
    if (/Dungeon|Cave|Tomb|Crypt|Mine|Vault/i.test(text)) return `${Number(m[1])}-D`;
    return null;
  }
  function itemsTiedTo(code) {
    if (!code) return [];
    const out = [];
    for (const [name, zones] of Object.entries(DROP_ZONES)) {
      if (!zones.includes(code)) continue;
      const chase = DROP_CHASE[name] ?? DROP_RATE[name] ?? 0;
      const rarity = RARITY_BY_NAME[name] ?? '';
      const tier = TIER_BY_NAME[name] ?? 0;
      out.push({ name, chase, rarity, tier });
    }
    out.sort((a, b) => (a.chase || 1e12) - (b.chase || 1e12));
    return out.slice(0, 12);
  }
  // Items the game ties to where the character stands, best odds first. In
  // town there is nothing to tie to, so the satanic zone's list stands in:
  // that is the place the character is about to go.
  let area = $derived.by(() => {
    const here = itemsTiedTo(areaCode(room));
    if (here.length) return { items: here, label: roomSub, from: 'here' };
    const szCode = zoneCode(sz?.zone);
    const there = itemsTiedTo(szCode);
    if (there.length) return { items: there, label: `satanic zone · ${zoneName(sz.zone)}`, from: 'satanic' };
    return { items: [], label: room ? roomSub : '', from: 'none' };
  });
  // word starts only: "tarethiel's" must not become "Tarethiel'S"
  const cap = (t) => t.replace(/(^|[\s(])([a-z])/g, (m, a, c) => a + c.toUpperCase());
  const tierName = (t) => (TIER_LETTERS && TIER_LETTERS[t]) ? TIER_LETTERS[t] : (t ? String(t) : '');
</script>

<div class="panel">
  <div class="body">
    <!-- what the run is doing right now: three numbers and the clock -->
    <div class="run" data-tauri-drag-region>
      <button
        class="clock"
        class:held={snap?.paused}
        style:border-image-source="url({art('chip_dark')})"
        onclick={() => invoke('set_paused', { paused: !snap?.paused }).catch(() => {})}
        title="Stop the clock. The counters keep counting — what a pause changes is what they are divided by. Ctrl+Shift+P"
      >
        <div class="value">
          {snap?.paused ? '' : ''}{snap ? dur(Math.max(0, clock.secs + (snap.paused ? 0 : (nowTick - clock.at) / 1000))) : '0:00'}
          {#if snap?.paused}<img class="frost" src={art('frozen_icon')} alt="" />{/if}
        </div>
        <div class="sub">{snap?.paused ? 'paused — click to carry on' : charSub}</div>
      </button>
      <div class="card" style:border-image-source="url({art('chip_dark')})">
        <div class="label">Gold</div>
        <div class="value c-gold">{fmt(snap?.gold?.earned)}</div>
        <div class="sub" title={snap?.carried_bank ? 'the balance the last run ended on — the game has not sent a new one yet' : 'bank balance as the game last reported it'}>
          {fmt(snap?.gold?.per_hour)}/h · bank {fmt(snap?.gold?.total)}{snap?.carried_bank ? ' *' : ''}
        </div>
      </div>
      <div class="card" style:border-image-source="url({art('chip_dark')})">
        <div class="label">XP</div>
        <div class="value c-xp">{fmt(snap?.xp?.earned)}</div>
        <div class="sub" title="the big number is what this session earned; 'in level' is the game's own bar — the experience banked towards the next hero level">
          {fmt(snap?.xp?.per_hour)}/h · in level {fmt(snap?.xp?.total)}
        </div>
      </div>
      <div class="card" style:border-image-source="url({art('chip_dark')})">
        <div class="label">Kills</div>
        <div class="value c-her">{fmt(snap?.kills?.earned)}</div>
        <div class="sub" title={snap?.carried_totals ? 'the total the last run ended on — the game has not saved the character yet' : 'lifetime total as the game last saved it'}>
          {fmt(snap?.kills?.per_hour)}/h · total {fmt(snap?.kills?.total)}{snap?.carried_totals ? ' *' : ''}
        </div>
      </div>
    </div>

    {#if lag}
      <div class="lag" data-tauri-drag-region>{lag}</div>
    {/if}

    <div class="workspace">
      <div class="summary-col">
        <div class="box" style:border-image-source="url({art('chip_dark')})">
          <div class="box-head"><span class="accent">Loot</span><span class="right">this session</span></div>
          <div class="rows">
            <div class="row colhead">
              <span class="rowname"></span>
              <span class="rowval">drops</span>
              <!-- The game's own claim, not ours: it flags the drop as owed to
                   magic find and we only count what it flagged. -->
              <span class="rowmf" title="Of those, the ones the game itself credited to Magic Find">mf</span>
              <span class="rowrate">per hour</span>
            </div>
            {#each RARITIES as name}
              {@const it = item(name)}
              <div class="row">
                <span class="rowname {RARITY_CLASS[name]}">{name}</span>
                <span class="rowval {RARITY_CLASS[name]}">{fmt(it.total)}</span>
                <span class="rowmf c-blue" title="credited to Magic Find by the game">{it.mf ? fmt(it.mf) : '—'}</span>
                <span class="dim rowrate">{fmt(it.per_hour)}/h</span>
              </div>
            {/each}
          </div>
        </div>

        {#if bosses.length || chests.length}
          <div class="box" style:border-image-source="url({art('chip_dark')})">
            <div class="box-head"><span class="accent">Killed &amp; opened</span><span class="right">this session</span></div>
            {#if bosses.length}
              <div class="subhead">Bosses</div>
              <div class="tally">
                {#each bosses as b}
                  <div class="tallyrow"><span class="dim">{b.label}</span><b class="c-sat">{fmt(b.total)}</b></div>
                {/each}
              </div>
            {/if}
            {#if chests.length}
              <div class="subhead">Chests</div>
              <div class="tally">
                {#each chests as c}
                  <div class="tallyrow"><span class="dim">{c.label}</span><b class="c-gold">{fmt(c.total)}</b></div>
                {/each}
              </div>
            {/if}
          </div>
        {/if}
      </div>

      <div class="box timeline" style:border-image-source="url({art('chip_dark')})">
        <div class="box-head timeline-head">
          <div class="timeline-heading">
            <span class="accent">Item timeline</span>
            {#if added}<span class="added" title={added}>{added}</span>{/if}
          </div>
          <span class="right">{extra?.drops?.length ?? 0} drops</span>
        </div>
        <div class="list">
          {#each extra?.drops ?? [] as d, index}
            {@const label = dropLabel(d)}
            {@const rarity = dropRarity(d)}
            {@const tier = tierLabel(d.tier)}
            {@const dropKey = `${d.ts_ms}:${index}`}
            <div class="drop">
              <span class="ts">{time(d.ts_ms)}</span>
              <span class="rar {rarityCls[rarity] ?? ''}">{rarity}</span>
              <span class="name {rarityCls[rarity] ?? ''}" title={label}>{label}</span>
              <div class="drop-meta">
                {#if tier}<span class="badge tier" title="Tier {tier}">{tier}</span>{/if}
                {#if d.mf}<span class="badge mf" title="Credited to Magic Find">MF</span>{/if}
                {#if lists.length && label}
                  <button
                    class="tolist"
                    title="Add {label} to a sound list"
                    aria-label="Add {label} to a sound list"
                    onclick={() => (adding = adding === dropKey ? null : dropKey)}
                  >+</button>
                  {#if adding === dropKey}
                    <!-- Keep the row visible when this menu opens near the end
                         of the journal's own scroller. -->
                    <div class="picker" use:reveal>
                      {#each lists as list}
                        <button onclick={() => addTo(list, label)}>{list.name}</button>
                      {/each}
                    </div>
                  {/if}
                {/if}
              </div>
            </div>
          {:else}
            <div class="dim empty">nothing yet — valuable drops land here</div>
          {/each}
        </div>
      </div>

      <div class="zone-col">
      <div class="box" style:border-image-source="url({art('chip_dark')})">
        <div class="box-head"><span class="accent">Zone</span><span class="right">{roomSub}</span></div>
        <div class="zone-title" title={room ?? ''}>{roomTitle}</div>
        <div class="vitals">
          <span>Level <b>{extra?.character?.level ?? '—'}</b></span>
          <span>Hero <b>{extra?.character?.herolevel ?? '—'}</b></span>
        </div>
      </div>

      <div class="box" style:border-image-source="url({art('chip_dark')})">
        <div class="box-head"><span class="accent c-sat">Satanic zone</span><span class="right">{szAgo}</span></div>
        {#if sz}
          <div class="zone-title" class:here={szHere}>{zoneName(sz.zone)}{#if szHere}<span class="badge here">you are here</span>{/if}</div>
          <div class="mods">
            <div class="mods-col">
              <div class="subhead c-set">Pros</div>
              {#each sz.buffs ?? [] as id}
                {@const b = buffInfo(id)}
                <div class="mod" title={b.desc}><img src={b.icon} alt="" /><div><div class="mod-name">{b.name}</div><div class="mod-desc">{b.desc}</div></div></div>
              {/each}
            </div>
            <div class="mods-col">
              <div class="subhead c-sat">Cons</div>
              {#each sz.debuffs ?? [] as id}
                {@const d = debuffInfo(id)}
                <div class="mod" title={d.desc}><div><div class="mod-name">{d.name}</div><div class="mod-desc">{d.desc}</div></div></div>
              {/each}
            </div>
          </div>
        {:else}
          <div class="dim small">Known once the game reports it — enter a zone with the live sensor installed.</div>
        {/if}
      </div>

      <div class="box" style:border-image-source="url({art('chip_dark')})">
        <div class="box-head"><span class="accent">{area.from === 'satanic' ? 'Drops in the satanic zone' : 'Drops in this area'}</span><span class="right">{area.label}</span></div>
        {#if area.items.length}
          <div class="area">
            {#each area.items as d}
              <div class="area-row">
                <span class="area-name {rarityCls[d.rarity] ?? ''}" title={d.name}>{cap(d.name)}</span>
                <span class="area-tier">{tierName(d.tier)}</span>
                <span class="area-odds">{d.chase ? `1/${fmt(d.chase)}` : ''}</span>
              </div>
            {/each}
          </div>
        {:else}
          <div class="dim small">{room ? 'The item tables tie nothing to this room. Zones, dungeons and boss dungeons have lists.' : 'Waiting for the room.'}</div>
        {/if}
      </div>
      </div>
    </div>
  </div>
</div>

<style>
  .zone-col {
    min-width: 0;
    min-height: 0;
    overflow-y: auto;
    display: flex;
    flex-direction: column;
    gap: 6px;
    padding-right: 2px;
  }
  .zone-col::-webkit-scrollbar { width: 6px; }
  .zone-col::-webkit-scrollbar-thumb { background: var(--dim-1); border-radius: 3px; }
  .zone-title { font-size: 15px; color: var(--bone-13); margin: 2px 0 4px; display: flex; align-items: center; gap: 6px; flex-wrap: wrap; }
  .zone-title.here { color: #ffd1d8; }
  .badge.here { font-size: 9px; background: #5a1622; color: #ffd1d8; border-radius: 3px; padding: 1px 5px; text-transform: uppercase; letter-spacing: .4px; }
  .vitals { display: flex; gap: 10px; font-size: 11px; color: var(--bone-4); flex-wrap: wrap; }
  .vitals b { color: var(--bone-13); }
  .mods { display: grid; grid-template-columns: 1fr 1fr; gap: 6px; }
  .mods-col { min-width: 0; }
  .mod { display: flex; gap: 5px; align-items: flex-start; margin: 2px 0 4px; min-width: 0; }
  .mod img { width: 18px; height: 18px; flex: none; margin-top: 1px; }
  .mod-name { font-size: 11px; color: var(--bone-13); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .mod-desc { font-size: 9.5px; color: var(--bone-4); line-height: 1.2; display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical; overflow: hidden; }
  .area { display: flex; flex-direction: column; gap: 1px; }
  .area-row { display: grid; grid-template-columns: minmax(0, 1fr) 24px 64px; gap: 6px; align-items: baseline; font-size: 11px; }
  .area-name { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .area-tier { color: var(--bone-4); font-size: 10px; text-align: center; }
  .area-odds { color: var(--bone-4); text-align: right; font-variant-numeric: tabular-nums; }
  .small { font-size: 10px; }
  .c-blue { color: #72b8ff; }

  @font-face {
    font-family: 'Offline Tracker UI';
    src: local('Segoe UI Semibold'), local('Segoe UI');
    font-weight: 700;
  }

  :global(html, body) {
    margin: 0;
    height: 100%;
    background: transparent;
    overflow: hidden;
    user-select: none;
    -webkit-user-select: none;
    cursor: default;
  }

  :global(#app) { height: 100%; }
  :global(img) { image-rendering: pixelated; }

  .panel {
    position: relative;
    box-sizing: border-box;
    width: 100%;
    height: 100%;
    display: flex;
    flex-direction: column;
    gap: 6px;
    font-family: 'Offline Tracker UI', sans-serif;
    font-size: 12px;
    color: var(--bone-6);
  }

  .card {
    box-sizing: border-box;
    flex: 1;
    min-width: 0;
    overflow: hidden;
    border: 6px solid transparent;
    border-image-slice: 6 fill;
    border-image-width: 6px;
    image-rendering: pixelated;
    padding: 2px 8px 4px;
  }

  .label {
    font-size: 10px;
    text-transform: uppercase;
    letter-spacing: 0.5px;
    color: var(--bone-4);
  }
  .value { font-size: 19px; line-height: 22px; }

  .sub {
    font-size: 10px;
    color: var(--edge-8);
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
  }
  .box {
    box-sizing: border-box;
    flex: none;
    border: 6px solid transparent;
    border-image-slice: 6 fill;
    border-image-width: 6px;
    image-rendering: pixelated;
    padding: 4px 8px 6px;
    display: flex;
    flex-direction: column;
    min-height: 0;
  }

  .lag {
    flex: none;
    font-size: 10px;
    color: var(--dim-2);
    text-align: center;
    margin-top: -2px;
  }

  .timeline-heading {
    min-width: 0;
    display: flex;
    align-items: center;
    gap: 8px;
  }
  .added {
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    color: #45c15a;
    font-size: 10px;
    text-transform: none;
    letter-spacing: 0;
  }

  .tolist {
    flex: none;
    font: inherit;
    width: 22px;
    height: 20px;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    padding: 0 0 1px;
    border: 1px solid rgba(155, 132, 112, 0.28);
    border-radius: 4px;
    background: rgba(255, 255, 255, 0.035);
    color: var(--bone-5);
    font-size: 14px;
    line-height: 1;
    cursor: pointer;
  }
  .tolist:hover {
    color: var(--bone-13);
    border-color: var(--edge-4);
    background: rgba(202, 69, 69, 0.14);
  }

  .picker {
    position: absolute;
    right: 4px;
    top: 100%;
    z-index: 8;
    display: flex;
    flex-direction: column;
    min-width: 148px;
    max-height: 180px;
    overflow-y: auto;
    background: var(--ground-5);
    border: 1px solid var(--edge-2);
    border-radius: 4px;
    padding: 2px;
    box-shadow: 0 8px 20px rgba(0, 0, 0, 0.46);
  }
  .picker button {
    font: inherit;
    font-size: 11px;
    color: var(--bone-6);
    background: none;
    border: none;
    text-align: left;
    padding: 3px 8px;
    cursor: pointer;
  }
  .picker button:hover { background: rgba(150, 37, 56, 0.55); color: var(--bone-13); }

  .body {
    flex: 1 1 auto;
    min-height: 0;
    overflow: hidden;
    display: flex;
    flex-direction: column;
    gap: 6px;
    container-name: stats;
    container-type: inline-size;
  }

  /* An `fr` track's floor is its item's min-content, and `.clock` — unlike the
     three cards beside it — had no `min-width: 0`. `.clock .sub` is the whole
     character line and does not wrap, so the length of the character's name
     set the width of Gold, XP and Kills: an 18-character name took 275px of a
     171px track and pushed the three figures out over their own frames. */
  .run {
    flex: none;
    display: grid;
    grid-template-columns: minmax(0, 1.4fr) repeat(3, minmax(0, 1fr));
    gap: 6px;
  }

  .clock {
    box-sizing: border-box;
    min-width: 0;
    overflow: hidden;
    border: 6px solid transparent;
    border-image-slice: 6 fill;
    border-image-width: 6px;
    image-rendering: pixelated;
    padding: 6px 8px;
    display: flex;
    flex-direction: column;
    justify-content: center;
  }
  .clock {
    /* it is a button now, but it is still the same tile */
    font: inherit;
    text-align: left;
    cursor: pointer;
    background: none;
  }
  .clock .value { font-size: 20px; color: var(--bone-13); display: flex; align-items: center; gap: 6px; }
  .clock:hover .sub { color: var(--bone-8); }
  /* held: the game's own ice, on the tile whose clock has stopped */
  .clock.held .value { color: #bfe4ff; }
  .clock.held .sub { color: #7fa8c4; }
  .clock .frost { width: 16px; height: 16px; image-rendering: pixelated; }

  .workspace {
    flex: 1 1 auto;
    min-height: 0;
    display: grid;
    grid-template-columns: minmax(250px, 300px) minmax(0, 1fr) minmax(250px, 300px);
    gap: 8px;
  }

  .summary-col {
    min-width: 0;
    min-height: 0;
    overflow-y: auto;
    display: flex;
    flex-direction: column;
    gap: 6px;
    padding-right: 2px;
  }
  .summary-col::-webkit-scrollbar,
  .workspace::-webkit-scrollbar { width: 6px; }
  .summary-col::-webkit-scrollbar-thumb,
  .workspace::-webkit-scrollbar-thumb { background: var(--dim-1); border-radius: 3px; }

  .timeline {
    flex: 1 1 auto;
    min-width: 0;
    min-height: 0;
    height: 100%;
    padding: 6px 8px 8px;
  }
  .timeline-head {
    min-height: 20px;
    align-items: center;
    margin-bottom: 5px;
  }

  @container stats (max-width: 700px) {
    .run { grid-template-columns: repeat(2, minmax(0, 1fr)); }
    .workspace {
      grid-template-columns: minmax(0, 1fr);
      grid-template-rows: auto 320px auto;
      overflow-y: auto;
      padding-right: 2px;
    }
    .zone-col { min-height: auto; overflow: visible; padding-right: 0; }
    .summary-col {
      min-height: auto;
      overflow: visible;
      padding-right: 0;
    }
    .timeline { height: 320px; }
  }

  .rows { display: flex; flex-direction: column; gap: 1px; }
  .rows .row {
    display: flex;
    align-items: baseline;
    gap: 8px;
    padding: 3px 4px;
    background: rgba(0, 0, 0, 0.2);
  }
  .rows .row:nth-child(even) { background: rgba(0, 0, 0, 0.1); }
  .rowname { flex: 1 1 auto; min-width: 0; }
  .rowval { min-width: 44px; text-align: right; font-size: 13px; }
  /* narrow on purpose: it is a footnote to the count beside it, not a column
     anyone reads down */
  .rowmf { min-width: 32px; text-align: right; font-size: 10px; }
  .rowrate { min-width: 54px; text-align: right; font-size: 10px; }

  /* the numbers on their own said nothing; the header says what they are */
  .rows .row.colhead {
    background: none;
    color: var(--edge-2b);
    font-size: 9px;
    letter-spacing: 0.4px;
    text-transform: uppercase;
    padding-bottom: 1px;
  }
  .row.colhead .rowval,
  .row.colhead .rowmf { font-size: 9px; }

  .subhead {
    color: var(--edge-2b);
    font-size: 9px;
    letter-spacing: 0.4px;
    text-transform: uppercase;
    padding: 6px 2px 2px;
  }

  /* counted things read as a table of values, not as buttons */
  .tally {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(128px, 1fr));
    column-gap: 14px;
  }
  .tallyrow {
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    gap: 8px;
    padding: 2px 2px 2px 0;
    border-bottom: 1px solid rgba(58, 43, 43, 0.7);
  }
  .tallyrow span { font-size: 11px; }
  .tallyrow b { font-size: 12px; color: var(--bone-6); }

  .box-head {
    flex: none;
    display: flex;
    justify-content: space-between;
    align-items: baseline;
    font-size: 10px;
    text-transform: uppercase;
    letter-spacing: 0.5px;
    color: var(--bone-4);
    margin-bottom: 3px;
  }
  .accent { color: #ca4545; }
  .right { color: var(--edge-8); text-transform: none; letter-spacing: 0; }

  .list {
    flex: 1 1 auto;
    min-height: 0;
    overflow-y: auto;
    display: flex;
    flex-direction: column;
    gap: 3px;
    padding-right: 2px;
  }
  .list::-webkit-scrollbar { width: 6px; }
  .list::-webkit-scrollbar-thumb { background: var(--dim-1); border-radius: 3px; }

  .drop {
    position: relative;
    display: grid;
    grid-template-columns: 58px 62px minmax(0, 1fr) auto;
    gap: 8px;
    align-items: center;
    min-height: 29px;
    padding: 3px 5px;
    border-left: 2px solid rgba(202, 69, 69, 0.22);
    border-bottom: 1px solid rgba(94, 72, 68, 0.34);
    background: rgba(0, 0, 0, 0.2);
    white-space: nowrap;
    flex: none;
  }
  .drop:nth-child(even) { background: rgba(255, 255, 255, 0.018); }
  .drop:hover {
    border-left-color: rgba(202, 69, 69, 0.72);
    background: rgba(202, 69, 69, 0.08);
  }
  .ts {
    color: var(--edge-6);
    font-size: 10px;
    font-variant-numeric: tabular-nums;
  }
  .rar {
    overflow: hidden;
    text-overflow: ellipsis;
    font-size: 9px;
    letter-spacing: 0.35px;
    text-transform: uppercase;
  }
  .name {
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    font-size: 12px;
  }
  .drop-meta {
    min-width: 0;
    display: flex;
    align-items: center;
    justify-content: flex-end;
    gap: 5px;
  }
  .badge {
    box-sizing: border-box;
    min-width: 22px;
    height: 17px;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    padding: 0 5px;
    border: 1px solid rgba(161, 143, 125, 0.22);
    border-radius: 999px;
    background: rgba(255, 255, 255, 0.035);
    color: var(--bone-8);
    font-size: 9px;
    line-height: 1;
  }
  .badge.mf {
    border-color: color-mix(in srgb, var(--mf) 34%, transparent);
    background: color-mix(in srgb, var(--mf) 8%, transparent);
    color: var(--mf);
  }
  .dim { color: var(--edge-8); font-size: 11px; }
  .empty {
    margin: auto 0;
    padding: 18px 0;
    text-align: center;
    width: 100%;
  }

  @container stats (max-width: 500px) {
    .drop { grid-template-columns: 54px minmax(0, 1fr) auto; gap: 6px; }
    .rar { display: none; }
  }

  .c-ang { color: #f6f794; }
  .c-her { color: #00ffae; }
  .c-sat { color: var(--rar-satanic); }
  .c-blue { color: var(--mf); }
  .c-myt { color: #c060e0; }
  .c-unh { color: #e04a7a; }
  .c-set { color: #40d040; }
  .c-ble { color: var(--bone-14); }
  .c-gold { color: var(--gold-2); }
  .c-xp { color: #a06ae0; }
</style>
